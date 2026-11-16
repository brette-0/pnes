/**
 * @file audio.cpp
 * @brief Music and sound-effect playback for the Nintendo 3DS backend (ndsp).
 *
 * THIS BACKEND MIXES NOTHING IN SOFTWARE, and that is the whole reason it
 * looks different from the Switch's (src/switch/audio.cpp) despite the two
 * consoles being the closest pair in this project. ndsp exposes 24 INDEPENDENT
 * HARDWARE VOICES, each with its own rate, its own 12-way output mix and its
 * own pause bit, so the shape here is libogc's (src/ogc/audio.cpp): voice 0
 * carries the music, voices 1..audio::sfxChannels carry one effect each, and
 * every level the API takes is handed to the DSP rather than multiplied into
 * samples. What that buys, point by point:
 *
 *   - ::audio::TrackVolume is an ndspChnSetMix on voice 0 alone, so the music
 *     is attenuated without the effects being touched -- audio.hpp's promise
 *     falls out of the hardware instead of out of a careful ordering inside a
 *     mixing loop (compare the Switch's fill_chunk, which has to scale the
 *     song BEFORE it sums the effects in).
 *   - ::audio::SfxPlay's volume byte is that voice's own mix, latched by the
 *     DSP when the effect starts.
 *   - ::audio::SfxStop and ::audio::SfxPause are EXACT and nearly free:
 *     ndspChnWaveBufClear drops the queue, ndspChnSetPaused freezes the voice
 *     with its cursor intact. No `paused` flag, no cursor bookkeeping.
 *   - SFX LATENCY IS THE DSP'S FRAME, ~5ms, not a queue depth. An effect is a
 *     voice of its own, so it starts on the next DSP tick rather than waiting
 *     for the music chunk it was mixed into to reach the DAC (the Switch pays
 *     43-64ms for exactly that, and its own note explains that moving to
 *     per-voice submission -- i.e. to this -- is the real fix).
 *
 * THE MUSIC STREAMS AS VORBIS, decoded with stb_vorbis on a dedicated thread,
 * which is the GameCube/PSP arrangement and for the same arithmetic: the
 * desktop pc_audio.wav is 47MB, and even at this DSP's own 32728Hz raw stereo
 * PCM is ~32MB -- more than an o3DS gives a .3dsx for everything put together.
 * ffmpeg transcodes it to a ~4MB OGG at build time (see the ctr branch of
 * CMakeLists.txt) and this reads it from the RomFS with stb_vorbis's file API,
 * so the compressed stream stays on the SD card and only one decoded chunk is
 * ever in RAM.
 *
 * THE EFFECTS ARE RAW PCM IN THE ROMFS, like the Switch's and for the reason
 * the GameCube embeds raw rather than Vorbis: they are a few hundred
 * milliseconds each (~215KB for this project's five at 32728Hz stereo), so
 * decoding them would cost startup time and a decoder's worth of RAM to save
 * nothing that matters. Each is loaded once into LINEAR MEMORY -- the DSP
 * reads a voice's buffer by DMA and cannot see the regular heap -- and handed
 * to its voice as-is.
 *
 * ndspInit() REQUIRES THE DSP FIRMWARE and fails without it. libctru loads
 * sdmc:/3ds/dspfirm.cdc itself (or takes it from hbmenu's `hb:ndsp` handle, if
 * the launcher supplied one) BEFORE it talks to the DSP service at all, so this
 * is not something an emulator setting or this backend can work around --
 * Nintendo's firmware cannot be redistributed, which is why every 3DS homebrew
 * with sound asks the player for one dump (Luma3DS: Rosalina -> Miscellaneous
 * options -> Dump DSP firmware). Under an emulator a zero-byte placeholder at
 * that path is enough, since HLE audio never executes the firmware.
 *
 * There being no software fallback, a failure leaves every entry point below
 * inert -- but NOT silent about itself: ::note_missing_dsp_firmware writes a
 * plain-text explanation to the SD card, in release builds too, because the
 * player hears a mute game and has nothing else to go on. OpenDSP (BSD-2) is
 * the standing option for removing the requirement altogether; it swaps
 * ndspInit for opendspInit and is noted in README.md's dependency list.
 */
#include "internal.hpp"
#include <pnes/audio.hpp>

#include <3ds.h>
#include <cstdio>
#include <cstring>
#include <malloc.h>

#define STB_VORBIS_NO_PUSHDATA_API
#include <stb_vorbis.c>

// MUST come after stb_vorbis.c, for the reason src/ps2/audio.cpp spells out at
// this same spot: stb_vorbis drags in <math.h>, libstdc++'s <cmath> #undefs any
// macro named `log` before declaring std::log, and stb_vorbis itself CALLS
// log() (its codebook setup, stb_vorbis.c's `exp(log(entriesd) / dim)`). Either
// ordering mistake is silent -- the macro erased for the rest of this file, or
// stb_vorbis's own call expanded into a logging statement.
#include <pnes/logger.hpp>

/*
 * EVERY STEP THAT CAN FAIL SILENTLY IS LOGGED, because they all present
 * identically as "no sound": the DSP coming up, the RomFS mounting, each effect
 * loading and its length. Diagnosing this backend without them means guessing
 * between four unrelated causes, which is exactly how the first bring-up went.
 * logger.hpp's 3DS branch puts them on the SD card (sdmc:/pnes.log) -- see its
 * own comment for why that is the only channel this console really has.
 *
 * ONE EXCEPTION IS NOT DEBUG-GATED: ::audio::Init's missing-firmware note. The
 * rest of this is for whoever is working on the backend, and logger.hpp is
 * rightly compiled out of a release build; that one line is for a PLAYER, whose
 * release .3dsx would otherwise be mute with nothing anywhere to say why. See
 * ::note_missing_dsp_firmware.
 */

// The rate the ctr branch encodes BOTH asset kinds at: ndsp's own
// NDSP_SAMPLE_RATE (SYSCLOCK_SOC / 512 = 32728.5Hz), so the DSP's resampler
// has nothing to do for either. The music's rate is still read back from the
// OGG itself (see ::open_track) rather than assumed -- a voice's rate is one
// call, and a re-encode at a different rate should retune the voice, not
// detune the song. This constant is therefore what the EFFECTS are played at,
// where there is no container header to ask.
static constexpr float SFX_RATE = 32728.0f;
static constexpr u32   CHANNELS = 2;
static constexpr u32   BYTES_PER_FRAME = CHANNELS * sizeof(i16);

// Voice 0 is the music; the effects take the voices above it, one per
// audio::sfxChannels slot. That split is what lets ::audio::TrackVolume scale
// the song without touching an effect (ndsp mixes per voice).
static constexpr int MUSIC_VOICE    = 0;
static constexpr int SFX_VOICE_BASE = MUSIC_VOICE + 1;
static constexpr int N_SFX_VOICES   = audio::sfxChannels;
static_assert(SFX_VOICE_BASE + N_SFX_VOICES <= 24,
              "audio::sfxChannels asks for more ndsp voices than the DSP has "
              "(24) once the music has taken voice 0");

// Double-buffered music chunks, ~62ms each. SIZED FOR NOTHING IN PARTICULAR,
// which is the luxury of per-voice effects: nothing is mixed into this stream,
// so a chunk's length bounds only how long ::audio::TrackStop takes to fall
// silent (one chunk -- the thread stops filling, but the queued buffer still
// plays), never how late an effect sounds. 2048 frames keeps that under a
// tenth of a second while leaving the decoder two full chunks of slack behind
// a callback that fires every ~5ms.
static constexpr u32 CHUNK_FRAMES = 2048;
static constexpr int N_BUF        = 2;

// audio.hpp declares these for the desktop mixer; the 3DS hands PCM to the DSP
// instead, but define them so the externs resolve if anything references them.
float *audio::pcm_buffer = nullptr;
u32 audio::pcm_buffer_size = 0;

namespace {

// Music voice state. Only ::audio_thread calls into `vorbis`.
stb_vorbis *vorbis     = nullptr;
u32         loop_frame = 0;         // stb_vorbis_seek() target on EOF

// TWO READINESS FLAGS, NOT ONE, because the effects do not need any of the
// music's machinery and must not die with it. ndsp_ready means the DSP came up
// and the voices are configured -- which is everything ::audio::SfxPlay uses,
// since an effect is a whole buffer handed to a voice of its own. music_ready
// additionally means the chunk buffers were allocated and the decode thread is
// running. One flag set at the end of ::audio::Init covered both, so a failed
// linearAlloc or threadCreate -- neither of which an effect touches -- silenced
// the entire menu as collateral.
bool ndsp_ready  = false;
bool music_ready = false;

// TrackPlay() runs on the main thread while audio_thread() may be mid-decode,
// and stb_vorbis's decoder object cannot be called into from two threads at
// once -- a seek landing inside a get_samples call corrupts its state, after
// which every decode returns 0 samples forever: permanent silence, no error,
// no crash. (First caught on the PS2; src/psp/audio.cpp and src/ps2/audio.cpp
// carry the same note.) So TrackPlay only POSTS the index here and the audio
// thread -- the one thread allowed to call any stb_vorbis_* function -- opens
// and seeks at the top of its own loop. -1 means nothing pending.
//
// `volatile`, the project's ::atomic (pnes/technology.hpp), is the whole
// synchronisation this needs for the posted values: one writer, one reader
// that is also the only clearer, naturally-aligned objects, and homebrew
// threads sharing one core.
volatile int  pending_track = -1;
volatile bool playing       = false;
// ::audio::TrackPause's toggle. The DSP holds the actual pause bit, so this is
// only the memory of which way the next toggle goes -- and it is cleared by
// ::audio::TrackPlay, which unpauses the voice: left set across a play, the
// first press afterwards would spend itself turning a pause OFF that the
// hardware had already dropped, and the player would have to press twice.
volatile bool music_paused = false;

// audio::TrackVolume's setting, as a percentage of full output. Kept even
// though the DSP is what applies it, for the two reasons the GameCube backend
// keeps its copy: the API allows a volume set BEFORE Init (applied in
// ::setup_music_voice), and a per-track ndspChnReset has to restore the level
// rather than leave the voice at its default.
volatile u8 volume = 100;

// One decoded chunk per buffer, in LINEAR MEMORY: the DSP reads these by DMA.
i16         *chunk[N_BUF]   = {};
ndspWaveBuf  wavebuf[N_BUF] = {};

// Where an effect's PCM lives once loaded, in linear memory for the same
// reason. A zero `frames` is the ordinary "there was no file" state, not an
// error -- an effect the build did not transcode simply never sounds.
struct sfx_runtime_t { i16 *data; u32 frames; };
// 256 entries because ::nSfx is a u8, so this is every table an application
// can register -- 2KB of BSS and no allocation to get wrong, the same shape as
// the Switch/Wii U/GameCube backends.
sfx_runtime_t sfx_info[256] = {};
ndspWaveBuf   sfx_wavebuf[N_SFX_VOICES] = {};

Thread        athread = nullptr;
LightEvent    audio_event;
volatile bool running = false;

// Both asset kinds live in the RomFS under the path the APPLICATION's own
// table gives, with the extension the build transcoded them to: the track
// table names tracks/pc_audio.wav and the effect table sfx/Select.ogg
// (demo/src/tracks.cpp, demo/ui/impl/sounds.hpp), so this swaps the extension
// and prefixes the mount -- it does not invent names of its own. Mirrors
// src/switch/audio.cpp's romfs_raw_path, which does the same thing for the
// same reason.
void romfs_path(const char *fp, const char *ext, char *out, const size_t cap) {
    snprintf(out, cap, "romfs:/%s", fp);
    char *const dot = strrchr(out, '.');
    const size_t n = strlen(ext) + 1;  // incl. NUL
    if (dot && (cap - static_cast<size_t>(dot - out)) >= n) memcpy(dot, ext, n);
}

// The mix array ndsp takes, with @p level (0.0-1.0) on the two front speakers.
// Entries 2..11 stay zero: those are the back/aux sends, and this is a stereo
// output mode (::audio::Init sets NDSP_OUTPUT_STEREO).
void set_voice_level(const int id, const float level) {
    float mix[12] = {};
    mix[0] = level;
    mix[1] = level;
    ndspChnSetMix(id, mix);
}

// Configures voice 0 for @p rate. Called on every track open rather than once
// at Init, because the rate comes from the OGG (see ::SFX_RATE) and a second
// track may have been encoded differently.
void setup_music_voice(const float rate) {
    // DROP THE QUEUE AND OWN UP TO IT, in that order. ndspChnReset detaches
    // whatever this voice was playing, which leaves the ndspWaveBuf structs
    // BELOW still carrying whatever status the DSP last wrote -- and a buffer
    // frozen at QUEUED is one ::audio_thread will never refill, because its
    // refill test is exactly "is this FREE or DONE". The result would be a
    // stream that plays the first track and then goes permanently, silently
    // dead on the second. Clearing the channel first and then declaring the
    // buffers free makes the state unambiguous rather than depending on what
    // ndspChnReset leaves behind. Safe to rewrite them here because the voice
    // no longer points at them, and because this only ever runs on the audio
    // thread -- the one thread that touches ::wavebuf at all.
    ndspChnWaveBufClear(MUSIC_VOICE);
    ndspChnReset(MUSIC_VOICE);
    for (auto &wb : wavebuf) wb.status = NDSP_WBUF_DONE;
    ndspChnSetInterp(MUSIC_VOICE, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(MUSIC_VOICE, rate);
    ndspChnSetFormat(MUSIC_VOICE, NDSP_FORMAT_STEREO_PCM16);
    set_voice_level(MUSIC_VOICE, static_cast<float>(volume) / 100.0f);
}

// Opens the track at @p index from the RomFS and points voice 0 at its rate.
// AUDIO-THREAD ONLY, like every other stb_vorbis_* call here -- see
// ::pending_track.
void open_track(const u8 index) {
    if (index >= nTracks) return;

    if (vorbis) { stb_vorbis_close(vorbis); vorbis = nullptr; }

    char path[300];
    romfs_path(tracks[index].fp, ".ogg", path, sizeof(path));

    int err = 0;
    vorbis = stb_vorbis_open_filename(path, &err, nullptr);
    if (!vorbis) return;

    const stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    // loop_start is in SECONDS (0 = loop the whole track), as every streaming
    // backend here reads it; convert against the stream's own rate.
    loop_frame = tracks[index].loop_start > 0.0f
        ? static_cast<u32>(tracks[index].loop_start * static_cast<float>(info.sample_rate))
        : 0;

    setup_music_voice(static_cast<float>(info.sample_rate));
}

// Fills chunk[which] with CHUNK_FRAMES stereo frames, looping back to
// loop_frame on EOF (mirrors src/psp/audio.cpp's and src/ogc/audio.cpp's
// fill(); `stalls` bails out on a degenerate stream instead of spinning).
void fill(const int which) {
    i16 *const dst = chunk[which];
    u32 got = 0;
    int stalls = 0;
    while (got < CHUNK_FRAMES && stalls < 4) {
        const int n = stb_vorbis_get_samples_short_interleaved(
            vorbis, static_cast<int>(CHANNELS), dst + got * CHANNELS,
            static_cast<int>((CHUNK_FRAMES - got) * CHANNELS));
        if (n <= 0) {
            stb_vorbis_seek(vorbis, loop_frame);
            stalls++;
            continue;
        }
        got += static_cast<u32>(n);
        stalls = 0;
    }
    if (got < CHUNK_FRAMES)
        memset(dst + got * CHANNELS, 0, (CHUNK_FRAMES - got) * BYTES_PER_FRAME);
}

// ndsp's own frame callback, which fires every DSP frame (~5ms) whether or not
// anything is queued. It does nothing but wake the decoder: it runs in a
// context where a Vorbis decode has no business happening.
void ndsp_frame(void *) { LightEvent_Signal(&audio_event); }

void audio_thread(void *) {
    while (running) {
        LightEvent_Wait(&audio_event);

        // Apply any TrackPlay() request -- see ::pending_track for why the
        // open and the seek happen here and not in TrackPlay itself.
        const int req = pending_track;
        if (req >= 0) {
            pending_track = -1;
            open_track(static_cast<u8>(req));
            if (vorbis) {
                stb_vorbis_seek_start(vorbis);
                playing = true;
                ndspChnSetPaused(MUSIC_VOICE, false);
            }
        }

        for (int i = 0; i < N_BUF; i++) {
            if (wavebuf[i].status != NDSP_WBUF_FREE &&
                wavebuf[i].status != NDSP_WBUF_DONE)
                continue;

            // Silence when stopped, rather than an empty queue: the voice
            // keeps running, so TrackPlay resumes into a stream that is
            // already paced and the callback above keeps arriving.
            if (!vorbis || !playing)
                memset(chunk[i], 0, CHUNK_FRAMES * BYTES_PER_FRAME);
            else
                fill(i);

            // The DSP reads this buffer by DMA, so the CPU's cached writes have
            // to be in memory before the voice is pointed at it.
            DSP_FlushDataCache(chunk[i], CHUNK_FRAMES * BYTES_PER_FRAME);

            wavebuf[i].data_pcm16 = chunk[i];
            wavebuf[i].nsamples   = CHUNK_FRAMES;
            wavebuf[i].looping    = false;
            ndspChnWaveBufAdd(MUSIC_VOICE, &wavebuf[i]);
        }
    }
}

// Loads every registered effect into its own linear-memory buffer. Called once,
// from Init, before the voices are configured.
void load_sfx() {
    log("load_sfx: nSfx=%u sfxs=%p", static_cast<unsigned>(nSfx),
          static_cast<const void *>(sfxs));
    for (u8 i = 0; i < nSfx; i++) {
        char path[300];
        romfs_path(sfxs[i].fp, ".raw", path, sizeof(path));

        FILE *const f = fopen(path, "rb");
        if (!f) { log("load_sfx: %s -- fopen failed", path); continue; }  // stays silent
        fseek(f, 0, SEEK_END);
        const long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0) { fclose(f); log("load_sfx: %s -- empty", path); continue; }

        // Frame-aligned, so a truncated file cannot hand the voice half a
        // frame and swap its channels for the rest of the effect.
        const u32 bytes = static_cast<u32>(sz) & ~(BYTES_PER_FRAME - 1);
        if (bytes == 0) { fclose(f); continue; }
        i16 *const data = static_cast<i16 *>(linearAlloc(bytes));
        if (!data) { fclose(f); log("load_sfx: %s -- linearAlloc(%lu) failed", path,
                                      static_cast<unsigned long>(bytes)); continue; }

        const bool ok = fread(data, 1, bytes, f) == bytes;
        fclose(f);
        if (!ok) { linearFree(data); log("load_sfx: %s -- short read", path); continue; }

        // Once here, at load, rather than per play: these bytes are never
        // written again.
        DSP_FlushDataCache(data, bytes);
        sfx_info[i] = { data, bytes / BYTES_PER_FRAME };
        log("load_sfx: %s -- %lu frames", path,
              static_cast<unsigned long>(sfx_info[i].frames));
    }
}

// Configures one effect voice. Separate from the music's because the rate is
// the build's constant here (::SFX_RATE) rather than a container's, and linear
// interpolation is enough for a voice that never resamples.
void setup_sfx_voice(const int channel) {
    const int id = SFX_VOICE_BASE + channel;
    ndspChnReset(id);
    ndspChnSetInterp(id, NDSP_INTERP_LINEAR);
    ndspChnSetRate(id, SFX_RATE);
    ndspChnSetFormat(id, NDSP_FORMAT_STEREO_PCM16);
}

/**
 * @brief Leaves a player-readable note on the SD card when the DSP is absent.
 *
 * THE ONE DIAGNOSTIC THAT SURVIVES A RELEASE BUILD, and the asymmetry with
 * every log() above is deliberate. Those are for whoever is working on this
 * backend, and logger.hpp compiles them out of a release .3dsx as it should.
 * This one is for a PLAYER: a missing dspfirm.cdc is not a bug they can see,
 * it is a game that boots perfectly and makes no sound, with the fix being a
 * file they have never heard of. Mute with no explanation anywhere is the one
 * failure this backend should not ship.
 *
 * A FILE AND NOT A SCREEN, because the screens are the game's: both belong to
 * citro2d (src/3ds/video.cpp) and the demo paints a nametable through its own
 * scene system, so there is no "draw one line of text anywhere" primitive to
 * reach for -- and inventing one, for an error, in the audio backend, is the
 * wrong place to put it. The SD card is where the missing file goes, so it is
 * also where someone looking for it will be.
 *
 * WRITTEN ONLY ON FAILURE, so a working install never gains a file: nothing
 * here runs unless ndspInit has already refused. It is rewritten (not
 * appended) each time, so it cannot grow, and it is plain text naming the path
 * and the dump route rather than a result code nobody can look up.
 */
void note_missing_dsp_firmware(const Result rc) {
    FILE *const f = std::fopen("sdmc:/pnes_no_audio.txt", "w");
    if (!f) return;
    std::fprintf(f,
        "pnes: no audio on this 3DS.\n\n"
        "ndspInit() failed (0x%08lX): the DSP firmware is missing.\n"
        "libctru loads it from  sdmc:/3ds/dspfirm.cdc  and there is no\n"
        "software fallback, so the game runs silently without it.\n\n"
        "To fix it, dump the firmware from this console once:\n"
        "  Luma3DS: Rosalina menu -> Miscellaneous options -> Dump DSP firmware\n"
        "  or run the DSP1 homebrew app, which writes the same file.\n\n"
        "The dump stays valid forever and every homebrew game with sound\n"
        "uses it -- it is not specific to this one.\n",
        static_cast<unsigned long>(rc));
    std::fclose(f);
}

} // namespace

void audio::Init(u8) {
    // CHECKED AND LOGGED, not called and hoped for: both assets live in the
    // RomFS, so a failed mount is total silence with no other symptom -- and
    // for a .3dsx the mount reads the running file through argv[0], which is
    // the loader's to provide and not this build's.
    const Result romfs_rc = romfsInit();
    log("Init: romfsInit -> 0x%08lX", static_cast<unsigned long>(romfs_rc));

    // Needs sdmc:/3ds/dspfirm.cdc. No software path exists to fall back to, so
    // a failure here leaves every entry point below inert -- but it does NOT
    // leave the player guessing; see ::note_missing_dsp_firmware.
    const Result ndsp_rc = ndspInit();
    log("Init: ndspInit -> 0x%08lX", static_cast<unsigned long>(ndsp_rc));
    if (R_FAILED(ndsp_rc)) {
        note_missing_dsp_firmware(ndsp_rc);
        return;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);

    load_sfx();
    for (int i = 0; i < N_SFX_VOICES; i++) setup_sfx_voice(i);

    // Voice 0 is configured per track open (::open_track), but give it the
    // build's rate and the current volume now so a TrackVolume set before Init
    // -- which audio.hpp permits -- has somewhere to land.
    setup_music_voice(SFX_RATE);

    // EFFECTS ARE LIVE FROM HERE, before a single line of the music path runs.
    // See ::ndsp_ready: an effect is a buffer and a voice, and neither of those
    // has anything to do with the chunk buffers or the decode thread below.
    ndsp_ready = true;

    for (int i = 0; i < N_BUF; i++) {
        chunk[i] = static_cast<i16 *>(linearAlloc(CHUNK_FRAMES * BYTES_PER_FRAME));
        if (!chunk[i]) { log("Init: music chunk linearAlloc failed"); return; }
        memset(chunk[i], 0, CHUNK_FRAMES * BYTES_PER_FRAME);
        wavebuf[i] = {};
        wavebuf[i].status = NDSP_WBUF_DONE;   // free for the thread's first pass
    }

    LightEvent_Init(&audio_event, RESET_ONESHOT);
    ndspSetCallback(ndsp_frame, nullptr);

    // One step above whatever priority the loader gave main, rather than a
    // literal: a .3dsx is launched at a different priority than a CIA would be,
    // so a hardcoded number is either needlessly high or silently lower than
    // the thread it has to stay ahead of. Clamped to the top of the range an
    // application may ask for (0x18).
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    prio -= 1;
    if (prio < 0x18) prio = 0x18;

    running = true;
    // 64KB of stack, matching the PSP's decode thread: stb_vorbis's pulldata
    // path is not frugal. Default core, which on an o3DS is the only one an
    // application gets.
    athread = threadCreate(audio_thread, nullptr, 64 * 1024, prio, -1, false);
    if (!athread) { running = false; log("Init: threadCreate(prio %ld) failed",
                                           static_cast<long>(prio)); return; }

    music_ready = true;
    log("Init: ready (sfx + music), thread prio %ld", static_cast<long>(prio));
}

void audio::Update() {
    // Streaming is driven by the ndsp callback and the decode thread; nothing
    // to do per frame.
}

void audio::TrackPlay(const u8 index) {
    // Does NOT touch `vorbis` or `loop_frame` directly -- only audio_thread()
    // may call into the decoder; see ::pending_track.
    if (!music_ready || index >= nTracks) return;
    pending_track = index;
}

void audio::TrackStop() {
    playing = false;
}

// Applied by the DSP on voice 0 alone, which is what keeps it off the effects
// (audio.hpp's "music only"): they are their own voices with their own mixes.
// Takes effect on the next DSP frame, not on the next chunk.
void audio::TrackVolume(const u8 percent) {
    volume = percent > 100 ? 100 : percent;
    if (ndsp_ready) set_voice_level(MUSIC_VOICE, static_cast<float>(volume) / 100.0f);
}

// The hardware's own pause bit, so this is exact and instant: the queued
// buffers stay queued and the voice resumes from the sample it stopped on.
void audio::TrackPause(const u8 pause) {
    (void)pause;  // match the SDL/Switch/PSP backends: toggle regardless of argument
    if (!music_ready) return;
    music_paused = !music_paused;
    ndspChnSetPaused(MUSIC_VOICE, music_paused);
}

// @p volume is a linear amplitude coefficient here -- 0 silence, 255 unity --
// this backend's reading of the byte audio.hpp deliberately leaves
// uninterpreted, and the same one every other mixing backend uses. Applied as
// this voice's mix, which the DSP holds for the life of the effect: a level
// change mid-effect belongs to the NEXT effect, exactly as audio.hpp describes.
void audio::SfxPlay(const u8 index, const u8 channel, const u8 volume) {
    // THE FIRST FEW CALLS ONLY, and every rejection among them: the three ways
    // this returns without a sound (never reached, nothing loaded, level zero)
    // are indistinguishable from the outside, and the level is the application's
    // slider rather than anything this file picks -- a muted slider looks
    // exactly like a broken backend. Capped rather than gated on a separate
    // switch because each line costs an SD write, and a menu plays these by the
    // dozen.
#ifndef NDEBUG
    static int logged = 0;
    if (logged < 8) {
        logged++;
        log("SfxPlay: index=%u channel=%u volume=%u ndsp_ready=%d nSfx=%u frames=%lu",
              static_cast<unsigned>(index), static_cast<unsigned>(channel),
              static_cast<unsigned>(volume), ndsp_ready ? 1 : 0,
              static_cast<unsigned>(nSfx),
              static_cast<unsigned long>(index < nSfx ? sfx_info[index].frames : 0));
    }
#endif
    if (!ndsp_ready || index >= nSfx || channel >= N_SFX_VOICES) return;
    if (!sfx_info[index].frames) return;        // not in the image

    const int id = SFX_VOICE_BASE + channel;

    // Starting on a busy voice cuts the old effect off, as audio.hpp's channel
    // contract requires -- and clears the pause bit with it, so a voice left
    // paused by ::audio::SfxPause cannot swallow the next effect played on it.
    //
    // ONLY WHEN THERE IS SOMETHING TO CUT, THOUGH, and that conditional is a
    // latency fix rather than a saved call. ndspChnWaveBufClear bumps the
    // channel's syncCount (libctru's ndsp-channel.c), which the DSP has to
    // acknowledge before the queue it is handed next takes effect -- so an
    // unconditional clear makes every effect wait out a sync round-trip it did
    // not need. An idle voice has nothing queued and no pause bit worth
    // clearing, so the common case -- an effect that finished before the next
    // one was asked for -- now goes straight to the add.
    if (ndspChnIsPlaying(id)) {
        ndspChnWaveBufClear(id);
        ndspChnSetPaused(id, false);
    }
    set_voice_level(id, static_cast<float>(volume) / 255.0f);

    sfx_wavebuf[channel] = {};
    sfx_wavebuf[channel].data_pcm16 = sfx_info[index].data;
    sfx_wavebuf[channel].nsamples   = sfx_info[index].frames;
    sfx_wavebuf[channel].looping    = false;
    ndspChnWaveBufAdd(id, &sfx_wavebuf[channel]);
}

// Unity: there is no separate sample bank off NES to carry a level of its own
// (audio.hpp's ::audio::SfxSamplePlay), so this plays the asset as authored.
void audio::SfxSamplePlay(const u8 index) { audio::SfxPlay(index, 1, 0xFF); }

// Exact here, unlike the NES driver: a voice is the DSP's, so dropping its
// queue or freezing it is one call (see audio.hpp's ::audio::SfxStop on why the
// NES cannot). Both ignore an out-of-range channel rather than clamping into
// another slot; the range is audio::sfxChannels.
void audio::SfxStop(const u8 channel) {
    if (!ndsp_ready || channel >= N_SFX_VOICES) return;
    ndspChnWaveBufClear(SFX_VOICE_BASE + channel);
    ndspChnSetPaused(SFX_VOICE_BASE + channel, false);
}

// The voice keeps its cursor while paused and resumes from it, so this is
// genuinely distinct from ::audio::SfxStop -- and unlike the Switch's, it costs
// no state here at all.
void audio::SfxPause(const u8 channel, const u8 pause) {
    if (!ndsp_ready || channel >= N_SFX_VOICES) return;
    ndspChnSetPaused(SFX_VOICE_BASE + channel, pause != 0);
}
