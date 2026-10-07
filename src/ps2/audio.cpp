/**
 * @file audio.cpp
 * @brief Music playback for the Sony PlayStation 2 backend (audsrv).
 *
 * Follows the GameCube/Wii U/PSP shape (src/ogc/audio.cpp, src/wiiu/
 * audio.cpp, src/psp/audio.cpp): the desktop pc_audio.wav is transcoded to a
 * small Vorbis OGG at build time and #embedded into the ELF (see the ps2
 * branch of CMakeLists.txt), decoded here with stb_vorbis.
 *
 * Like PSP, the EE has real kernel threads and a call that blocks until the
 * hardware actually has room for more data, so this streams from a
 * dedicated thread rather than polling from audio::Update() once per video
 * frame the way GameCube (no threads) has to: it decodes one chunk, calls
 * audsrv_wait_audio() (blocks until audsrv's ring buffer has room for it --
 * the canonical ps2sdk pacing idiom, see samples/rpc/audsrv/playwav/
 * playwav.c), then audsrv_play_audio(), and repeats. That keeps playback off
 * the 60Hz render loop entirely, so Update() is a no-op here too.
 *
 * audsrv is NOT a BIOS-resident module (unlike SIO2MAN/PADMAN in src/ps2/
 * input.cpp) -- it's a ps2sdk-supplied IOP driver, so the prebuilt audsrv.irx
 * ps2sdk ships is #embedded into the ELF the same way the OGG is, and loaded
 * at runtime with SifExecModuleBuffer instead of SifLoadModule's rom0: path.
 * LIBSD (the SPU2 driver audsrv itself depends on) IS BIOS-resident.
 *
 * No SFX path: like GameCube/PSP, this project currently registers no SFX
 * table for any non-NES target (see demo/src/tracks.cpp) -- building a whole
 * second decode/mix path for a currently-empty table isn't worth it. Revisit
 * alongside those backends if/when a non-NES SFX table exists.
 */
#include "internal.hpp"
#include <platform-nes/audio.hpp>

#include <sifrpc.h>
#include <loadfile.h>
#include <kernel.h>
#include <audsrv.h>
#include <cstring>

#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include <stb_vorbis.c>

// The Vorbis-encoded music, baked into the ELF at build time. ffmpeg
// produces pc_audio.ogg (encoded straight to this backend's playback rate,
// see SAMPLE_RATE below) into the build tree, and CMake puts that dir on the
// embed path, so #embed finds it.
static const unsigned char ogg_data[] = {
#embed <pc_audio.ogg>   // resolved via --embed-dir (set in the ps2 CMake branch)
};
static constexpr int ogg_len = static_cast<int>(sizeof(ogg_data));

// The prebuilt audsrv IOP module ps2sdk ships (see the file header);
// #embedded the same way as the OGG, via --embed-dir pointing at ps2sdk's
// iop/irx directory (set in the ps2 CMake branch).
static const unsigned char audsrv_irx[] = {
#embed <audsrv.irx>
};
static constexpr int audsrv_irx_len = static_cast<int>(sizeof(audsrv_irx));

static constexpr int SAMPLE_RATE = 44100;
static constexpr int CHANNELS    = 2;

// One audsrv chunk per decode/play cycle. ~46ms at 44.1kHz stereo 16-bit --
// small enough for responsive TrackPlay/TrackStop, large enough that the
// decode thread isn't spinning. Matches src/psp/audio.cpp's CHUNK_FRAMES.
static constexpr int CHUNK_FRAMES = 2048;
static constexpr int CHUNK_BYTES  = CHUNK_FRAMES * CHANNELS * static_cast<int>(sizeof(s16));

// audio.hpp declares these for the SDL3 mixer; this backend streams straight
// into audsrv chunk buffers instead, but define them so the externs resolve.
float *audio::pcm_buffer = nullptr;
u32 audio::pcm_buffer_size = 0;

// Every non-NES backend leaves ::sfxs/::nSfx undefined unless the app calls
// ::SFX(...); this weak zero-entry default keeps the link working until it
// does (mirrors the SDL3/Switch/Wii U/PSP backends), even though this
// backend doesn't act on it (see the file header).
__attribute__((weak)) extern const audio::sfx_t sfxs[1] = {};
__attribute__((weak)) extern const u8 nSfx = 0;

namespace {

alignas(16) s16 chunk[CHUNK_FRAMES * CHANNELS];

stb_vorbis *vorbis     = nullptr;
u32         loop_frame = 0;      // stb_vorbis_seek() target on EOF
bool        playing    = false;
bool        paused     = false;
bool        running    = false;

// Fills chunk[] with CHUNK_FRAMES stereo frames, looping back to
// loop_frame on EOF (mirrors src/psp/audio.cpp's fill()).
void fill() {
    s16 *dst = chunk;
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
               static_cast<size_t>(CHUNK_FRAMES - got) * CHANNELS * sizeof(s16));
    }
}

void audio_thread(void *) {
    while (running) {
        if (!vorbis || !playing || paused) {
            memset(chunk, 0, sizeof(chunk));
        } else {
            fill();
        }
        // Blocks until audsrv's ring buffer has room for this chunk -- the
        // call that paces this whole thread at the hardware's real playback
        // rate, no sleep/poll needed (see the file header).
        audsrv_wait_audio(CHUNK_BYTES);
        audsrv_play_audio(reinterpret_cast<const char *>(chunk), CHUNK_BYTES);
    }
    ExitThread();
}

alignas(16) u8 audio_thread_stack[0x4000];

} // namespace

void audio::Init(u8) {
    SifLoadModule("rom0:LIBSD", 0, nullptr);
    SifExecModuleBuffer(const_cast<unsigned char *>(audsrv_irx), audsrv_irx_len, 0, nullptr, nullptr);

    if (audsrv_init() != 0) return;

    audsrv_fmt_t format;
    format.bits     = 16;
    format.freq     = SAMPLE_RATE;
    format.channels = CHANNELS;
    audsrv_set_format(&format);
    audsrv_set_volume(MAX_VOLUME);

    int err = 0;
    vorbis = stb_vorbis_open_memory(ogg_data, ogg_len, &err, nullptr);
    if (!vorbis) return;

    if (nTracks > 0) {
        loop_frame = tracks[0].loop_start > 0.0f
            ? static_cast<u32>(tracks[0].loop_start * static_cast<float>(SAMPLE_RATE))
            : 0;
    }

    running = true;

    ee_thread_t thread;
    thread.func             = reinterpret_cast<void *>(audio_thread);
    thread.stack            = audio_thread_stack;
    thread.stack_size       = sizeof(audio_thread_stack);
    thread.gp_reg           = &_gp;
    thread.initial_priority = 50;
    thread.attr             = 0;
    thread.option           = 0;

    const s32 tid = CreateThread(&thread);
    if (tid >= 0) StartThread(tid, nullptr);
}

void audio::Update() {
    // Streaming is driven by the dedicated audio thread; nothing to do per frame.
}

void audio::TrackPlay(const u8 index) {
    if (!vorbis || index >= nTracks) return;
    stb_vorbis_seek_start(vorbis);
    loop_frame = tracks[index].loop_start > 0.0f
        ? static_cast<u32>(tracks[index].loop_start * static_cast<float>(SAMPLE_RATE))
        : 0;
    playing = true;
    paused  = false;
}

void audio::TrackStop() {
    playing = false;
}

void audio::TrackPause(const u8 pause) {
    (void)pause;  // match the SDL/Switch/Wii U/PSP backends: toggle regardless of argument
    paused = !paused;
}

// See the file header: no non-NES SFX table exists in this project yet.
void audio::SfxPlay(u8 /*index*/, u8 /*channel*/) {}
void audio::SfxSamplePlay(u8 /*index*/) {}
