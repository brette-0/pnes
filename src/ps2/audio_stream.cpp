/**
 * @file audio_stream.cpp
 * @brief Music playback for the Sony PlayStation 2 backend: SPU2-format ADPCM
 *        streamed through audsrv, decoded on the EE. The default backend,
 *        selected by -DPS2_MUSIC=stream; see the PS2_MUSIC block in
 *        CMakeLists.txt for the alternatives (spu2, vorbis).
 *
 * This is the backend that imposes nothing on the asset. The shared
 * demo/audio/tracks/pc_audio.wav is used as-is at any length, transcoded at
 * build time exactly the way every other backend transcodes it, with no
 * per-platform trimming and no size knob that has to be got right.
 *
 * It exists because the two obvious alternatives each give up something:
 *
 *   - src/ps2/audio.cpp decodes Ogg Vorbis on the EE. Vorbis is a
 *     floating-point MDCT codec and the EE is a 294MHz scalar core with an 8KB
 *     data cache, so that decode is a large slice of the frame budget -- it is
 *     what held this backend at ~55fps.
 *   - src/ps2/audio_spu2.cpp hands the sound chip an ADPCM sample and costs
 *     the EE nothing at all, which is the ideal -- but a resident sample has to
 *     fit in SPU2 sound RAM, measured at ~756KB usable here. That is 120s at
 *     11kHz mono for a 4m29s track, so it only suits an asset trimmed to a
 *     short musical loop.
 *
 * ADPCM decoded on the EE splits the difference and lands much closer to the
 * free end than the costly one: ~8 integer operations per sample (one nibble,
 * a shift, a two-tap filter, a clamp) against Vorbis's hundreds of
 * floating-point ones plus the cache misses. At the build's default rate that
 * is a fraction of a percent of the EE, against roughly a quarter of it.
 *
 * Mono, not stereo. The quality-per-byte is better spent on sample rate than
 * on a second channel here, it halves both the decode and the embedded size,
 * and it keeps the decoder free of ADPCM channel-interleave handling. If
 * stereo is ever wanted, ps2sdk's other encoder (bin/ps2adpcm, which has
 * -s/-c/-l flags aimed at exactly this streaming case) is the place to start
 * rather than extending this.
 *
 * The streaming machinery -- a dedicated EE thread above main, paced by
 * audsrv_wait_audio() and yielding with DelayThread() -- is deliberately the
 * same shape as src/ps2/audio.cpp, because that shape is the one this project
 * has actually had working on hardware. Only the codec in fill() differs. See
 * that file's header and its DelayThread() comment before changing any of it;
 * both have cost real debugging.
 *
 * audsrv is NOT a BIOS-resident module (unlike SIO2MAN/PADMAN in src/ps2/
 * input.cpp) -- it's a ps2sdk-supplied IOP driver, so the prebuilt audsrv.irx
 * ps2sdk ships is #embedded into the ELF the same way the music is, and loaded
 * at runtime with SifExecModuleBuffer instead of SifLoadModule's rom0: path.
 * LIBSD (the SPU2 driver audsrv itself depends on) IS BIOS-resident.
 *
 * No SFX path: like GameCube/PSP, this project currently registers no SFX
 * table for any non-NES target (see demo/src/tracks.cpp). Note that
 * src/ps2/audio_spu2.cpp is the better starting point if that changes -- SFX
 * are short, so they fit SPU2 RAM comfortably and cost nothing to play.
 */
#include "internal.hpp"
#include <pnes/audio.hpp>

#include <sifrpc.h>
#include <loadfile.h>
#include <kernel.h>
#include <audsrv.h>
#include <delaythread.h>
#include <sbv_patches.h>
#include <cstring>

#include <pnes/logger.hpp>

#ifndef PS2_STREAM_RATE
#error "PS2_STREAM_RATE must be defined by the build (the rate pc_audio.adp was encoded at)"
#endif

// The SPU2-format ADPCM music, baked into the ELF at build time: CMake runs
// ffmpeg (downmix to mono at PS2_STREAM_RATE) then ps2sdk's bin/adpenc, and
// puts that dir on the embed path so #embed finds it.
//
// Encoded WITHOUT adpenc's -L: the loop flags in the data are instructions to
// the SPU2's own voice hardware, and nothing here plays through a voice -- the
// loop is done in software by fill() below, which can also honour a
// per-track loop_start that the flags have no way to express.
static const unsigned char adpcm_data[] = {
#embed <pc_audio.adp>   // resolved via --embed-dir (set in the ps2 CMake branch)
};
static constexpr int adpcm_len = static_cast<int>(sizeof(adpcm_data));

// The prebuilt audsrv IOP module ps2sdk ships (see the file header).
//
// alignas(16): this buffer is handed straight to SifExecModuleBuffer, which
// DMAs it to the IOP. A plain `unsigned char[]` has no alignment of its own
// (1 byte), so whatever address it landed on was pure luck of the compiler's
// .rodata layout; a non-LTO per-TU debug build and a whole-program LTO
// release build pack .rodata differently, so "it happened to land aligned"
// was never something either build actually guaranteed -- an unlucky release
// layout here left the module looking like it loaded while never actually
// starting on the IOP, hanging audsrv_init() forever.
alignas(16) static const unsigned char audsrv_irx[] = {
#embed <audsrv.irx>
};
static constexpr int audsrv_irx_len = static_cast<int>(sizeof(audsrv_irx));

static constexpr int SAMPLE_RATE = PS2_STREAM_RATE;

// Stereo, even though the ADPCM stream itself is mono: fill() writes each
// decoded sample to both channels. The mono stream is a storage/decode
// decision, but the audsrv output format is not a free choice -- audsrv can
// reject a format outright (AUDSRV_ERR_FORMAT_NOT_SUPPORTED) and every format
// known to work on this console is stereo: 44100/16/2 in src/ps2/audio.cpp,
// and 22050/16/2 in ps2sdk's own playwav and playwav2 samples. A 32000Hz mono
// format was tried here and produced total silence.
static constexpr int CHANNELS    = 2;

// One audsrv chunk per decode/play cycle, in mono frames (each becomes a
// stereo frame in chunk[]). ~46ms at 44.1kHz. Matches
// src/ps2/audio.cpp's CHUNK_FRAMES; nothing here is derived from the frame
// rate, which is the point.
static constexpr int CHUNK_FRAMES = 2048;
static constexpr int CHUNK_BYTES  = CHUNK_FRAMES * CHANNELS * static_cast<int>(sizeof(s16));

// adpenc's layout: a 16-byte "APCM" header, then 16-byte SPU2 ADPCM blocks.
// Each block holds 28 samples as 4-bit nibbles in bytes 2..15; byte 0 packs
// the shift in its low nibble and the filter index in its high nibble, and
// byte 1 carries SPU2 voice loop flags this backend has no use for.
static constexpr int ADP_HEADER_BYTES  = 16;
static constexpr int ADP_BLOCK_BYTES   = 16;
static constexpr int ADP_BLOCK_SAMPLES = 28;

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
// SIF the same way audsrv_irx is.
alignas(16) s16 chunk[CHUNK_FRAMES * CHANNELS];

// Standard PS/SPU 4-bit ADPCM predictor, as a pair of two-tap filters scaled
// by 64. Verified rather than assumed: decoding this project's own asset with
// these coefficients and comparing against the PCM ffmpeg produced before
// encoding gives 34.4dB SNR, which a wrong filter or nibble order could not.
const int F0[5] = {0,  60, 115,  98, 122};
const int F1[5] = {0,   0, -52, -55, -60};

const int totalBlocks = (adpcm_len - ADP_HEADER_BYTES) / ADP_BLOCK_BYTES;

s16 decoded[ADP_BLOCK_SAMPLES];   // the block fill() is currently handing out
int curBlock   = -1;              // which block decoded[] holds
int posInBlock = ADP_BLOCK_SAMPLES;
int h1 = 0, h2 = 0;               // predictor history, carried across blocks
int loopBlock  = 0;               // block the stream restarts at on EOF

bool          running = false;
volatile bool playing = false;
volatile bool paused  = false;

// TrackPlay() runs on the main thread while audio_thread() is concurrently
// mid-decode, and the decoder state above (curBlock/posInBlock/h1/h2) is not
// safe to reposition from underneath it. So TrackPlay() never touches it: it
// posts the requested index here and audio_thread() -- the only thread that
// may touch decoder state -- applies it at the top of its own loop. -1 means
// no request pending.
//
// Plain `volatile int`, not std::atomic: the EE has no hardware word-CAS
// (that's what std::atomic<int>::exchange needs under the hood), so linking it
// drags in libatomic, which this toolchain doesn't have wired up. A
// naturally-aligned int is already indivisible on this single-core CPU --
// `volatile` is only here to stop the compiler caching either side's
// read/write in a register -- and with exactly one writer (TrackPlay, on the
// main thread) and one reader (audio_thread, which is also the only one that
// ever clears it back to -1), there's no partial update to race on.
volatile int pending_track = -1;

// Decodes one 16-byte ADPCM block into decoded[]. `>> 6`, not `/ 64`: the
// predictor term is routinely negative and C++ integer division truncates
// toward zero while an arithmetic shift floors, which is what the format (and
// the reference this was checked against) specifies.
void decode_block(const int blockIndex) {
    const unsigned char *blk = adpcm_data + ADP_HEADER_BYTES + blockIndex * ADP_BLOCK_BYTES;

    const int shift = blk[0] & 0x0F;
    int       filt  = (blk[0] >> 4) & 0x0F;
    if (filt > 4) filt = 0;   // the format defines 0..4; be inert on anything else

    for (int n = 0; n < ADP_BLOCK_SAMPLES; n++) {
        const int byte = blk[2 + (n >> 1)];
        const int nib  = ((n & 1) == 0) ? (byte & 0x0F) : (byte >> 4);

        // The nibble is the top 4 bits of a signed 16-bit value, then scaled
        // down by the block's shift.
        int s = static_cast<s16>(static_cast<u16>(nib << 12));
        s >>= shift;

        int pred = s + ((F0[filt] * h1 + F1[filt] * h2 + 32) >> 6);
        if (pred >  32767) pred =  32767;
        if (pred < -32768) pred = -32768;

        decoded[n] = static_cast<s16>(pred);
        h2 = h1;
        h1 = pred;
    }
}

// Rewinds the stream to loopBlock. The predictor history is cleared with it:
// it is a running state the encoder itself started from silence, so carrying
// the pre-wrap history across the seam would decode the first block against
// the wrong reference and click.
void seek_to_loop() {
    curBlock   = loopBlock - 1;   // fill() advances before decoding
    posInBlock = ADP_BLOCK_SAMPLES;
    h1 = h2 = 0;
}

// Fills chunk[] with CHUNK_FRAMES mono frames, wrapping to loopBlock on EOF.
void fill() {
    int got = 0;
    while (got < CHUNK_FRAMES) {
        if (posInBlock >= ADP_BLOCK_SAMPLES) {
            int next = curBlock + 1;
            if (next >= totalBlocks) {
                next = loopBlock;
                h1 = h2 = 0;   // see seek_to_loop
            }
            curBlock = next;
            decode_block(curBlock);
            posInBlock = 0;
        }

        int take = CHUNK_FRAMES - got;
        if (const int avail = ADP_BLOCK_SAMPLES - posInBlock; take > avail) take = avail;

        // Mono source, stereo sink: duplicate rather than memcpy. See CHANNELS
        // for why the output format is not mono.
        s16       *dst = chunk + got * CHANNELS;
        const s16 *src = decoded + posInBlock;
        for (int i = 0; i < take; i++) {
            dst[i * 2]     = src[i];
            dst[i * 2 + 1] = src[i];
        }

        got        += take;
        posInBlock += take;
    }
}

void audio_thread(void *) {
    while (running) {
        // Apply any TrackPlay() request posted from the main thread -- see
        // ::pending_track for why this, and not TrackPlay itself, repositions
        // the decoder.
        const int req = pending_track;
        if (req >= 0) {
            pending_track = -1;
        }
        if (req >= 0) {
            loopBlock = tracks[req].loop_start > 0.0f
                ? static_cast<int>(tracks[req].loop_start * static_cast<float>(SAMPLE_RATE))
                  / ADP_BLOCK_SAMPLES
                : 0;
            if (loopBlock >= totalBlocks) loopBlock = 0;
            seek_to_loop();
            playing = true;
            paused  = false;
        }

        if (!playing || paused) {
            memset(chunk, 0, sizeof(chunk));
        } else {
            fill();
        }

        audsrv_wait_audio(CHUNK_BYTES);
        audsrv_play_audio(reinterpret_cast<const char *>(chunk), CHUNK_BYTES);

        // Load-bearing; see the identical call in src/ps2/audio.cpp for the
        // measurement behind it. audsrv_wait_audio() above does NOT reliably
        // yield the EE to a lower-priority thread, so removing this starves
        // the render thread and produces silence. DelayThread() is a genuine
        // kernel-blocking sleep, so it forces a deterministic yield window
        // back to main every chunk. One chunk is ~46ms of playback; 2ms is a
        // small fraction of that, nowhere near an audsrv underrun.
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

    // Checked, unlike the other backends: audsrv can reject a format
    // (AUDSRV_ERR_FORMAT_NOT_SUPPORTED) and does so silently as far as the
    // caller is concerned, which is how an unsupported 32000Hz mono format
    // here turned into "no audio" with nothing to show for it. `< 0` rather
    // than `!= 0` -- audsrv documents errors as negative and really does
    // return meaningful positive values elsewhere in this API.
    if (const int err = audsrv_set_format(&format); err < 0) {
        log("audio::Init: audsrv_set_format(%dHz, 16-bit, %dch) rejected (%d), bailing",
            SAMPLE_RATE, CHANNELS, err);
        return;
    }
    audsrv_set_volume(MAX_VOLUME);

    if (totalBlocks < 1) {
        log("audio::Init: embedded ADPCM too small (%d bytes), no music", adpcm_len);
        return;
    }
    seek_to_loop();

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

    // Strictly HIGHER priority than main (smaller number = more urgent). A
    // lower-priority decode thread does not work on this backend: it can only
    // run in whatever slack the render loop leaves, and WaitSema on vblank
    // returns immediately once a frame has overrun its budget, so the window
    // vanishes exactly when the frame is heaviest. Every revision of this
    // backend in which audio actually played had this thread above main.
    //
    // Queried rather than hardcoded: a flat constant can't know where main
    // sits, and the relationship to main is the entire point.
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

    log("audio::Init: ADPCM stream ready, %d bytes, %d blocks, %dHz mono",
        adpcm_len, totalBlocks, SAMPLE_RATE);
}

void audio::Update() {
    // Streaming is driven by the dedicated audio thread; nothing to do per
    // frame. Deliberately NOT a decode entry point -- see src/ps2/audio.cpp's
    // header for what putting a decode on this call cost.
}

void audio::TrackPlay(const u8 index) {
    // Does NOT reposition the decoder directly -- only audio_thread() may; see
    // ::pending_track.
    if (index >= nTracks) return;
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
