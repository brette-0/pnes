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
#include <sbv_patches.h>
#include <delaythread.h>
#include <cstring>

#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include <stb_vorbis.c>

// MUST come after stb_vorbis.c: that drags in <math.h>, and libstdc++'s
// <cmath> #undefs any macro named `log` before declaring its own
// std::log(double) overloads (the two would otherwise collide) -- defining
// ::log() before that point, not after, left it silently erased for the
// rest of this file the moment stb_vorbis.c's own #include <math.h> ran.
#include <platform-nes/logger.hpp>

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
//
// alignas(16): this buffer is handed straight to SifExecModuleBuffer, which
// DMAs it to the IOP -- same alignment requirement as ::chunk/::audio_thread
// _stack below, which already carry it. A plain `unsigned char[]` has no
// alignment of its own (1 byte), so whatever address it landed on was pure
// luck of the compiler's .rodata layout; a non-LTO per-TU debug build and a
// whole-program LTO release build pack .rodata differently, so "it happened
// to land aligned" was never something either build actually guaranteed --
// an unlucky release layout here left the module looking like it loaded
// while never actually starting on the IOP, hanging audsrv_init() forever.
alignas(16) static const unsigned char audsrv_irx[] = {
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

// TrackPlay() runs on the main thread while audio_thread() is concurrently
// mid-decode on `vorbis` -- stb_vorbis's decoder object is not safe to call
// into from two threads at once (a seek landing mid stb_vorbis_get_samples_
// short_interleaved() call can corrupt its internal state, after which every
// decode call returns 0 samples forever and fill() below zero-fills every
// chunk from then on: permanent silence with no error, no crash).
//
// So TrackPlay() never touches `vorbis` itself: it only posts the requested
// index here, and audio_thread() -- the only thread allowed to call any
// stb_vorbis_* function -- applies it at the top of its own loop. -1 means
// no request pending.
//
// Plain `volatile int`, not std::atomic: the EE has no hardware word-CAS
// (that's what std::atomic<int>::exchange needs under the hood), so linking
// it drags in libatomic, which this toolchain doesn't have wired up. A
// naturally-aligned int is already indivisible on this single-core CPU --
// `volatile` is only here to stop the compiler from caching either side's
// read/write in a register -- and with exactly one writer (TrackPlay, on the
// main thread) and one reader (audio_thread, which is also the only one that
// ever clears it back to -1), there's no partial-update to race on.
volatile int pending_track = -1;

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
            memset(chunk, 0, sizeof(chunk));
        } else {
            fill();
        }
        // Blocks until audsrv's ring buffer has room for this chunk -- the
        // call that paces this whole thread at the hardware's real playback
        // rate, no sleep/poll needed (see the file header).
        audsrv_wait_audio(CHUNK_BYTES);
        audsrv_play_audio(reinterpret_cast<const char *>(chunk), CHUNK_BYTES);

        // This thread has to run at higher priority than main just to be
        // scheduled at all (see the priority comment in Init() -- equal or
        // lower priority left it sitting in THS_READY forever, since
        // nothing here makes the main/render loop yield through a real
        // kernel wait the scheduler recognises). Being strictly higher
        // priority then meant it could fully preempt the render thread
        // instead of sharing time with it, dropping frame rate in-level.
        // DelayThread() is a genuine kernel-blocking sleep (unlike a
        // busy-loop), so this forces a deterministic yield window back to
        // main every chunk regardless of what audsrv_wait_audio() or
        // gsKit_sync_flip() do internally. One chunk is ~46ms of playback
        // (2048 frames / 44100Hz); 2ms here is a small fraction of that
        // budget, nowhere near enough to risk an audsrv ring-buffer
        // underrun.
        DelayThread(2000);
    }
    ExitThread();
}

alignas(16) u8 audio_thread_stack[0x4000];

} // namespace

void audio::Init(u8) {
    SifLoadModule("rom0:LIBSD", 0, nullptr);

    // rom0:LOADFILE's default RPC service doesn't support loading a module
    // from an EE RAM buffer at all (only from a real path) -- without this
    // patch, SifExecModuleBuffer below *looks* like it succeeds (it has no
    // way to report otherwise) but audsrv.irx never actually starts running
    // on the IOP, so audsrv_init()'s RPC bind to it then blocks forever with
    // no timeout. sbv_patch_disable_prefix_check() separately disables
    // MODLOAD's own protected-device allowlist, which can otherwise block
    // loading an unprotected executable from a source (an EE RAM buffer)
    // it doesn't recognise.
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();

    SifExecModuleBuffer(const_cast<unsigned char *>(audsrv_irx), audsrv_irx_len, 0, nullptr, nullptr);

    if (audsrv_init() != 0) {
        log("audio::Init: audsrv_init failed, bailing");
        return;
    }

    audsrv_fmt_t format;
    format.bits     = 16;
    format.freq     = SAMPLE_RATE;
    format.channels = CHANNELS;
    audsrv_set_format(&format);
    audsrv_set_volume(MAX_VOLUME);

    int err = 0;
    vorbis = stb_vorbis_open_memory(ogg_data, ogg_len, &err, nullptr);
    if (!vorbis) {
        log("audio::Init: vorbis open failed, err=%d, bailing", err);
        return;
    }

    if (nTracks > 0) {
        loop_frame = tracks[0].loop_start > 0.0f
            ? static_cast<u32>(tracks[0].loop_start * static_cast<float>(SAMPLE_RATE))
            : 0;
    }

    running = true;

    // Zero-initialized, not just field-by-field: ee_thread_t::status (the
    // struct's first member) is an output-only field CreateThread fills in,
    // but it's never set here before the call, so it was left as whatever
    // garbage happened to be on the stack -- if that garbage landed on a
    // recognized nonzero THS_* status value, the kernel could plausibly
    // treat this as describing an already-live thread instead of a new one.
    ee_thread_t thread{};
    thread.func             = reinterpret_cast<void *>(audio_thread);
    thread.stack            = audio_thread_stack;
    thread.stack_size       = sizeof(audio_thread_stack);
    thread.gp_reg           = &_gp;

    // A flat priority constant here (guessed, with no idea what the main
    // thread's own priority actually is) left this thread sitting in
    // THS_READY forever: the EE kernel never preempts a strictly-lower-
    // priority (bigger number = less urgent) thread in favour of one that's
    // higher priority and never actually blocks through a real kernel wait
    // call -- and gsKit_sync_flip() in the main render loop apparently just
    // polls a GS status register rather than doing that. Equal priority
    // produced the exact same silent starvation (this kernel/config isn't
    // timer-preempting same-priority ready threads into a round robin
    // either), so this has to be genuinely more urgent than main just to
    // run at all; DelayThread() in its own loop above is what keeps it from
    // then dominating the render thread in return.
    ee_thread_status_t mainStatus{};
    const s32 referResult = ReferThreadStatus(GetThreadId(), &mainStatus);
    const s32 mainPriority = referResult >= 0 ? mainStatus.current_priority : 50;
    thread.initial_priority = mainPriority > 5 ? mainPriority - 5 : 0;
    thread.attr             = 0;
    thread.option           = 0;

    const s32 tid = CreateThread(&thread);
    if (tid >= 0) StartThread(tid, nullptr);
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
    (void)pause;  // match the SDL/Switch/Wii U/PSP backends: toggle regardless of argument
    paused = !paused;
}

// See the file header: no non-NES SFX table exists in this project yet.
void audio::SfxPlay(u8 /*index*/, u8 /*channel*/) {}
void audio::SfxSamplePlay(u8 /*index*/) {}
