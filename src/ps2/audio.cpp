/**
 * @file audio.cpp
 * @brief Music playback for the Sony PlayStation 2 backend (audsrv).
 *
 * The desktop pc_audio.wav is transcoded to a small Vorbis OGG at build time
 * and #embedded into the ELF (see the ps2 branch of CMakeLists.txt), decoded
 * here with stb_vorbis and streamed to audsrv.
 *
 * Playback runs on a dedicated EE thread, not from audio::Update() (which is
 * a no-op here): it decodes one fixed-size chunk, waits for audsrv's ring
 * buffer to have room for it, then audsrv_play_audio()s it, and repeats.
 *
 * The wait is audsrv_wait_audio(), as in ps2sdk's samples/rpc/audsrv/playwav/
 * playwav.c, followed by a short DelayThread() -- see the comment on that
 * call, which is load-bearing.
 *
 * Two alternatives were tried here and both produced total silence on
 * hardware; neither should be reintroduced without a way to test it in
 * isolation. (1) audsrv_on_fillbuf() firing a callback that signals a
 * semaphore, per that same directory's playwav2 sample: with an empty ring
 * and nothing yet queued there is no drain event, so the thread parks on its
 * first iteration and never feeds anything, which leaves nothing to drain.
 * (2) Dropping the DelayThread() on the assumption audsrv_wait_audio() yields
 * the CPU by itself.
 *
 * The thread runs at HIGHER priority than main. See the long comment in
 * Init() for why lower priority cannot work on this backend -- briefly, the
 * render loop has no slack to donate reliably, so a thread that can only run
 * in main's idle window starves exactly when the frame is heaviest.
 *
 * Ownership rule, relied on throughout: the audio thread is the ONLY thread
 * that calls any stb_vorbis_* or audsrv_* function. That matters for both --
 * stb_vorbis's decoder object is not safe to call into from two threads (see
 * ::pending_track), and audsrv's EE-side RPC client is a single
 * SifRpcClientData, so two threads calling into it would interleave on one
 * structure. The public entry points below only ever set flags.
 *
 * audsrv is NOT a BIOS-resident module (unlike SIO2MAN/PADMAN in src/ps2/
 * input.cpp) -- it's a ps2sdk-supplied IOP driver, so the prebuilt audsrv.irx
 * ps2sdk ships is #embedded into the ELF the same way the OGG is, and loaded
 * at runtime with SifExecModuleBuffer instead of SifLoadModule's rom0: path.
 * LIBSD (the SPU2 driver audsrv itself depends on) IS BIOS-resident.
 *
 * The decode cost this file spends is avoidable, not inherent: the SPU2 has
 * 2MB of its own sound RAM and decodes ADPCM in hardware, and audsrv exposes
 * it (audsrv_load_adpcm/audsrv_ch_play_adpcm, with a `loop` field in
 * audsrv_adpcm_t and 24 voices). Music as SPU2 ADPCM costs the EE nothing per
 * frame -- no Vorbis, no decode thread, no priority tradeoff against
 * rendering, and it is how PS2 titles actually did this. That is the real fix
 * for the frame-rate cost; everything in this file is a way of paying a bill
 * that does not have to be paid.
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
#include <delaythread.h>
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
// DMAs it to the IOP. A plain `unsigned char[]` has no alignment of its own
// (1 byte), so whatever address it landed on was pure luck of the compiler's
// .rodata layout; a non-LTO per-TU debug build and a whole-program LTO
// release build pack .rodata differently, so "it happened to land aligned"
// was never something either build actually guaranteed -- an unlucky
// release layout here left the module looking like it loaded while never
// actually starting on the IOP, hanging audsrv_init() forever.
alignas(16) static const unsigned char audsrv_irx[] = {
#embed <audsrv.irx>
};
static constexpr int audsrv_irx_len = static_cast<int>(sizeof(audsrv_irx));

static constexpr int SAMPLE_RATE = 44100;
static constexpr int CHANNELS    = 2;

// One audsrv chunk per decode/play cycle, ~46ms at 44.1kHz stereo 16-bit.
//
// Nothing here is derived from the frame rate -- that is the point, and it is
// what made the audsrv_available()/UPDATE_STRIDE revision this replaces need
// retuning whenever frame timing moved. It does set TrackStop/TrackPause
// latency, since that is how long the thread can be mid-cycle when a flag is
// posted.
//
// 1024 (4KB, a better fit for the EE's 8KB two-way D-cache alongside
// stb_vorbis's working set) was tried and is almost certainly the better
// number, but it was changed in the same pass as the pacing rewrite below and
// so has never been verified in isolation on hardware. Left at the value that
// is known to produce sound; revisit it on its own.
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

// alignas(16): handed to audsrv_play_audio(), which DMAs it to the IOP over
// SIF the same way audsrv_irx above is.
alignas(16) s16 chunk[CHUNK_FRAMES * CHANNELS];

stb_vorbis *vorbis     = nullptr;
u32         loop_frame = 0;      // stb_vorbis_seek() target on EOF; audio thread only
bool        running    = false;

// Written by main (TrackStop clears `playing`, TrackPause toggles `paused`)
// and by the audio thread (which sets both when it applies a pending track).
// The two never contend for the same transition -- main only ever stops or
// pauses, the thread only ever starts -- and a bool can't tear, so there is
// nothing here to lose. volatile, not std::atomic, for the reason spelled
// out under ::pending_track below.
volatile bool playing = false;
volatile bool paused  = false;

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

// Fills chunk[] with CHUNK_FRAMES stereo frames, looping back to loop_frame
// on EOF (mirrors src/psp/audio.cpp's fill()). stalls bails out on a
// degenerate/empty stream instead of spinning.
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
            // Deliberately does NOT audsrv_stop_audio() here to drop the
            // silence this loop queues while idle. audsrv documents stop as
            // "stops audio from playing" and says nothing about what state it
            // leaves the SPU2 transfer in, so calling it on the path that
            // *starts* music risks trading a short startup delay for no music
            // at all. The cost of not flushing is bounded by how much silence
            // is queued, and level.cpp calls TrackPlay immediately after
            // audio::Init, so in practice almost none is.
            stb_vorbis_seek_start(vorbis);
            loop_frame = tracks[req].loop_start > 0.0f
                ? static_cast<u32>(tracks[req].loop_start * static_cast<float>(SAMPLE_RATE))
                : 0;
            playing = true;
            paused  = false;
        }

        if (!vorbis || !playing || paused) {
            // Silence rather than an early-out: the wait below is this
            // thread's only pacing source, and it is driven by audsrv
            // *consuming* data. Going quiet here instead would leave nothing
            // to drain, so no fill-buffer callback would ever fire again and
            // this loop would have nothing to block on.
            memset(chunk, 0, sizeof(chunk));
        } else {
            fill();
        }

        // Blocks until audsrv's ring buffer has room for this chunk -- the
        // wait that paces the whole thread at the SPU2's real consumption
        // rate, with no clock, sleep, poll or stride of its own. It is also
        // what stops a higher-priority thread dominating main: it holds the
        // CPU for one decode, then parks here.
        audsrv_wait_audio(CHUNK_BYTES);
        audsrv_play_audio(reinterpret_cast<const char *>(chunk), CHUNK_BYTES);

        // Load-bearing, and not for the reason the removed comment here used
        // to give. Removing this (on the theory that audsrv_wait_audio()
        // above is itself a kernel block, so a higher-priority thread parked
        // in it could not starve main) produced total silence on hardware --
        // so audsrv_wait_audio() evidently does NOT reliably yield the EE to
        // a lower-priority thread. DelayThread() is a genuine kernel-blocking
        // sleep, so this is what actually forces a deterministic yield window
        // back to main every chunk. One chunk is ~46ms of playback; 2ms here
        // is a small fraction of that, nowhere near enough to risk an audsrv
        // ring-buffer underrun.
        //
        // It is also why this thread costs frame rate: 2ms per chunk is the
        // floor on what main gets back, and the Vorbis decode happens at
        // higher priority than rendering. Fixing that properly means not
        // decoding on the EE at all -- see the SPU2 ADPCM note below.
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
    thread.func       = reinterpret_cast<void *>(audio_thread);
    thread.stack      = audio_thread_stack;
    thread.stack_size = sizeof(audio_thread_stack);
    thread.gp_reg     = &_gp;

    // Strictly HIGHER priority than main (smaller number = more urgent).
    //
    // A lower-priority decode thread does NOT work here, and the reason is
    // worth recording because it looks like it should. Making the render loop
    // block in a real kernel wait (src/ps2/video.cpp's ::vblank_sema) is what
    // lets the kernel switch away from main at all, but it only donates time
    // that actually exists: WaitSema returns immediately whenever a vblank is
    // already pending, which is precisely what happens when a frame overruns
    // its 16.6ms budget. This backend rasterises the NES frame in software,
    // channel-swaps 61440 words and DMAs a 240KB texture every frame, so the
    // slack is small and not guaranteed to exist at all in a heavy scene. A
    // thread that can only run in that window therefore starves exactly when
    // it must not, and music stops. Every revision of this backend in which
    // audio actually played had this thread above main.
    //
    // Being above main is safe because the thread holds the CPU only for one
    // chunk's decode and then blocks in the kernel (see ::fill_sema): its
    // total share is the decode cost, which is irreducible work that has to
    // be paid somewhere. Paying it here costs a small, constant slice per
    // chunk; paying it on the render thread -- what the audsrv_available()/
    // UPDATE_STRIDE revision this replaces did -- cost a ~67ms decode burst
    // inside one frame out of every four.
    //
    // Queried rather than hardcoded: a flat constant can't know where main
    // actually sits, and the relationship to main is the entire point. The
    // -5/floor-0 shape is the one that has been exercised on hardware.
    ee_thread_status_t mainStatus{};
    const s32 referResult  = ReferThreadStatus(GetThreadId(), &mainStatus);
    const s32 mainPriority = referResult >= 0 ? mainStatus.current_priority : 50;
    thread.initial_priority = mainPriority > 5 ? mainPriority - 5 : 0;
    thread.attr             = 0;
    thread.option           = 0;

    const s32 tid = CreateThread(&thread);
    if (tid < 0) {
        log("audio::Init: CreateThread failed (%d), no music", tid);
        running = false;
        return;
    }
    StartThread(tid, nullptr);
}

void audio::Update() {
    // Streaming is driven by the dedicated audio thread; nothing to do per
    // frame. Deliberately NOT a decode entry point -- see the file header for
    // what putting Vorbis decode on this call cost.
}

void audio::TrackPlay(const u8 index) {
    // Does NOT touch `vorbis` or `loop_frame` directly -- only audio_thread()
    // may call into the decoder; see ::pending_track.
    if (!vorbis || index >= nTracks) return;
    pending_track = index;
}

void audio::TrackStop() {
    // No audsrv_stop_audio() here: whatever is already queued plays out, up to
    // one chunk of it. Flushing would be tighter, but audsrv documents stop
    // only as "stops audio from playing" and says nothing about the state it
    // leaves the SPU2 transfer in, and no build of this backend that actually
    // produced sound has ever called it -- not worth risking the start path
    // on. The thread feeds silence from here on; see audio_thread.
    playing = false;
}

void audio::TrackPause(const u8 pause) {
    (void)pause;  // match the SDL/Switch/Wii U/PSP backends: toggle regardless of argument
    paused = !paused;
}

// See the file header: no non-NES SFX table exists in this project yet.
void audio::SfxPlay(u8 /*index*/, u8 /*channel*/) {}
void audio::SfxSamplePlay(u8 /*index*/) {}
