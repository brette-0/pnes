/**
 * @file audio.cpp
 * @brief Music playback for the Nintendo Switch backend (libnx audout).
 *
 * Mirrors the desktop audio contract (audio.hpp): the demo registers tracks in
 * its track table and calls Init / TrackPlay / Update. libnx's audout
 * device is fixed at 48 kHz / 2ch / signed-16 PCM, so the build pipeline encodes
 * the source WAV to exactly that format (see the ffmpeg step in the Switch
 * branch of CMakeLists.txt) and ships it in the .nro's RomFS. This backend reads
 * that raw PCM into RAM once and streams it.
 *
 * Streaming runs on a dedicated thread that blocks on audout's
 * released-buffer signal, refills the freed chunk from the track (looping at the
 * track's loop point), and re-appends it. Keeping playback off the render thread
 * means audio is never coupled to (and never stalls) the 60Hz frame loop --
 * Update() is therefore a no-op here.
 *
 * SFX (declared via ::SFX in audio.hpp, same RomFS raw-PCM pipeline as
 * tracks) are loaded once into their own buffer and played on one of
 * ::N_SFX_VOICES fixed voice slots; fill_chunk() additively mixes every
 * active voice into the same chunk the track streams from, same as any two
 * PCM sources ever mix -- summed, sample by sample, with clamping. Starting
 * a new SFX on a channel that's still playing cuts the old one off.
 */
#include "internal.hpp"
#include <pnes/audio.hpp>

#include <switch.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// audout native format: 48000 Hz, 2 channels, PCM signed 16-bit.
static constexpr u32 SAMPLE_RATE     = 48000;
static constexpr u32 BYTES_PER_FRAME = 2 /*ch*/ * sizeof(s16);
static constexpr u32 BYTES_PER_SEC   = SAMPLE_RATE * BYTES_PER_FRAME;  // 192000

// Triple-buffered ~85ms chunks. Each must be 0x1000-aligned with a 0x1000-
// multiple size (audout requirement); 0x4000 satisfies both.
static constexpr u32 CHUNK   = 0x4000;
static constexpr int N_BUF   = 3;

// audio.hpp declares these for the desktop mixer; the Switch streams via audout
// instead, but define them so the externs resolve if anything references them.
float *audio::pcm_buffer = nullptr;
u32 audio::pcm_buffer_size = 0;

static AudioOutBuffer buffers[N_BUF];

static u8 *track_data = nullptr;   // whole track, 48k/2ch/s16 PCM
static u32 track_len  = 0;         // bytes
static u32 loop_point = 0;         // byte offset to wrap to at end

static Mutex   mtx;
static Thread  athread;
static bool    running   = false;
static bool    playing   = false;
static bool    paused    = false;
static u32     play_pos  = 0;      // current read offset into track_data

// Every non-NES backend leaves ::sfxs/::nSfx undefined unless the app calls
// ::SFX(...); this weak zero-entry default keeps the link working until it
// does (mirrors the SDL3 desktop backend).
__attribute__((weak)) extern const audio::sfx_t sfxs[1] = {};
__attribute__((weak)) extern const u8 nSfx = 0;

// One-shot SFX PCM: same 48k/2ch/s16 RomFS raw format as tracks, loaded
// once at init and concatenated into its own buffer (mtx also guards this
// state -- audio_thread reads it every chunk while the game thread calls
// SfxPlay).
struct sfx_runtime_t { u32 offset; u32 length; };   // byte offset/length into sfx_data
static sfx_runtime_t sfx_info[256];
static u8            *sfx_data = nullptr;

static constexpr int N_SFX_VOICES = 4;
struct sfx_voice_t { u32 pos; u32 end; bool active; };
static sfx_voice_t sfx_voices[N_SFX_VOICES];

// Translate "tracks/foo.wav" -> "romfs:/tracks/foo.raw" (the build re-encodes
// the desktop WAV to raw 48k PCM and bakes it into the .nro RomFS).
static void romfs_raw_path(const char *fp, char *out, size_t cap) {
    snprintf(out, cap, "romfs:/%s", fp);
    char *dot = strrchr(out, '.');
    if (dot && (cap - (dot - out)) >= 5) memcpy(dot, ".raw", 5);  // incl. NUL
}

static void load_track(u8 index) {
    if (index >= nTracks) return;

    char path[300];
    romfs_raw_path(tracks[index].fp, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > 0) {
        track_data = static_cast<u8 *>(malloc(static_cast<size_t>(sz)));
        if (track_data && fread(track_data, 1, static_cast<size_t>(sz), f) == static_cast<size_t>(sz)) {
            track_len = static_cast<u32>(sz) & ~(BYTES_PER_FRAME - 1);  // frame-align
        } else {
            free(track_data);
            track_data = nullptr;
        }
    }
    fclose(f);

    // loop_start is in seconds (0 = loop the whole track); convert to a
    // frame-aligned byte offset.
    u32 lp = static_cast<u32>(tracks[index].loop_start * BYTES_PER_SEC);
    lp &= ~(BYTES_PER_FRAME - 1);
    loop_point = (track_len && lp < track_len) ? lp : 0;
}

// Load and concatenate every declared SFX (mirrors load_track's shape) into
// one raw-PCM buffer, filling sfx_info with each entry's byte offset/length.
static void load_sfx() {
    u32 total = 0;
    for (u8 i = 0; i < nSfx; i++) {
        char path[300];
        romfs_raw_path(sfxs[i].fp, path, sizeof(path));
        FILE *f = fopen(path, "rb");
        if (!f) { sfx_info[i] = {0, 0}; continue; }
        fseek(f, 0, SEEK_END);
        const long sz = ftell(f);
        fclose(f);
        const u32 len = sz > 0 ? static_cast<u32>(sz) & ~(BYTES_PER_FRAME - 1) : 0;
        sfx_info[i].offset = total;
        sfx_info[i].length = len;
        total += len;
    }
    if (total == 0) return;

    sfx_data = static_cast<u8 *>(malloc(total));
    if (!sfx_data) return;

    for (u8 i = 0; i < nSfx; i++) {
        if (sfx_info[i].length == 0) continue;
        char path[300];
        romfs_raw_path(sfxs[i].fp, path, sizeof(path));
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fread(sfx_data + sfx_info[i].offset, 1, sfx_info[i].length, f);
        fclose(f);
    }
}

void audio::SfxPlay(const u8 index, const u8 channel) {
    if (index >= nSfx || channel >= N_SFX_VOICES || sfx_info[index].length == 0) return;
    mutexLock(&mtx);
    sfx_voices[channel] = {
        .pos    = sfx_info[index].offset,
        .end    = sfx_info[index].offset + sfx_info[index].length,
        .active = true
    };
    mutexUnlock(&mtx);
}

void audio::SfxSamplePlay(const u8 index) { audio::SfxPlay(index, 1); }

// Fill one chunk from the track (or silence when stopped/paused/absent),
// then additively mix in every active SFX voice (s16, clamped).
static void fill_chunk(u8 *dst) {
    mutexLock(&mtx);
    if (!playing || paused || track_len == 0) {
        memset(dst, 0, CHUNK);
    } else {
        u32 written = 0;
        while (written < CHUNK) {
            u32 avail = track_len - play_pos;
            u32 n = CHUNK - written;
            if (n > avail) n = avail;
            memcpy(dst + written, track_data + play_pos, n);
            written  += n;
            play_pos += n;
            if (play_pos >= track_len) play_pos = loop_point;
        }
    }

    i16 *out = reinterpret_cast<i16 *>(dst);
    for (auto &v : sfx_voices) {
        if (!v.active) continue;
        u32 avail = v.end - v.pos;
        u32 n = CHUNK < avail ? CHUNK : avail;
        const i16 *src = reinterpret_cast<const i16 *>(sfx_data + v.pos);
        for (u32 i = 0; i < n / sizeof(i16); i++) {
            const int mixed = static_cast<int>(out[i]) + static_cast<int>(src[i]);
            out[i] = static_cast<i16>(mixed < -32768 ? -32768 : (mixed > 32767 ? 32767 : mixed));
        }
        v.pos += n;
        if (v.pos >= v.end) v.active = false;
    }
    mutexUnlock(&mtx);
}

static void audio_thread(void *) {
    while (running) {
        AudioOutBuffer *released = nullptr;
        u32 count = 0;
        if (R_FAILED(audoutGetReleasedAudioOutBuffer(&released, &count)) || count == 0) {
            svcSleepThread(1'000'000);  // 1ms guard if it returned without a buffer
            continue;
        }
        fill_chunk(static_cast<u8 *>(released->buffer));
        released->data_size = CHUNK;
        audoutAppendAudioOutBuffer(released);
    }
}

void audio::Init(u8) {
    romfsInit();
    load_track(0);
    load_sfx();

    if (R_FAILED(audoutInitialize())) return;
    audoutStartAudioOut();

    for (auto &b : buffers) {
        void *mem = aligned_alloc(0x1000, CHUNK);
        memset(mem, 0, CHUNK);
        b.next        = nullptr;
        b.buffer      = mem;
        b.buffer_size = CHUNK;
        b.data_size   = CHUNK;
        b.data_offset = 0;
        audoutAppendAudioOutBuffer(&b);
    }

    mutexInit(&mtx);
    running = true;
    // Higher priority than the main thread (0x2C) so the mixer stays fed; default core.
    if (R_SUCCEEDED(threadCreate(&athread, audio_thread, nullptr, nullptr, 0x10000, 0x20, -2)))
        threadStart(&athread);
}

void audio::Update() {
    // Streaming is driven by the audout thread; nothing to do per frame.
}

void audio::TrackPlay(const u8 index) {
    if (index >= nTracks) return;
    mutexLock(&mtx);
    play_pos = 0;
    playing  = true;
    paused   = false;
    mutexUnlock(&mtx);
}

void audio::TrackStop() {
    mutexLock(&mtx);
    playing = false;
    mutexUnlock(&mtx);
}

void audio::TrackPause(const u8 pause) {
    (void)pause;  // match the SDL backend: toggle regardless of argument
    mutexLock(&mtx);
    paused = !paused;
    mutexUnlock(&mtx);
}
