/**
 * @file audio_spu2.cpp
 * @brief Music playback for the Sony PlayStation 2 backend via SPU2 hardware
 *        ADPCM (audsrv). Selected by -DPS2_MUSIC=adpcm; the alternative is
 *        src/ps2/audio.cpp. Exactly one of the two is compiled -- see the
 *        PS2_MUSIC block in CMakeLists.txt.
 *
 * This backend costs the EE *nothing* per frame. The track is encoded to SPU2
 * ADPCM at build time, uploaded once into the SPU2's own 2MB of sound RAM, and
 * played on one of its 24 hardware voices with the loop flag set. From then on
 * the sound chip decodes, resamples and loops it in hardware, driven by the
 * IOP -- there is no decode thread, no per-frame callback, no priority
 * tradeoff against rendering, and audio::Update() genuinely does nothing.
 *
 * That is how PS2 titles actually did music, and it is the reason to prefer
 * this over src/ps2/audio.cpp, which decodes Ogg Vorbis on the EE. Vorbis is a
 * floating-point MDCT codec and the EE is a 294MHz scalar core with an 8KB
 * data cache, so that decode is a large, irreducible slice of the frame budget
 * -- and four revisions of that file only ever moved the bill around (burst it
 * onto the render thread, or preempt the render thread to pay it). Here the
 * bill does not exist.
 *
 * THE TRADEOFF, which is real: a resident sample has to fit in SPU2 RAM.
 * SPU2 ADPCM is 16 bytes per 28 samples (0.571 bytes/sample), so 2MB holds
 * roughly 3.67M samples -- about 166s at 22.05kHz, or 333s at 11.025kHz. The
 * current pc_audio.wav is 4m29s and demo/src/tracks.cpp gives it
 * loop_start = 0 (the whole track loops), so at this length the ceiling is
 * ~13.6kHz mono; the build defaults to 11.025kHz, which fits in 81% of SPU2
 * RAM. Better fidelity needs a shorter loop, not a different codec -- trim
 * the asset to its musical loop and the rate can go straight back up
 * (a 60s loop at 22.05kHz is ~756KB).
 *
 * Mono, not stereo: an SPU2 voice is mono. Stereo would mean encoding two
 * samples, burning two voices and two halves of the RAM budget, and relying on
 * two separate RPC-issued voice starts staying sample-aligned. Not worth it for
 * background music at this rate.
 *
 * audsrv is NOT a BIOS-resident module (unlike SIO2MAN/PADMAN in src/ps2/
 * input.cpp) -- it's a ps2sdk-supplied IOP driver, so the prebuilt audsrv.irx
 * ps2sdk ships is #embedded into the ELF the same way the sample is, and loaded
 * at runtime with SifExecModuleBuffer instead of SifLoadModule's rom0: path.
 * LIBSD (the SPU2 driver audsrv itself depends on) IS BIOS-resident.
 *
 * No SFX path: like GameCube/PSP, this project currently registers no SFX
 * table for any non-NES target (see demo/src/tracks.cpp). Note that this
 * backend is the one that would make SFX nearly free if that changes -- 23
 * more hardware voices are sitting unused, and audsrv_ch_play_adpcm() on a
 * second loaded sample is the whole implementation.
 */
#include "internal.hpp"
#include <platform-nes/audio.hpp>

#include <sifrpc.h>
#include <loadfile.h>
#include <kernel.h>
#include <audsrv.h>
#include <sbv_patches.h>

#include <platform-nes/logger.hpp>

// The SPU2-ADPCM-encoded music, baked into the ELF at build time: CMake runs
// ffmpeg (downmix to mono at PS2_ADPCM_RATE) then ps2sdk's own bin/adpenc with
// -L, and puts that dir on the embed path so #embed finds it.
//
// adpenc's 16-byte header carries the SPU2 pitch register value, which it
// derives from the source WAV's sample rate (pitch == rate * 4096 / 48000,
// since 0x1000 is the SPU2's 48kHz unity rate), so audsrv_adpcm_t::pitch is
// never assigned below -- the encoder already got it right and guessing would
// only break it.
//
// ::loop IS assigned, because the encoder getting it right is not sufficient.
// -L marks the data correctly -- it sets the header's loop byte and flags the
// first ADPCM block 0x06 (loop-start|repeat); the final block carries 0x07
// (loop-end|repeat|loop-start) either way. But audsrv evidently does not
// propagate that header flag into the descriptor it fills, so leaving ::loop
// at its zero-initialised value had audsrv set the voice up as a one-shot:
// the track played once and went silent, confirmed on hardware. Both halves
// are needed -- -L for the block flags the SPU2 reads, ::loop for the voice
// setup audsrv performs.
//
// alignas(16): handed to audsrv_load_adpcm(), which DMAs it to the IOP over
// SIF the same way audsrv_irx below is.
//
// Deliberately NOT const: Init() patches one byte before upload, to clear a
// loop flag the encoder sets that makes the sample loop exactly once. See
// ::fix_loop_terminator().
alignas(16) static unsigned char adpcm_data[] = {
#embed <pc_audio.adp>   // resolved via --embed-dir (set in the ps2 CMake branch)
};
static constexpr int adpcm_len = static_cast<int>(sizeof(adpcm_data));

// adpenc's layout: a 16-byte "APCM" header, then 16-byte SPU2 ADPCM blocks of
// 28 samples each. Byte 0 of a block is its shift/filter nibbles; byte 1 is
// the SPU2 loop flags -- 0x01 loop-end, 0x02 repeat, 0x04 loop-start.
static constexpr int ADP_HEADER_BYTES = 16;
static constexpr int ADP_BLOCK_BYTES  = 16;

// The prebuilt audsrv IOP module ps2sdk ships (see the file header).
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

// audio.hpp declares these for the SDL3 mixer; this backend hands the sound
// chip a sample and never touches PCM, but define them so the externs resolve.
float *audio::pcm_buffer = nullptr;
u32 audio::pcm_buffer_size = 0;

// Every non-NES backend leaves ::sfxs/::nSfx undefined unless the app calls
// ::SFX(...); this weak zero-entry default keeps the link working until it
// does (mirrors the SDL3/Switch/Wii U/PSP backends), even though this
// backend doesn't act on it (see the file header).
__attribute__((weak)) extern const audio::sfx_t sfxs[1] = {};
__attribute__((weak)) extern const u8 nSfx = 0;

namespace {

// Clears the loop-start bit from the sample's FINAL block, which is what makes
// the difference between looping forever and looping exactly once.
//
// adpenc -L emits 0x06 (loop-start|repeat) on block 0 and 0x07
// (loop-end|repeat|loop-start) on the last block; verified by scanning the
// encoder's output, those are the only two blocks with any flags set at all.
// On the SPU2 the loop-start bit does not describe the sample, it is an
// instruction: it sets the voice's repeat address to the block carrying it.
// So the first pass ends, jumps correctly back to block 0 -- but that final
// block has just repointed the repeat address at ITSELF, and it holds nothing
// but zeroes. Every subsequent loop-end jumps into that one silent block and
// stays there.
//
// Measured on hardware before this: the track played through, looped once,
// then went silent. Clearing 0x04 here leaves 0x03 (loop-end|repeat), so the
// repeat address stays block 0 for good.
//
// Done here rather than in the build rule so it cannot be lost to a different
// adpenc version or a hand-encoded asset, and because the flag byte is the
// kind of thing worth having the reason for next to the code that depends on
// it.
void fix_loop_terminator() {
    if (adpcm_len <= ADP_HEADER_BYTES + ADP_BLOCK_BYTES) return;

    const int blocks = (adpcm_len - ADP_HEADER_BYTES) / ADP_BLOCK_BYTES;
    if (blocks < 1) return;
    const int lastFlag = ADP_HEADER_BYTES + (blocks - 1) * ADP_BLOCK_BYTES + 1;

    adpcm_data[lastFlag] &= static_cast<unsigned char>(~0x04);

    // The EE's data cache is writeback, and audsrv_load_adpcm DMAs this buffer
    // out of main memory -- without flushing, the byte just written could still
    // be sitting in a dirty cache line and the DMA would ship the unpatched
    // original.
    SyncDCache(adpcm_data, adpcm_data + adpcm_len);
}

audsrv_adpcm_t sample{};       // filled by audsrv_load_adpcm from the header
bool           loaded  = false;
int            channel = -1;   // SPU2 voice the track is playing on, -1 if none
bool           paused  = false;

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

    // No audsrv_set_format() here, unlike src/ps2/audio.cpp: that configures
    // audsrv's PCM ring buffer, which this backend never feeds.
    // `< 0`, not `!= 0`: audsrv documents these as "negative value on error"
    // and the rest of the API really does return meaningful positive values on
    // success (audsrv_play_audio returns bytes sent, audsrv_ch_play_adpcm
    // returns the channel id), so a nonzero-but-positive result here is a
    // success, not a failure. Testing != 0 is how an earlier revision of this
    // file bailed out of a load that had actually worked -- silently, since
    // logging is compiled out of release builds. ps2sdk's own playadpcm sample
    // does not check either of these returns at all.
    if (const int err = audsrv_adpcm_init(); err < 0) {
        log("audio::Init: audsrv_adpcm_init failed (%d), bailing", err);
        return;
    }
    audsrv_set_volume(MAX_VOLUME);

    // Uploads the whole sample into SPU2 sound RAM. This is the one expensive
    // call in this file and it happens exactly once.
    //
    // Worth watching if this ever fails: audsrv's own PCM ring also lives in
    // SPU2 RAM, so the space actually available to samples is less than the
    // chip's 2MB by however much audsrv reserves for itself -- which is not
    // documented. The build default leaves ~400KB of headroom against the full
    // 2MB; if this returns an error, that headroom wasn't enough and the fix
    // is a lower PS2_ADPCM_RATE (or a shorter loop), not a change here.
    fix_loop_terminator();

    const int loadResult = audsrv_load_adpcm(&sample, adpcm_data, adpcm_len);
    if (loadResult < 0) {   // see the note on audsrv_adpcm_init above
        log("audio::Init: audsrv_load_adpcm failed (%d) for %d bytes, no music", loadResult, adpcm_len);
        return;
    }

    const int rawLoop = sample.loop;   // what audsrv left it as; logged below

    // Set between the load and the play, which is the window that matters:
    // load_adpcm fills the descriptor, ch_play_adpcm acts on it. See the
    // ::loop note above adpcm_data for why this is not left to the encoder.
    sample.loop = 1;

    loaded = true;
    // Logs the result, not just success: it is the one call here whose
    // non-negative return value is undocumented, and knowing what it actually
    // hands back (0, a byte count, an SPU2 address) is worth having on record.
    // Logs the raw descriptor as load_adpcm left it, before ::loop is forced
    // above, so the record shows what audsrv actually populates.
    log("audio::Init: SPU2 ADPCM loaded, %d bytes, ret=%d, pitch=%d loop=%d(raw %d) size=%d",
        adpcm_len, loadResult, sample.pitch, sample.loop, rawLoop, sample.size);
}

void audio::Update() {
    // Nothing, and not in the "nothing to do this frame" sense the other
    // backends mean: the SPU2 is decoding and looping this track in hardware,
    // so there is no per-frame work to do at all. This is the whole point of
    // this backend (see the file header).
}

void audio::TrackPlay(const u8 index) {
    if (!loaded || index >= nTracks) return;

    // One sample is loaded, and demo/src/tracks.cpp declares one track, so
    // `index` only has to be validated, not selected on. If a second track is
    // ever added, this is where a second audsrv_adpcm_t would be chosen.
    if (channel < 0) {
        channel = audsrv_ch_play_adpcm(-1, &sample);   // -1: any free voice
        if (channel < 0) {
            log("audio::TrackPlay: audsrv_ch_play_adpcm failed (%d)", channel);
            return;
        }
    }

    // Also the resume path: see TrackPause for why volume, not a stop/start.
    audsrv_adpcm_set_volume_and_pan(channel, MAX_VOLUME, 0);
    paused = false;
}

void audio::TrackStop() {
    if (channel < 0) return;

    // Muted rather than stopped, and the voice is deliberately kept: audsrv
    // exposes no way to stop or rewind a single ADPCM voice. The only stop it
    // has is audsrv_adpcm_init(), which also frees every loaded sample and so
    // would mean re-uploading the whole track to SPU2 RAM to start it again.
    //
    // Holding the voice is what stops TrackPlay() leaking a second one every
    // time it's called. The audible behaviour is identical for this project --
    // one track, loop_start = 0, so there is no position worth preserving and
    // nothing else competing for the other 23 voices.
    audsrv_adpcm_set_volume_and_pan(channel, 0, 0);
}

void audio::TrackPause(const u8 pause) {
    (void)pause;  // match the SDL/Switch/Wii U/PSP backends: toggle regardless of argument
    if (channel < 0) return;

    paused = !paused;
    // A mute, not a position hold -- the hardware voice keeps looping behind
    // it, so unpausing resumes wherever the loop has got to rather than where
    // it was paused. See TrackStop for why there is no alternative through
    // audsrv, and the file header for the SFX note if that ever matters.
    audsrv_adpcm_set_volume_and_pan(channel, paused ? 0 : MAX_VOLUME, 0);
}

// See the file header: no non-NES SFX table exists in this project yet. This
// is the backend where implementing them would be nearly free, though.
void audio::SfxPlay(u8 /*index*/, u8 /*channel*/) {}
void audio::SfxSamplePlay(u8 /*index*/) {}
