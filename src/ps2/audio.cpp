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
// TEMPORARY: bisecting the same release-only hang video.cpp's own
// video::DebugFlash() is already being used for (see its header comment) --
// log() below is a no-op in a release build (NDEBUG), so DebugFlash() is the
// only checkpoint signal that survives there. irq::init() ends by firing one
// flash; main.cpp's RESET body fires another right after this file's
// audio::Init() returns. Confirmed the build only ever reaches the FIRST of
// those two, i.e. the hang is somewhere inside audio::Init() itself -- so
// this sprinkles one flash after each of its blocking calls to narrow down
// which one never returns. Counting total flashes seen (including irq::
// init()'s own) localizes it: irq::init()'s flash is #1, and each checkpoint
// below is the next consecutive number in source order.
#include <platform-nes/video.hpp>

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
// to land aligned" was never something either build actually guaranteed.
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

// TEMPORARY: boot now reaches title, but tracks are still silent in-level.
// These three single-shot flags let audio_thread() (which must never call
// video::DebugFlash() itself -- it isn't safe to touch gsKit's state from a
// thread other than the one driving present_frame()/WaitForPresent(), see
// main.cpp's ::audio::Update() which is what actually fires the flash)
// report back whether TrackPlay()'s request was ever applied, and whether
// the FIRST real (non-silence-filler) audsrv_play_audio() call reported
// success or failure. -1 = not yet observed.
volatile int debug_track_applied = -1;     // becomes the applied track index once seen
volatile int debug_first_play_result = -0x7fffffff;   // becomes audsrv_play_audio()'s actual return
// CreateThread()/StartThread() both reported success (confirmed: blue
// flash, no orange/yellow), yet no white "track applied" flash ever fires
// in-level -- so it's still unconfirmed that audio_thread() ever executes
// even its first loop iteration (a crash on entry, e.g. a bad ::_gp/stack
// setup that only misbehaves under -O3/-flto, would look identical from
// the outside: thread "started" successfully as far as the kernel's
// bookkeeping goes, then silently dies). This is set true before anything
// else happens on the very first iteration, independent of TrackPlay()
// ever being called at all.
volatile bool debug_thread_alive = false;
// TEMPORARY: confirmed no purple flash either -- audio_thread() never
// executes even its first statement despite CreateThread()/StartThread()
// both reporting success. Stored here so audio::Update() (main thread) can
// ask the kernel directly, via ReferThreadStatus(), what state the thread
// is actually sitting in, instead of guessing blind.
volatile s32 debug_audio_tid = -1;

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
        debug_thread_alive = true;   // TEMPORARY: see its own declaration

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
            debug_track_applied = req;   // TEMPORARY: see its own declaration
        }

        const bool realChunk = vorbis && playing && !paused;
        if (!realChunk) {
            memset(chunk, 0, sizeof(chunk));
        } else {
            fill();
        }
        // Blocks until audsrv's ring buffer has room for this chunk -- the
        // call that paces this whole thread at the hardware's real playback
        // rate, no sleep/poll needed (see the file header).
        audsrv_wait_audio(CHUNK_BYTES);
        const int playResult = audsrv_play_audio(reinterpret_cast<const char *>(chunk), CHUNK_BYTES);
        // TEMPORARY: capture the first real (non-silence-filler) call's
        // result once -- see ::debug_first_play_result's own declaration.
        if (realChunk && debug_first_play_result == -0x7fffffff) {
            debug_first_play_result = playResult;
        }
    }
    ExitThread();
}

alignas(16) u8 audio_thread_stack[0x4000];

} // namespace

void audio::Init(u8) {
    log("audio::Init: entered");
    SifLoadModule("rom0:LIBSD", 0, nullptr);
    log("audio::Init: SifLoadModule(LIBSD) returned");
    video::DebugFlash();   // checkpoint #2: SifLoadModule("rom0:LIBSD") returned

    // rom0:LOADFILE's default RPC service doesn't support loading a module
    // from an EE RAM buffer at all (only from a real path) -- without this
    // patch, SifExecModuleBuffer below *looks* like it succeeds (it has no
    // way to report otherwise) but audsrv.irx never actually starts running
    // on the IOP, so audsrv_init()'s RPC bind to it then blocks forever with
    // no timeout. sbv_patch_disable_prefix_check() separately disables
    // MODLOAD's own protected-device allowlist, which can otherwise block
    // loading an unprotected executable from a source (an EE RAM buffer)
    // it doesn't recognise.
    const int lmbPatchResult = sbv_patch_enable_lmb();
    const int prefixPatchResult = sbv_patch_disable_prefix_check();
    log("audio::Init: lmbPatchResult=%d prefixPatchResult=%d", lmbPatchResult, prefixPatchResult);
    video::DebugFlash();   // checkpoint #3: both sbv_patches applied

    int audsrvModRes = -999;
    const int audsrvExecResult = SifExecModuleBuffer(const_cast<unsigned char *>(audsrv_irx), audsrv_irx_len, 0, nullptr, &audsrvModRes);
    log("audio::Init: SifExecModuleBuffer result=%d modRes=%d", audsrvExecResult, audsrvModRes);
    // checkpoint #4a/#4b: confirmed hanging between here and #5 (inside
    // audsrv_init() itself) with no visibility into *why* -- these two
    // colour-coded flashes carry the two return values log() can't show in
    // release. Green = that result looked like success (>= 0); red = it
    // didn't. #4a is SifExecModuleBuffer's own return value (the EE-side
    // DMA/RPC call); #4b is audsrvModRes, the IOP-side module's own result
    // from actually running (what the file header's "*looks* like it
    // succeeds" warning is about -- #4a can be green while #4b is red).
    video::DebugFlash(audsrvExecResult < 0 ? 0xFF : 0x00, audsrvExecResult < 0 ? 0x00 : 0xFF, 0x00);
    video::DebugFlash(audsrvModRes     < 0 ? 0xFF : 0x00, audsrvModRes     < 0 ? 0x00 : 0xFF, 0x00);

    // TEMPORARY: audsrvModRes >= 0 (checkpoint #4b, above) means audsrv.irx's
    // own _start() really did run and return success on the IOP -- so the
    // module loaded fine. audsrv_init() then hangs forever anyway (no
    // return, no timeout -- see this function's own comment on
    // sbv_patch_enable_lmb() for that exact failure shape). The likely gap:
    // _start() returning isn't the same as the module's RPC server thread
    // having finished sceSifRegisterRpc() yet -- a startup race audsrv_init()
    // apparently has no retry/backoff for. In the debug build this never
    // showed up because every sio_puts() log() call between here and there
    // burns tens of milliseconds over the real serial line, accidentally
    // giving the module time to finish registering before audsrv_init() ever
    // asks. This burns the same kind of wall-clock time on purpose instead
    // of by accident -- gsKit_sync_flip-paced, no visible flash (black on
    // black) -- to see if it's really just a race and not a hard failure.
    // 3 calls * 40 vsyncs each =~ 2s at 60Hz: generous but not a real wait.
    for (int i = 0; i < 3; i++) video::DebugFlash(0x00, 0x00, 0x00);

    if (audsrv_init() != 0) {
        log("audio::Init: audsrv_init failed, bailing");
        // TEMPORARY: this early return used to be silent in release (log()
        // is a no-op there) -- title still reaches the screen either way
        // since main.cpp's own post-Init flash fires unconditionally, so
        // reaching title proves Init() RETURNED, not that it succeeded.
        // Distinct colour from checkpoint #4's audsrvExecResult/modRes
        // flashes (magenta, not plain red) so it can't be confused with
        // those -- this is audsrv_init() itself failing, after the alignment
        // fix already got SifExecModuleBuffer/the module's own _start() past
        // where they used to hang.
        video::DebugFlash(0xFF, 0x00, 0xFF);
        return;
    }
    log("audio::Init: audsrv_init succeeded");
    video::DebugFlash();   // checkpoint #5: audsrv_init() succeeded

    audsrv_fmt_t format;
    format.bits     = 16;
    format.freq     = SAMPLE_RATE;
    format.channels = CHANNELS;
    audsrv_set_format(&format);
    audsrv_set_volume(MAX_VOLUME);
    video::DebugFlash();   // checkpoint #6: audsrv_set_format/set_volume returned

    int err = 0;
    vorbis = stb_vorbis_open_memory(ogg_data, ogg_len, &err, nullptr);
    if (!vorbis) {
        log("audio::Init: vorbis open failed, err=%d, bailing", err);
        // TEMPORARY: same reasoning as the audsrv_init() failure flash
        // above -- cyan, not magenta, so the two early-return paths are
        // distinguishable from each other and from checkpoint #4's flashes.
        video::DebugFlash(0x00, 0xFF, 0xFF);
        return;
    }
    log("audio::Init: vorbis opened");
    video::DebugFlash();   // checkpoint #7: stb_vorbis_open_memory returned

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

    // Confirmed via ReferThreadStatus() from the main loop: a flat
    // initial_priority = 50 here left the thread sitting in THS_READY
    // forever -- the EE kernel never preempts a strictly-lower-priority
    // (bigger number = less urgent) thread in favour of one that's higher
    // priority and never actually blocks through a real kernel wait call,
    // and gsKit_sync_flip() in the main render loop apparently just polls a
    // GS status register rather than doing that. 50 was a guessed constant
    // with no idea what the main thread's own priority actually is, so
    // instead of guessing again, ask: a few points more urgent than
    // whatever's calling Init() (this runs on the main thread, so that's
    // the main thread's own priority) guarantees audio_thread() is always
    // at least as eligible to run, and its own blocking call
    // (audsrv_wait_audio(), in its loop) is what yields it back promptly --
    // same shape as every other backend's audio thread, just finally with a
    // priority that doesn't starve it.
    ee_thread_status_t mainStatus{};
    const s32 mainTid = GetThreadId();
    const s32 referResult = ReferThreadStatus(mainTid, &mainStatus);
    const s32 mainPriority = referResult >= 0 ? mainStatus.current_priority : 50;
    thread.initial_priority = mainPriority > 5 ? mainPriority - 5 : 0;
    thread.attr             = 0;
    thread.option           = 0;

    const s32 tid = CreateThread(&thread);
    debug_audio_tid = tid;   // TEMPORARY: see its own declaration
    log("audio::Init: CreateThread tid=%d", tid);
    // TEMPORARY: checkpoint #8 (the plain white flash below) fires no matter
    // what tid is -- it never actually proved CreateThread() succeeded, only
    // that Init() reached this line. No white "TrackPlay reached the audio
    // thread" flash ever fires in-level, which is exactly what you'd expect
    // if tid < 0 here: StartThread() is skipped, audio_thread() never runs,
    // and nothing is left to ever drain ::pending_track. Blue = tid >= 0
    // (thread genuinely created), orange = tid < 0 (CreateThread itself
    // failed -- distinct from every other colour used so far).
    video::DebugFlash(tid >= 0 ? 0x00 : 0xFF, tid >= 0 ? 0x00 : 0x80, tid >= 0 ? 0xFF : 0x00);
    s32 startResult = -1;
    if (tid >= 0) startResult = StartThread(tid, nullptr);
    log("audio::Init: StartThread result=%d", startResult);
    // Yellow = StartThread() itself reported failure (only reachable when
    // tid >= 0, i.e. the blue flash above already fired).
    if (tid >= 0 && startResult < 0) video::DebugFlash(0xFF, 0xFF, 0x00);
    log("audio::Init: fully completed");
    video::DebugFlash();   // checkpoint #8: CreateThread/StartThread returned, Init() about to return
}

void audio::Update() {
    // Streaming is driven by the dedicated audio thread; nothing to do per
    // frame -- except the TEMPORARY in-level silence diagnostic below.

    // TEMPORARY: audio_thread() cannot safely call video::DebugFlash()
    // itself (gsKit's state is only safe to touch from the one thread that
    // also drives present_frame()/WaitForPresent() -- this function, called
    // once per frame from the main loop, is that thread). These two
    // one-shot flags (set from audio_thread(), see their own declarations)
    // are drained here instead, each firing exactly once:
    //   - debug_track_applied: confirms TrackPlay()'s request actually
    //     reached audio_thread() (white flash). If this never fires after
    //     entering a level, the bug is upstream of this file entirely.
    //   - debug_first_play_result: the first REAL (non-silence-filler)
    //     audsrv_play_audio() call's own return value, colour-coded green
    //     (>= 0, looked like success) or red (< 0, it's failing outright,
    //     e.g. CHUNK_BYTES not fitting audsrv's ring buffer).
    //   - debug_thread_alive: confirms audio_thread() executed at least one
    //     loop iteration at all, independent of TrackPlay() ever being
    //     called (purple flash). If THIS never fires despite the boot-time
    //     blue/no-orange-no-yellow flashes already confirming CreateThread()
    //     /StartThread() both reported success, the thread is dying
    //     silently right after those kernel calls succeed -- a crash on
    //     entry (bad ::_gp/stack setup), not a logic bug in this loop.
    //   - debug_audio_tid + ReferThreadStatus(): confirmed no purple flash
    //     either, so ask the kernel directly what state the thread is
    //     actually in, rather than keep guessing. One-shot, a few frames in
    //     (give the thread a moment to actually get scheduled at least
    //     once before asking) so it's not just catching it still READY.
    //       DORMANT (gray)  -- thread exited/was never really running;
    //                          CreateThread/StartThread's success was a lie,
    //                          or it ran once and immediately ExitThread()'d.
    //       WAIT    (teal)  -- blocked on something (a semaphore/RPC call)
    //                          without ever reaching debug_thread_alive = true
    //                          first -- shouldn't be reachable given that's
    //                          the very first statement in the loop, but
    //                          would mean it died/blocked INSIDE that first
    //                          statement somehow.
    //       READY   (lime)  -- genuinely just starved: created, runnable,
    //                          but the scheduler has never actually given it
    //                          a timeslice -- a real priority/yield problem
    //                          in the main loop, not a crash.
    //       RUN     (pink)  -- was literally running at the exact instant
    //                          this sampled it, yet debug_thread_alive is
    //                          still false -- contradiction, means that flag
    //                          itself isn't being observed correctly.
    //       anything else   -- unexpected status bits; dumped as a raw
    //                          flash so it isn't silently misreported.
    static bool aliveFlashDone = false;
    static bool trackFlashDone = false;
    static bool playFlashDone  = false;
    static bool statusFlashDone = false;
    static int  framesSinceStart = 0;
    framesSinceStart++;
    if (!aliveFlashDone && debug_thread_alive) {
        aliveFlashDone = true;
        video::DebugFlash(0x80, 0x00, 0xFF);   // purple: audio_thread() is alive
    }
    if (!statusFlashDone && !aliveFlashDone && framesSinceStart == 30 && debug_audio_tid >= 0) {
        statusFlashDone = true;
        ee_thread_status_t status{};
        const s32 referResult = ReferThreadStatus(debug_audio_tid, &status);
        if (referResult < 0) {
            video::DebugFlash(0x40, 0x40, 0x40);   // dark gray: ReferThreadStatus() itself failed
        } else if (status.status & THS_DORMANT) {
            video::DebugFlash(0x80, 0x80, 0x80);   // gray: dormant
        } else if (status.status & THS_WAIT) {
            video::DebugFlash(0x00, 0x80, 0x80);   // teal: waiting
        } else if (status.status & THS_READY) {
            video::DebugFlash(0x80, 0xFF, 0x00);   // lime: starved, never scheduled
        } else if (status.status & THS_RUN) {
            video::DebugFlash(0xFF, 0x80, 0xC0);   // pink: running right now (contradiction)
        } else {
            video::DebugFlash(0x80, 0x40, 0x00);   // brown: unexpected status bits
        }
    }
    if (!trackFlashDone && debug_track_applied >= 0) {
        trackFlashDone = true;
        video::DebugFlash();   // white: TrackPlay() reached audio_thread()
    }
    if (!playFlashDone && debug_first_play_result != -0x7fffffff) {
        playFlashDone = true;
        const bool ok = debug_first_play_result >= 0;
        video::DebugFlash(ok ? 0x00 : 0xFF, ok ? 0xFF : 0x00, 0x00);
    }
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
