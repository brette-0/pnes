/**
 * @file audio.cpp
 * @brief Music playback for the Sony PlayStation 3 backend (PSL1GHT libaudio).
 *
 * Same shape as the PSP/PS2/Wii U backends (src/psp/audio.cpp,
 * src/ps2/audio.cpp, src/wiiu/audio.cpp): the desktop pc_audio.wav is
 * transcoded to a small Vorbis OGG at build time and #embedded into the
 * executable (see the ps3 branch of CMakeLists.txt), then decoded here with
 * stb_vorbis and streamed from a dedicated thread, keeping playback off the
 * 60Hz render loop entirely so Update() is a no-op.
 *
 * Two things are genuinely easier here than on the PS2, and both are worth
 * naming because they're the parts that branch looks complicated for:
 *
 *  - No IOP module to load. src/ps2/audio.cpp has to #embed ps2sdk's prebuilt
 *    audsrv.irx and call sbv_patch_enable_lmb() so SifExecModuleBuffer can
 *    load it out of EE RAM. libaudio is PPU-side; audioInit() is the whole
 *    equivalent, so neither the second --embed-dir nor the sbv patch exists
 *    in the ps3 CMake branch.
 *
 *  - No format conversion. libaudio's port buffer is 32-bit float, which is
 *    exactly what stb_vorbis_get_samples_float_interleaved() produces, so
 *    there is no i16 conversion step at all. The port is also fixed at 48kHz,
 *    so the ffmpeg step encodes the OGG to 48000 rather than the 44100 the
 *    PSP/PS2 branches use -- resampling in here would be the only other way
 *    to bridge that, and picking the rate at encode time costs nothing.
 *
 * Pacing comes from the notify event queue: libaudio posts an event each time
 * it finishes reading a block, so the thread blocks on sysEventQueueReceive()
 * and fills the block after the one being read. AUDIO_BLOCK_SAMPLES is 256
 * frames (~5.3ms at 48kHz), so the thread wakes often but does very little
 * each time.
 *
 * No SFX path: like the GameCube/PSP/PS2 backends, this project currently
 * registers no SFX table for any non-NES target (see demo/src/tracks.cpp), so
 * building a second decode/mix path for a currently-empty table isn't worth
 * it. Revisit alongside those if/when a non-NES SFX table exists.
 */
#include "internal.hpp"
#include <pnes/audio.hpp>

#include <audio/audio.h>
#include <lv2/thread.h>
#include <sys/thread.h>
#include <sys/event_queue.h>
#include <sys/systime.h>

#include <atomic>
#include <cstring>

#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include <stb_vorbis.c>

// The Vorbis-encoded music, baked into the ELF at build time. ffmpeg produces
// pc_audio.ogg (encoded straight to libaudio's fixed 48kHz, see SAMPLE_RATE)
// into the build tree and CMake puts that dir on the include path, so #embed
// finds it. The compiler here is Clang, not ps3dev's GCC 13.2 -- #embed is a
// GCC 15+ feature and 13.2 does not have it, which is the whole reason
// cmake/PS3.cmake puts Clang in front of this toolchain.
static const unsigned char ogg_data[] = {
#embed <pc_audio.ogg>   // resolved via --embed-dir (set in the ps3 CMake branch)
};
static constexpr int ogg_len = static_cast<int>(sizeof(ogg_data));

// libaudio's port rate is fixed at 48kHz; the OGG is encoded to exactly that
// at build time so no resampling is needed here.
static constexpr int SAMPLE_RATE = 48000;
static constexpr int CHANNELS    = 2;

// audio.hpp declares these for the SDL3 mixer; this backend streams straight
// into libaudio's port buffer instead, but define them so the externs resolve.
float *audio::pcm_buffer = nullptr;
u32 audio::pcm_buffer_size = 0;

// Every non-NES backend leaves ::sfxs/::nSfx undefined unless the app calls
// ::SFX(...); this weak zero-entry default keeps the link working until it
// does (mirrors the SDL3/Switch/Wii U/PSP backends), even though this backend
// doesn't act on it (see the file header).
__attribute__((weak)) extern const audio::sfx_t sfxs[1] = {};
__attribute__((weak)) extern const u8 nSfx = 0;

namespace {

stb_vorbis *vorbis     = nullptr;
u32         loop_frame = 0;      // stb_vorbis_seek() target on EOF
bool        playing    = false;
bool        paused     = false;

u32                 port_num  = 0;
audioPortConfig     port_cfg  = {};
sys_event_queue_t   snd_queue = 0;
sys_ipc_key_t       snd_key   = 0;
sys_ppu_thread_t    athread   = 0;
bool                opened    = false;

std::atomic<bool> running{false};

// TrackPlay() runs on the main thread while audio_thread() is concurrently
// mid-decode on `vorbis` -- stb_vorbis's decoder object is not safe to call
// into from two threads at once (a seek landing mid-decode corrupts its
// internal state, after which every decode call returns 0 samples forever:
// permanent silence, no error, no crash). This was first caught on PS2; see
// src/ps2/audio.cpp's own copy of that comment.
//
// So TrackPlay() never touches `vorbis` itself: it only posts the requested
// index here, and audio_thread() -- the only thread allowed to call any
// stb_vorbis_* function -- applies it at the top of its own loop. -1 means no
// request pending.
//
// std::atomic<int> here, unlike the `volatile int` the PS2 and PSP backends
// use. Those two comment that their CPU has no hardware word-CAS so
// std::atomic would pull in libatomic; the PPU does have lwarx/stwcx, so
// std::atomic<int> is genuinely lock-free on this target with nothing extra
// linked. And it is not merely tidier here: the PPU is 2-way SMT, so this
// really is two hardware threads touching the same word concurrently, rather
// than the single-core interleaving the EE and the Allegrex are limited to.
std::atomic<int> pending_track{-1};

// Fills `frames` stereo frames of float PCM at `dst`, looping back to
// loop_frame on EOF (mirrors src/psp/audio.cpp's fill()).
void fill(float *dst, const int frames) {
    int got = 0, stalls = 0;
    while (got < frames && stalls < 4) {
        const int n = stb_vorbis_get_samples_float_interleaved(
            vorbis, CHANNELS, dst + got * CHANNELS, (frames - got) * CHANNELS);
        if (n == 0) {
            stb_vorbis_seek(vorbis, loop_frame);
            stalls++;
            continue;
        }
        got += n;
        stalls = 0;
    }
    if (got < frames) {
        std::memset(dst + got * CHANNELS, 0,
                    static_cast<size_t>(frames - got) * CHANNELS * sizeof(float));
    }
}

// Which reading of audioPortConfig::readIndex this run is using. See
// ::resolve_read_index.
enum class ReadIndexKind { Pointer, OwnCounter };

// audioPortConfig::readIndex is the single genuinely ambiguous field in this
// whole backend, and getting it wrong is not a subtle failure: dereferencing a
// plain small integer as a pointer faults inside the audio thread moments
// after boot, which is indistinguishable from "the game didn't start".
//
// The two readings:
//
//  - PSL1GHT's own doxygen calls it "index of currently read block", i.e. the
//    value itself.
//  - The real Cell SDK struct this one mirrors field-for-field spells the same
//    slot `sys_addr_t readIndexAddr` -- an ADDRESS of a u64 holding the index.
//    Its neighbour here, `audioDataStart`, is likewise a u32 holding an
//    address ("start address of audio port buffer"), which is strong evidence
//    for the Cell reading being the right one for this struct.
//
// Rather than pick, this decides at runtime from the value libaudio actually
// handed back. A block index is 0..numBlocks-1, i.e. at most 31; a user-space
// address on this console is far above that. So a value below numBlocks
// cannot be an address, and a large value cannot be an index.
//
// The OwnCounter fallback is not a degraded mode: audio.h's own usage notes
// describe exactly that contract -- "The first block number is 0, the
// following is 1, and so on until you reach the number of allocated blocks,
// then the block number returns to 0" -- one block per notify event, which is
// what this thread already is.
ReadIndexKind resolve_read_index(const u64 **out) {
    const u64 raw = static_cast<u64>(port_cfg.readIndex);
    if (raw > port_cfg.numBlocks && raw >= 0x10000) {
        *out = reinterpret_cast<const u64 *>(raw);
        ps3_trace("audio: readIndex looks like an address (Cell reading)");
        return ReadIndexKind::Pointer;
    }
    *out = nullptr;
    ps3_trace("audio: readIndex is not an address -- counting blocks locally");
    return ReadIndexKind::OwnCounter;
}

void audio_thread(void *) {
    const u64 *read_index = nullptr;
    const ReadIndexKind kind = resolve_read_index(&read_index);

    float *const data_start = reinterpret_cast<float *>(static_cast<u64>(port_cfg.audioDataStart));
    const u32 block_floats = static_cast<u32>(port_cfg.channelCount) * AUDIO_BLOCK_SAMPLES;
    u64 own_block = 0;   // used only when `kind` is OwnCounter

    while (running.load(std::memory_order_relaxed)) {
        sys_event_t event;
        // 100ms timeout rather than an indefinite wait: a timed-out receive
        // just re-checks `running`, which is what lets audio::Init's teardown
        // path (and process exit) actually reach this thread instead of
        // leaving it blocked on a queue nothing will post to again.
        if (sysEventQueueReceive(snd_queue, &event, 100 * 1000) != 0) continue;

        // Fill the block AFTER the one libaudio is currently reading: writing
        // the current one would race the hardware mid-block.
        const u64 cur = kind == ReadIndexKind::Pointer ? *read_index : own_block++;
        const u64 next_block = (cur + 1) % port_cfg.numBlocks;
        float *dst = data_start + next_block * block_floats;

        // Apply any TrackPlay() request posted from the main thread -- see
        // ::pending_track for why this, not TrackPlay itself, calls into the
        // decoder.
        const int req = pending_track.exchange(-1, std::memory_order_relaxed);
        if (req >= 0 && vorbis) {
            stb_vorbis_seek_start(vorbis);
            loop_frame = tracks[req].loop_start > 0.0f
                ? static_cast<u32>(tracks[req].loop_start * static_cast<float>(SAMPLE_RATE))
                : 0;
            playing = true;
            paused  = false;
        }

        if (!vorbis || !playing || paused) {
            std::memset(dst, 0, static_cast<size_t>(block_floats) * sizeof(float));
        } else {
            fill(dst, AUDIO_BLOCK_SAMPLES);
        }
    }

    // Required, not tidiness: lv2 has no notion of a PPU thread entry point
    // "returning". sysThreadCreate's entry is branched to, not called with a
    // return address worth anything, so falling off the end of this function
    // executes whatever follows it. sysThreadExit (lv2/thread.h) is how a
    // thread actually ends. Only reachable once `running` goes false, which
    // today is the process-exit path.
    sysThreadExit(0);
}

} // namespace

void audio::Init(u8) {
    ps3_trace("audio::Init: entered");

    int err = 0;
    vorbis = stb_vorbis_open_memory(ogg_data, ogg_len, &err, nullptr);
    if (!vorbis) {
        ps3_trace("audio::Init: stb_vorbis_open_memory FAILED");
        return;
    }

    if (nTracks > 0) {
        loop_frame = tracks[0].loop_start > 0.0f
            ? static_cast<u32>(tracks[0].loop_start * static_cast<float>(SAMPLE_RATE))
            : 0;
    }

    if (audioInit() != 0) {
        ps3_trace("audio::Init: audioInit FAILED");
        return;
    }

    audioPortParam params;
    std::memset(&params, 0, sizeof(params));
    params.numChannels = AUDIO_PORT_2CH;
    // 8 blocks (~43ms of 256-frame blocks at 48kHz) is libaudio's smallest
    // port and plenty of slack for a decode this cheap -- the thread only has
    // to stay one block ahead of the read cursor.
    params.numBlocks   = AUDIO_BLOCK_8;
    // AUDIO_PORT_INITLEVEL, not 0: `level` is only read when that attribute is
    // set, and the memset above leaves it at 0.0f -- so the previous `attrib =
    // 0` meant the port opened at whatever level libaudio defaults to, with
    // the 1.0f this backend actually wants never communicated at all. Asking
    // for full scale explicitly is the whole reason the field exists.
    params.attrib      = AUDIO_PORT_INITLEVEL;
    params.level       = 1.0f;

    if (audioPortOpen(&params, &port_num) != 0) {
        ps3_trace("audio::Init: audioPortOpen FAILED");
        audioQuit();
        return;
    }
    if (audioGetPortConfig(port_num, &port_cfg) != 0 || port_cfg.audioDataStart == 0) {
        // Without a valid audioDataStart there is nowhere to write samples,
        // and audio_thread would compute its destination from a null base.
        ps3_trace("audio::Init: audioGetPortConfig FAILED");
        audioPortClose(port_num);
        audioQuit();
        return;
    }
    opened = true;

    audioCreateNotifyEventQueue(&snd_queue, &snd_key);
    audioSetNotifyEventQueue(snd_key);
    // Anything libaudio queued between the port opening and the handler being
    // attached is stale by now; starting from a drained queue means the first
    // receive in audio_thread corresponds to a real block boundary.
    sysEventQueueDrain(snd_queue);

    audioPortStart(port_num);

    running.store(true, std::memory_order_relaxed);
    // 1000 is PSL1GHT's conventional "slightly above normal" thread priority
    // (lower is higher-priority on lv2); 0x10000 matches the stack the PSP
    // backend gives its own audio thread.
    if (sysThreadCreate(&athread, audio_thread, nullptr, 1000, 0x10000, THREAD_JOINABLE,
                        const_cast<char *>("audio_thread")) != 0) {
        running.store(false, std::memory_order_relaxed);
        ps3_trace("audio::Init: sysThreadCreate FAILED -- no music this run");
        return;
    }
    ps3_trace("audio::Init: complete");
}

void audio::Update() {
    // Streaming is driven by the dedicated audio thread; nothing to do per frame.
}

void audio::TrackPlay(const u8 index) {
    // Does NOT touch `vorbis` or `loop_frame`/`playing`/`paused` directly --
    // only audio_thread() may call into the decoder; see ::pending_track.
    if (!vorbis || index >= nTracks) return;
    pending_track.store(index, std::memory_order_relaxed);
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
