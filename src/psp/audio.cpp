/**
 * @file audio.cpp
 * @brief Music playback for the Sony PSP backend (pspaudio).
 *
 * The PSP Fat/Slim's 32-64MB of RAM can't hold the 47MB desktop pc_audio.wav
 * decoded to raw PCM the way the Switch backend's approach effectively can,
 * so this follows the GameCube/Wii U shape instead (src/ogc/audio.cpp,
 * src/wiiu/audio.cpp): the desktop WAV is transcoded to a small Vorbis OGG at
 * build time and #embedded into the EBOOT.PBP (see the psp branch of
 * CMakeLists.txt), decoded here with stb_vorbis.
 *
 * Unlike GameCube (which has no threads and polls from audio::Update(), once
 * per video frame), the PSP has real kernel threads and a blocking audio
 * output call, so this streams from a dedicated thread instead: it decodes
 * one chunk, hands it to sceAudioOutputBlocking() (which blocks until the
 * hardware has consumed the PREVIOUS chunk, so the loop is naturally paced --
 * no explicit buffer-ready polling needed), and repeats. That keeps playback
 * off the 60Hz render loop entirely, so Update() is a no-op, matching the
 * Switch/Wii U backends' shape rather than GameCube's.
 *
 * No SFX path: like the GameCube backend, this project currently registers no
 * SFX table for any non-NES target (see demo/src/tracks.cpp), and PSP's
 * situation is the same one that keeps GameCube's SfxPlay a no-op --
 * building a whole second decode/mix path for a currently-empty table isn't
 * worth it. Revisit alongside GameCube if/when a non-NES SFX table exists.
 */
#include "internal.hpp"
#include <pnes/audio.hpp>

#include <pspkernel.h>
#include <pspaudio.h>
#include <cstring>

#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include <stb_vorbis.c>

// The Vorbis-encoded music, baked into the EBOOT.PBP's ELF at build time.
// ffmpeg produces pc_audio.ogg (encoded straight to this backend's playback
// rate, see SAMPLE_RATE below) into the build tree and CMake puts that dir on
// the include path, so #embed finds it. psp-g++ (GCC 15) supports #embed, the
// same C23 mechanism the demo already uses for CHR art.
static const unsigned char ogg_data[] = {
#embed <pc_audio.ogg>   // resolved via --embed-dir (set in the psp CMake branch)
};
static constexpr int ogg_len = static_cast<int>(sizeof(ogg_data));

// sceAudioOutputBlocking's hardware DAC is fixed at 44.1kHz; the OGG is
// encoded to exactly that rate at build time so no resampling is needed here.
static constexpr int SAMPLE_RATE = 44100;
static constexpr int CHANNELS    = 2;

// One sceAudio hardware channel, reserved for music. Sample count must be a
// PSP_AUDIO_SAMPLE_ALIGN(64) multiple in [PSP_AUDIO_SAMPLE_MIN,
// PSP_AUDIO_SAMPLE_MAX]; 2048 frames is ~46ms, small enough for responsive
// TrackPlay/TrackStop, large enough that the decode thread isn't spinning.
static constexpr int CHANNEL = 0;
static constexpr int CHUNK_FRAMES = 2048;

// audio.hpp declares these for the SDL3 mixer; this backend streams straight
// into sceAudio chunk buffers instead, but define them so the externs resolve.
float *audio::pcm_buffer = nullptr;
u32 audio::pcm_buffer_size = 0;

// Every non-NES backend leaves ::sfxs/::nSfx undefined unless the app calls
// ::SFX(...); this weak zero-entry default keeps the link working until it
// does (mirrors the SDL3/Switch/Wii U backends), even though this backend
// doesn't act on it (see the file header).
__attribute__((weak)) extern const audio::sfx_t sfxs[1] = {};
__attribute__((weak)) extern const u8 nSfx = 0;

namespace {

// Ping-ponged chunk buffers: audio_thread decodes into one half while
// sceAudioOutputBlocking() drains the other (that call blocks until the
// hardware finishes the buffer it was previously handed, so alternating
// halves here is exactly the double-buffering that call's contract expects).
i16 chunk[2][CHUNK_FRAMES * CHANNELS];

stb_vorbis *vorbis     = nullptr;
u32         loop_frame = 0;      // stb_vorbis_seek() target on EOF
bool        playing    = false;
bool        paused     = false;

SceUID      athread;
bool        running = false;

// TrackPlay() runs on the main thread while audio_thread() is concurrently
// mid-decode on `vorbis` -- stb_vorbis's decoder object is not safe to call
// into from two threads at once (a seek landing mid stb_vorbis_get_samples_
// short_interleaved() call can corrupt its internal state, after which every
// decode call returns 0 samples forever and fill() below zero-fills every
// chunk from then on: permanent silence with no error, no crash). See
// src/ps2/audio.cpp's own copy of this comment, where this was first caught.
//
// So TrackPlay() never touches `vorbis` itself: it only posts the requested
// index here, and audio_thread() -- the only thread allowed to call any
// stb_vorbis_* function -- applies it at the top of its own loop. -1 means
// no request pending.
//
// Plain `volatile int`, not std::atomic: the PSP's Allegrex core (like the
// PS2's EE -- see src/ps2/audio.cpp's own copy of this comment) has no
// hardware word-CAS, so std::atomic<int>::exchange needs libatomic, which
// isn't wired into this toolchain's link either. A naturally-aligned int is
// already indivisible on this single-core CPU -- `volatile` only stops the
// compiler caching either side's read/write in a register -- and with
// exactly one writer (TrackPlay) and one reader (audio_thread, the only one
// that ever clears it back to -1), there's no partial-update to race on.
volatile int pending_track = -1;

// Fills chunk[which] with CHUNK_FRAMES stereo frames, looping back to
// loop_frame on EOF (mirrors src/ogc/audio.cpp's fill()).
void fill(const int which) {
    i16 *dst = chunk[which];
    int got = 0, stalls = 0;
    while (got < CHUNK_FRAMES && stalls < 4) {
        const int n = stb_vorbis_get_samples_short_interleaved(
            vorbis, CHANNELS, dst + got * CHANNELS, (CHUNK_FRAMES - got) * CHANNELS);
        if (n == 0) {
            stb_vorbis_seek(vorbis, loop_frame);
            stalls++;
            continue;
        }
        got += n;
        stalls = 0;
    }
    if (got < CHUNK_FRAMES) {
        memset(dst + got * CHANNELS, 0,
               static_cast<size_t>(CHUNK_FRAMES - got) * CHANNELS * sizeof(i16));
    }
}

int audio_thread(SceSize, void *) {
    int next = 0;
    while (running) {
        // Apply any TrackPlay() request posted from the main thread -- see
        // ::pending_track's own comment for why this, and not TrackPlay
        // itself, is what calls stb_vorbis_seek_start().
        const int req = pending_track;
        if (req >= 0) {
            pending_track = -1;
        }
        if (req >= 0 && vorbis) {
            stb_vorbis_seek_start(vorbis);
            loop_frame = tracks[req].loop_start > 0.0f
                ? static_cast<u32>(tracks[req].loop_start * static_cast<float>(SAMPLE_RATE))
                : 0;
            playing = true;
            paused  = false;
        }

        if (!vorbis || !playing || paused) {
            memset(chunk[next], 0, sizeof(chunk[next]));
        } else {
            fill(next);
        }
        // Blocks until the PREVIOUS sceAudioOutputBlocking() call's buffer has
        // fully drained -- this call is what paces the whole thread at the
        // hardware's real playback rate, no sleep/poll needed.
        sceAudioOutputBlocking(CHANNEL, PSP_AUDIO_VOLUME_MAX, chunk[next]);
        next ^= 1;
    }
    return 0;
}

} // namespace

void audio::Init(u8) {
    int err = 0;
    vorbis = stb_vorbis_open_memory(ogg_data, ogg_len, &err, nullptr);
    if (!vorbis) return;

    if (nTracks > 0) {
        loop_frame = tracks[0].loop_start > 0.0f
            ? static_cast<u32>(tracks[0].loop_start * static_cast<float>(SAMPLE_RATE))
            : 0;
    }

    if (sceAudioChReserve(CHANNEL, CHUNK_FRAMES, PSP_AUDIO_FORMAT_STEREO) < 0) return;

    running = true;
    athread = sceKernelCreateThread("audio_thread", audio_thread, 0x12, 0x10000, 0, nullptr);
    if (athread >= 0) sceKernelStartThread(athread, 0, nullptr);
}

void audio::Update() {
    // Streaming is driven by the dedicated audio thread; nothing to do per frame.
}

void audio::TrackPlay(const u8 index) {
    // Does NOT touch `vorbis` or `loop_frame`/`playing`/`paused` directly --
    // only audio_thread() may call into the decoder; see ::pending_track.
    if (!vorbis || index >= nTracks) return;
    pending_track = index;
}

void audio::TrackStop() {
    playing = false;
}

void audio::TrackPause(const u8 pause) {
    (void)pause;  // match the SDL/Switch/Wii U backends: toggle regardless of argument
    paused = !paused;
}

// See the file header: no non-NES SFX table exists in this project yet.
void audio::SfxPlay(u8 /*index*/, u8 /*channel*/) {}
void audio::SfxSamplePlay(u8 /*index*/) {}
