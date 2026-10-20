/**
 * @file audio.cpp
 * @brief Music playback for the native GameCube + Wii backend (libogc/ASND).
 *
 * Real Nintendo DSP-ADPCM decode is NOT reachable from this toolchain: ASND
 * and AESND (libogc's two audio APIs) only expose raw-PCM voice formats, and
 * the ADPCM decode microcode Nintendo's own SDK used was never released for
 * homebrew -- devkitPro's dsp.h is just a raw DSP-task loader with no ADPCM
 * task to hand it. So this follows the same shape as the Wii U backend
 * (src/wiiu/audio.cpp): the desktop pc_audio.wav is transcoded to a small
 * Vorbis OGG at build time and #embedded into the binary (see the gc/wii
 * branch of CMakeLists.txt), decoded here with stb_vorbis.
 *
 * Unlike Wii U (which streams via an SDL2 callback on its own audio thread),
 * libogc has no SDL audio device, so playback is polled from audio::Update(),
 * which the engine already calls once per video frame. ASND supports exactly
 * two buffers per voice -- one playing, one queued -- so Update() checks
 * ASND_TestVoiceBufferReady() and decodes the next chunk into whichever of
 * the two ping-ponged buffers isn't currently playing. Buffers are sized to
 * ~0.37s of audio, far more than one frame needs, so a missed Update() call
 * or two (e.g. a GC build's occasional per-frame hitch) doesn't starve
 * playback. GC/Wii RAM (24MB/64MB) also can't hold the whole 47MB source
 * decoded to raw PCM the way Wii U's approach effectively can, which is the
 * other reason this streams instead of decoding once at Init like Wii U does.
 *
 * Full pre-rendered streaming is also the only workflow the DSP-ADPCM
 * question upstream of this landed on: there's no per-note ADPCM sampler
 * here, just a compressed audio stream played back start-to-loop-point.
 */
#include "internal.hpp"
#include <pnes/audio.hpp>

#include <asndlib.h>
#include <gctypes.h>

#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include <stb_vorbis.c>

// The Vorbis-encoded music, baked into the .dol at build time.
static const unsigned char ogg_data[] = {
#embed <pc_audio.ogg>   // resolved via --embed-dir (set in the gc/wii CMake branch)
};
static constexpr int ogg_len = static_cast<int>(sizeof(ogg_data));

// audio.hpp declares these for the SDL3 mixer; this backend streams straight
// into ASND voice buffers instead, but define them so the externs resolve.
float *audio::pcm_buffer = nullptr;
u32 audio::pcm_buffer_size = 0;

namespace {

constexpr s32 VOICE          = 0;  // ASND reserves voice 0, by convention, for a music/OGG player
constexpr int CHANNELS       = 2;  // pc_audio.wav is stereo; this backend doesn't need to generalize past that
constexpr int BUFFER_FRAMES  = 4096;   // ~93ms @ 44.1kHz
constexpr int BUFFER_SHORTS  = BUFFER_FRAMES * CHANNELS;

// ASND voice buffers must be 32-byte aligned and padded to 32 bytes; both
// halves land on a 32-byte boundary since BUFFER_SHORTS*sizeof(i16) is
// itself a multiple of 32.
alignas(32) i16 buf[2][BUFFER_SHORTS];

stb_vorbis *vorbis     = nullptr;
int         src_rate   = 44100;
u32         loop_frame = 0;      // stb_vorbis_seek() target on EOF
int         next_buf   = 0;      // which half of buf[] Update() fills next
bool        playing    = false;
bool        paused     = false;
bool        primed     = false;  // buf[0] was decoded ahead in Init(), not TrackPlay()
bool        pending    = false;  // buf[next_buf] is decoded and waiting on ASND_AddVoice()

// Fills buf[which] with BUFFER_FRAMES stereo frames, looping back to
// loop_frame on EOF (mid-buffer, if the loop point falls inside this chunk).
// stall_guard bails out on a degenerate/empty stream instead of spinning.
void fill(const int which) {
    i16 *dst = buf[which];
    int got = 0, stalls = 0;
    while (got < BUFFER_FRAMES && stalls < 4) {
        const int n = stb_vorbis_get_samples_short_interleaved(
            vorbis, CHANNELS, dst + got * CHANNELS, (BUFFER_FRAMES - got) * CHANNELS);
        if (n == 0) {
            stb_vorbis_seek(vorbis, loop_frame);
            stalls++;
            continue;
        }
        got += n;
        stalls = 0;
    }
    if (got < BUFFER_FRAMES) {
        memset(dst + got * CHANNELS, 0, static_cast<size_t>(BUFFER_FRAMES - got) * CHANNELS * sizeof(i16));
    }
}

} // namespace

void audio::Init(u8) {
    ASND_Init();
    ASND_Pause(0);

    int err = 0;
    vorbis = stb_vorbis_open_memory(ogg_data, ogg_len, &err, nullptr);
    if (!vorbis) return;
    src_rate = stb_vorbis_get_info(vorbis).sample_rate;

    // Decode the first buffer here, ahead of TrackPlay(), so that call site's
    // caller (level.cpp calls audio::Init() immediately followed by
    // audio::TrackPlay(0), right as a level starts) doesn't pay the decode
    // cost synchronously at the moment music is meant to start.
    if (nTracks > 0) {
        loop_frame = tracks[0].loop_start > 0.0f
            ? static_cast<u32>(tracks[0].loop_start * static_cast<float>(src_rate))
            : 0;
        fill(0);
        primed = true;
    }
}

void audio::Update() {
    if (!vorbis || !playing || paused) return;

    // Decode the next chunk only once there's nowhere else it needs to wait:
    // ASND has exactly one "next" slot, so decoding ahead of confirming that
    // slot is actually free would mean, if ASND_AddVoice() below then fails
    // with SND_BUSY, that chunk gets silently thrown away -- the Vorbis
    // decoder's read position had already moved past it -- and playback
    // skips ahead by however much was lost. That's what produced the
    // startup glitch-and-jump: decode and "confirmed queued" must stay in
    // lockstep, so pending only clears on a successful add, never on the
    // decode itself.
    if (!pending) {
        if (!ASND_TestVoiceBufferReady(VOICE)) return;
        next_buf ^= 1;
        fill(next_buf);
        pending = true;
    }

    if (ASND_AddVoice(VOICE, buf[next_buf], BUFFER_SHORTS * sizeof(i16)) == SND_OK) {
        pending = false;
    }
    // else: voice slot wasn't actually free yet; retry the same already-
    // decoded buf[next_buf] next Update() call instead of decoding further.
}

void audio::TrackPlay(const u8 index) {
    if (!vorbis || index >= nTracks) return;

    ASND_StopVoice(VOICE);

    // Init() already decoded buf[0] for track 0 ahead of time (see its own
    // comment); reuse that instead of re-seeking/re-decoding synchronously
    // here, which is what produced an audible stutter right as playback
    // started. Any other index (or a replay after the primed buffer's
    // already been consumed) still pays the decode cost inline.
    if (!(index == 0 && primed)) {
        stb_vorbis_seek_start(vorbis);
        // tracks[index].loop_start is in seconds (0 = loop the whole track),
        // matching the convention src/wiiu/audio.cpp's load_track() already uses.
        loop_frame = tracks[index].loop_start > 0.0f
            ? static_cast<u32>(tracks[index].loop_start * static_cast<float>(src_rate))
            : 0;
        fill(0);
    }
    primed  = false;
    pending = false;

    next_buf = 0;
    ASND_SetVoice(VOICE, VOICE_STEREO_16BIT, src_rate, 0,
                  buf[0], BUFFER_SHORTS * sizeof(i16),
                  MAX_VOLUME, MAX_VOLUME, nullptr);
    playing = true;
    paused  = false;
}

void audio::TrackStop() {
    ASND_StopVoice(VOICE);
    playing = false;
}

void audio::TrackPause(const u8 pause) {
    (void)pause;  // match the SDL/Wii U backends: toggle regardless of argument
    paused = !paused;
    ASND_PauseVoice(VOICE, paused);
}

// No SFX assets are registered for this project on non-NES targets yet (see
// demo/src/tracks.cpp); leave these as the same no-ops the stub had rather
// than building an unused decode path for a table that's currently empty.
void audio::SfxPlay(u8 /*index*/, u8 /*channel*/) {}
void audio::SfxSamplePlay(u8 /*index*/) {}
