/**
 * @file audio.cpp
 * @brief Music playback for the native Wii U backend (SDL2 audio).
 *
 * Mirrors the desktop audio contract (audio.hpp): the demo registers tracks in
 * its track table and calls Init / TrackPlay / Update.
 *
 * The music is NOT loaded from the filesystem. A native Wii U title only has a
 * /vol/content mount when it is launched with its packaged content (a .wuhb or
 * an unpacked code/content/meta layout); a bare .rpx (e.g. dropped straight into
 * Cemu) has none, and SDL_LoadWAV would silently fail. To play correctly however
 * the title is launched, the build re-encodes the desktop WAV to a small Vorbis
 * OGG (see the wiiu branch of CMakeLists.txt) and #embeds it into the .rpx; this
 * file decodes it with stb_vorbis at startup. This mirrors the web backend's
 * proven OGG path and also shrinks the artifact (the 46MB WAV would otherwise
 * have to ride along uncompressed).
 *
 * Once decoded, playback is identical to the other streaming backends: the OGG
 * is converted once to the AX device format (48k/2ch/s16; SDL handles the rate
 * change and the little-endian -> Espresso big-endian byte swap) and streamed
 * from an SDL audio callback, looping at the track's loop point. The callback
 * runs on SDL's own audio thread, independent of the 60Hz render loop, so
 * Update() is a no-op.
 *
 * SFX (declared via ::SFX in audio.hpp) are decoded once at init and mixed
 * additively into the same callback, on one of ::N_SFX_VOICES fixed voice
 * slots -- starting a new effect on a busy channel cuts the old one off.
 * Unlike the music track, SFX are NOT #embedded: an app can declare an
 * arbitrary, unbounded SFX table, and #embed needs a literal path known at
 * this file's own compile time, so there's no way to bake an app-defined
 * list into the binary here. Instead each entry's `fp` is looked up at
 * runtime under /vol/content (Vorbis-encoded, same as the embedded music),
 * which requires the title to be launched from its packaged content (.wuhb,
 * or an unpacked code/content/meta layout) -- a bare .rpx dropped straight
 * into Cemu has no /vol/content mount and will simply play no SFX, same as
 * a missing file does today for TrackPlay.
 */
#include "internal.hpp"
#include <pnes/audio.hpp>

#include <SDL2/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// stb_vorbis decodes the embedded OGG to S16 PCM. Header+impl in one TU; pulled
// in only here (the include dir is added by the wiiu branch of CMakeLists.txt).
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include <stb_vorbis.c>

// The Vorbis-encoded music, baked into the .rpx at build time. ffmpeg produces
// pc_audio.ogg (48k/2ch) into the build tree and CMake puts that dir on the
// include path, so #embed finds it. GCC 15+ (devkitPPC) supports #embed, the
// same C23 mechanism the demo already uses for CHR art.
static const unsigned char ogg_data[] = {
#embed <pc_audio.ogg>   // resolved via --embed-dir (set in the wiiu CMake branch)
};
static constexpr int ogg_len = static_cast<int>(sizeof(ogg_data));

// Device format: 48 kHz, 2ch, signed-16 PCM (the Wii U AX renderer's native
// rate; SDL converts the decoded OGG into this when we build the track buffer).
static constexpr int SAMPLE_RATE     = 48000;
static constexpr int BYTES_PER_FRAME = 2 /*ch*/ * static_cast<int>(sizeof(i16));
static constexpr int BYTES_PER_SEC   = SAMPLE_RATE * BYTES_PER_FRAME;  // 192000

// audio.hpp declares these for the desktop mixer; the Wii U streams via an SDL
// callback instead, but define them so the externs resolve if anything refers
// to them.
float *audio::pcm_buffer = nullptr;
u32 audio::pcm_buffer_size = 0;

static SDL_AudioDeviceID dev = 0;

static u8 *track_data = nullptr;   // whole track, converted to device format
static u32 track_len  = 0;         // bytes
static u32 loop_point = 0;         // byte offset to wrap to at end
static u32 play_pos   = 0;         // current read offset into track_data

static bool playing = false;
static bool paused  = false;

// Every non-NES backend leaves ::sfxs/::nSfx undefined unless the app calls
// ::SFX(...); this weak zero-entry default keeps the link working until it
// does (mirrors the SDL3/Switch backends).
__attribute__((weak)) extern const audio::sfx_t sfxs[1] = {};
__attribute__((weak)) extern const u8 nSfx = 0;

// One-shot SFX PCM, decoded and converted to the same device format as the
// track and concatenated into its own buffer.
struct sfx_runtime_t { u32 offset; u32 length; };   // byte offset/length into sfx_data
static sfx_runtime_t sfx_info[256];
static u8            *sfx_data = nullptr;

static constexpr int N_SFX_VOICES = 4;
struct sfx_voice_t { u32 pos; u32 end; bool active; };
static sfx_voice_t sfx_voices[N_SFX_VOICES];

// Decode one /vol/content-relative Vorbis file into the device format,
// appending it to sfx_data. Mirrors load_track()'s decode+convert shape but
// reads from the filesystem (see the file-header note on why SFX can't use
// #embed) rather than a compiled-in byte array.
static void load_one_sfx(const u8 i, u32 *running_offset) {
    char path[300];
    snprintf(path, sizeof(path), "/vol/content/%s", sfxs[i].fp);
    char *dot = strrchr(path, '.');
    if (dot && (sizeof(path) - (dot - path)) >= 5) memcpy(dot, ".ogg", 5);

    FILE *f = fopen(path, "rb");
    if (!f) { sfx_info[i] = {*running_offset, 0}; return; }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); sfx_info[i] = {*running_offset, 0}; return; }

    auto *raw = static_cast<unsigned char *>(SDL_malloc(static_cast<size_t>(sz)));
    const bool ok = raw && fread(raw, 1, static_cast<size_t>(sz), f) == static_cast<size_t>(sz);
    fclose(f);
    if (!ok) { SDL_free(raw); sfx_info[i] = {*running_offset, 0}; return; }

    int channels = 0, rate = 0;
    short *decoded = nullptr;
    const int frames = stb_vorbis_decode_memory(raw, static_cast<int>(sz), &channels, &rate, &decoded);
    SDL_free(raw);
    if (frames < 0 || !decoded) { sfx_info[i] = {*running_offset, 0}; return; }
    const u32 pcm_bytes = static_cast<u32>(frames) * channels * sizeof(short);

    SDL_AudioCVT cvt;
    const int r = SDL_BuildAudioCVT(&cvt,
                                    AUDIO_S16SYS, static_cast<Uint8>(channels), rate,
                                    AUDIO_S16SYS, 2, SAMPLE_RATE);
    if (r < 0) { free(decoded); sfx_info[i] = {*running_offset, 0}; return; }

    u8 *converted;
    u32 converted_len;
    if (r == 0) {
        converted = static_cast<u8 *>(SDL_malloc(pcm_bytes));
        memcpy(converted, decoded, pcm_bytes);
        converted_len = pcm_bytes & ~(BYTES_PER_FRAME - 1);
        free(decoded);
    } else {
        cvt.len = static_cast<int>(pcm_bytes);
        cvt.buf = static_cast<u8 *>(SDL_malloc(static_cast<size_t>(cvt.len) * cvt.len_mult));
        memcpy(cvt.buf, decoded, pcm_bytes);
        free(decoded);
        if (!cvt.buf || SDL_ConvertAudio(&cvt) < 0) {
            SDL_free(cvt.buf);
            sfx_info[i] = {*running_offset, 0};
            return;
        }
        converted = cvt.buf;
        converted_len = static_cast<u32>(cvt.len_cvt) & ~(BYTES_PER_FRAME - 1);
    }

    sfx_data = static_cast<u8 *>(SDL_realloc(sfx_data, *running_offset + converted_len));
    memcpy(sfx_data + *running_offset, converted, converted_len);
    SDL_free(converted);

    sfx_info[i] = {*running_offset, converted_len};
    *running_offset += converted_len;
}

static void load_sfx() {
    u32 offset = 0;
    for (u8 i = 0; i < nSfx; i++) load_one_sfx(i, &offset);
}

void audio::SfxPlay(const u8 index, const u8 channel) {
    if (index >= nSfx || channel >= N_SFX_VOICES || sfx_info[index].length == 0) return;
    SDL_LockAudioDevice(dev);
    sfx_voices[channel] = {
        .pos    = sfx_info[index].offset,
        .end    = sfx_info[index].offset + sfx_info[index].length,
        .active = true
    };
    SDL_UnlockAudioDevice(dev);
}

void audio::SfxSamplePlay(const u8 index) { audio::SfxPlay(index, 1); }

// Decode the embedded OGG and convert it to the 48k/2ch/s16 device format.
static void load_track() {
    int channels = 0, rate = 0;
    short *decoded = nullptr;
    const int frames = stb_vorbis_decode_memory(ogg_data, ogg_len,
                                                &channels, &rate, &decoded);
    if (frames < 0 || !decoded) {
        SDL_Log("AUDIO: stb_vorbis decode of embedded OGG failed");
        return;
    }
    const u32 pcm_bytes = static_cast<u32>(frames) * channels * sizeof(short);

    // Build a converter from the decoded spec (native-endian S16) to the device
    // format. SDL_ConvertAudio works in place in a buffer sized
    // cvt.len * cvt.len_mult, so allocate that.
    SDL_AudioCVT cvt;
    const int r = SDL_BuildAudioCVT(&cvt,
                                    AUDIO_S16SYS, static_cast<Uint8>(channels), rate,
                                    AUDIO_S16SYS, 2, SAMPLE_RATE);
    if (r < 0) {
        SDL_Log("AUDIO: SDL_BuildAudioCVT failed: %s", SDL_GetError());
        free(decoded);  // stb_vorbis allocates with the libc allocator
        return;
    }

    if (r == 0) {
        // Already in device format; keep the decoded buffer as-is.
        track_data = static_cast<u8 *>(SDL_malloc(pcm_bytes));
        if (track_data) {
            memcpy(track_data, decoded, pcm_bytes);
            track_len = pcm_bytes & ~(BYTES_PER_FRAME - 1);  // frame-align
        }
        free(decoded);
    } else {
        cvt.len = static_cast<int>(pcm_bytes);
        cvt.buf = static_cast<u8 *>(SDL_malloc(static_cast<size_t>(cvt.len) * cvt.len_mult));
        if (!cvt.buf) {
            free(decoded);
            return;
        }
        memcpy(cvt.buf, decoded, pcm_bytes);
        free(decoded);
        if (SDL_ConvertAudio(&cvt) < 0) {
            SDL_Log("AUDIO: SDL_ConvertAudio failed: %s", SDL_GetError());
            SDL_free(cvt.buf);
            return;
        }
        track_data = cvt.buf;                          // keep the converted buffer
        track_len  = static_cast<u32>(cvt.len_cvt) & ~(BYTES_PER_FRAME - 1);
    }

    // tracks[0].loop_start is in seconds (0 = loop the whole track); convert to a
    // frame-aligned byte offset in the converted stream.
    u32 lp = 0;
    if (nTracks > 0) lp = static_cast<u32>(tracks[0].loop_start * BYTES_PER_SEC);
    lp &= ~(BYTES_PER_FRAME - 1);
    loop_point = (track_len && lp < track_len) ? lp : 0;
}

// SDL audio callback: fill `len` bytes from the track (or silence when
// stopped/paused/absent), looping at loop_point, then additively mix in
// every active SFX voice (s16, clamped).
static void audio_callback(void *, u8 *stream, int len) {
    if (!playing || paused || track_len == 0) {
        memset(stream, 0, static_cast<size_t>(len));
    } else {
        int written = 0;
        while (written < len) {
            u32 avail = track_len - play_pos;
            u32 n = static_cast<u32>(len - written);
            if (n > avail) n = avail;
            memcpy(stream + written, track_data + play_pos, n);
            written  += static_cast<int>(n);
            play_pos += n;
            if (play_pos >= track_len) play_pos = loop_point;
        }
    }

    i16 *out = reinterpret_cast<i16 *>(stream);
    for (auto &v : sfx_voices) {
        if (!v.active) continue;
        u32 avail = v.end - v.pos;
        u32 n = static_cast<u32>(len) < avail ? static_cast<u32>(len) : avail;
        const i16 *src = reinterpret_cast<const i16 *>(sfx_data + v.pos);
        for (u32 i = 0; i < n / sizeof(i16); i++) {
            const int mixed = static_cast<int>(out[i]) + static_cast<int>(src[i]);
            out[i] = static_cast<i16>(mixed < -32768 ? -32768 : (mixed > 32767 ? 32767 : mixed));
        }
        v.pos += n;
        if (v.pos >= v.end) v.active = false;
    }
}

void audio::Init(u8) {
    load_track();
    load_sfx();

    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq     = SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;   // == AUDIO_S16MSB on big-endian Espresso,
                                    // exactly what the wiiu AX driver forces.
    want.channels = 2;
    want.samples  = 1024;
    want.callback = audio_callback;

    // allowed_changes == 0 forces the device to our format, so the callback can
    // assume 48k/2ch/s16 and the converted track buffer matches it exactly.
    dev = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    if (dev == 0) {
        SDL_Log("AUDIO: SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(dev, 0);  // start the callback running
}

void audio::Update() {
    // Streaming is driven by the SDL audio callback; nothing to do per frame.
}

void audio::TrackPlay(const u8 index) {
    if (index >= nTracks) return;
    SDL_LockAudioDevice(dev);
    play_pos = 0;
    playing  = true;
    paused   = false;
    SDL_UnlockAudioDevice(dev);
}

void audio::TrackStop() {
    SDL_LockAudioDevice(dev);
    playing = false;
    SDL_UnlockAudioDevice(dev);
}

void audio::TrackPause(const u8 pause) {
    (void)pause;  // match the SDL/Switch backends: toggle regardless of argument
    SDL_LockAudioDevice(dev);
    paused = !paused;
    SDL_UnlockAudioDevice(dev);
}
