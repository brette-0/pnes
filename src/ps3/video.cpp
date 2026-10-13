/**
 * @file video.cpp
 * @brief Sony PlayStation 3 presentation backend for the shared software PPU.
 *
 * The PPU emulation itself -- ::emu::GenerateFrame and every nametable/
 * attribute/palette/OAM write function plus the scroll and sprite-zero
 * machinery -- lives in src/emu/ppu.cpp and is shared verbatim with the SDL3,
 * libogc/GX, 3DS, Switch, Wii U, PSP and PS2 backends. This file is only the
 * PS3-specific half: it asks the core to render a 256x240 frame into a staging
 * buffer, integer-scale-blits that into an RSX-mapped display buffer, and
 * flips it.
 *
 * ## Why none of the RSX 3D pipeline is used
 *
 * The obvious PS3 present is what the PS2 backend does with the GS: upload the
 * frame as a texture and draw one textured quad, letting the GPU scale it. On
 * the RSX that is not the small step it is on the GS -- it needs a vertex and
 * a fragment program compiled by cgcomp, a gcmSurface render target, a vertex
 * array, and the texture upload path, which is a lot of first-pass surface
 * area for a 256x240 image that the PPU can place itself. So this does what
 * src/psp/video.cpp does (raw buffer writes, no GE) and what the Switch
 * backend does (no deko3d for a first pass): writes pixels into the display
 * buffer directly and calls gcmSetFlip(). Nothing here forecloses the textured
 * quad later; it just isn't needed to get a correct picture.
 *
 * ## Scaling
 *
 * Unlike the PSP, the output size is not a fixed panel -- it is whatever the
 * console's current video mode is (480p/576p/720p/1080p, user setting, queried
 * at init). And unlike the PS2 there is no free hardware scale here, since the
 * PPU is doing the work. So the scale factor is computed once at init as the
 * largest integer that fits in BOTH axes:
 *
 *     scale = min(width / 256, height / 240)      (at least 1)
 *
 * which gives 2x at 480p (512x480), 3x at 720p (768x720 -- 240 divides 720
 * exactly), and 4x at 1080p (1024x960). The result is centred, and every
 * destination pixel is exactly one source pixel: the same no-warping guarantee
 * src/psp/video.cpp spells out for its 1x case, which is why an integer factor
 * is picked rather than stretching to fill. The remaining margin is
 * pillar/letterboxing, matching the NES's own 4:3 display aspect reasonably
 * closely at every mode.
 *
 * ## Pixel format
 *
 * The core fills ARGB8888 (0xAARRGGBB as a u32). The video mode is configured
 * VIDEO_BUFFER_FORMAT_XRGB, i.e. bytes X,R,G,B in memory -- and the PPU is
 * big-endian, so storing that same 0xAARRGGBB u32 lays the bytes down as
 * AA,RR,GG,BB already. No channel swap is needed at all, unlike the PSP and
 * Switch backends (both little-endian, both swapping R and B). The alpha byte
 * lands in the ignored X slot.
 */
#include "internal.hpp"

#include <platform-nes/video.hpp>
#include <platform-nes/interrupts.hpp>
#include "../emu/emu.hpp"

#include <cstring>
#include <malloc.h>

// Must come between <ppu-types.h> (via internal.hpp, above) and <rsx/...>:
// Clang cannot parse PSL1GHT's RSX structs as-declared. See the header for the
// full reasoning and for what would invalidate it.
#include "prxptr_clang_shim.hpp"

#include <rsx/rsx.h>
#include <sysutil/video.h>
#include <sysutil/sysutil.h>
#include <sys/systime.h>

// ---------------------------------------------------------------------------
// PS3-specific state. Everything the core needs (VideoRAM, paletteRAM, scroll,
// PPUCTRL/PPUMASK, the OAM snapshot, patternTable, ...) is owned by src/emu;
// only the RSX context and the display buffers live here.
// ---------------------------------------------------------------------------
static constexpr int VP_W = video::viewport_px();   // 256 -- NES-native
static constexpr int VP_H = video::viewport_py();   // 240 -- NES-native

static_assert(VP_W == 256 && VP_H == 240,
              "PS3 backend assumes the NES-native viewport -- see video.hpp's TARGET_PS3 branch");

// rsxInit's command buffer and its IO (main-memory) mapping. Nothing here is
// command-buffer heavy -- the only commands ever emitted are the flip ones --
// so the command buffer is the size PSL1GHT's samples use and no more.
static constexpr u32 CB_SIZE   = 0x100000;   // 1MB RSX command buffer
// 4MB, not 1MB. libgcm carves the command buffer out of the START of the IO
// area, so ioSize == cmdSize leaves the IO heap with nothing at all -- a state
// no PSL1GHT sample is in, and one with no reason to be trusted. rsx.h only
// requires a 1MB-aligned size that is a multiple of 1MB, so going wider costs
// 3MB of main memory and removes the question. Nothing here rsxMalloc()s from
// the IO heap, so this is headroom rather than a requirement.
static constexpr u32 HOST_SIZE = 0x400000;   // 4MB host (main memory) IO area

static constexpr int NUM_BUFFERS = 2;        // double buffered

static gcmContextData *context = nullptr;
static u32 *display_buf[NUM_BUFFERS] = {};
static u32  display_off[NUM_BUFFERS] = {};
static int  back_buf = 0;                    // index being drawn into this frame

static int screen_w = 0;                     // current video mode, in pixels
static int screen_h = 0;
static int stride_px = 0;                    // display buffer pitch, in u32s
static int scale = 1;                        // integer upscale factor
static int origin_x = 0;                     // top-left of the scaled image
static int origin_y = 0;

// False until irq::init() has got all the way to the end. Every bail-out in
// that function used to leave the backend half-built -- context null, or
// display_buf[] null, or stride_px 0 -- and WaitForPresent() would then blit
// through a null base pointer on the very first frame. That turns a legible
// "init step N failed" into an access violation several calls away from its
// cause, which is the wrong trade when the failure is already hard to observe.
// Presenting nothing is the honest behaviour; the trace says why.
static bool init_ok = false;

// The core renders here at native 256x240, then the blit below scales out of
// it. A staging buffer is needed (unlike the PSP, which renders straight into
// VRAM) precisely because there IS a scale step: GenerateFrame writes one
// destination pixel per source pixel, so it cannot write the scaled image.
static u32 staging[VP_W * VP_H];

// ---------------------------------------------------------------------------
// XMB exit/menu handling. The firmware invokes this callback when the user
// quits from the XMB; it just flips the shared ::quit flag -- the same one
// video::WaitForPresent() and the demo's own RESET loop already poll -- so the
// process unwinds through the ordinary post()/return path instead of being
// killed mid-frame. Same shape as the PSP backend's exit callback.
// ---------------------------------------------------------------------------
static void sysutil_callback(u64 status, u64 /*param*/, void * /*usrdata*/) {
    if (status == SYSUTIL_EXIT_GAME) quit = 1;
}

void ps3_setup_exit_callback() {
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_callback, nullptr);
}

void ps3_poll_sysutil() {
    // Callbacks registered above only ever fire from inside this call, so it
    // has to happen on a regular cadence -- once per presented frame.
    sysUtilCheckCallback();
}

// ---------------------------------------------------------------------------
// Flip, the standard PSL1GHT sequence: queue the flip into the command buffer,
// flush it to the RSX, and make the RSX itself wait for the flip to retire
// before executing anything after it. The CPU-side wait is separate (below).
// ---------------------------------------------------------------------------
static void queue_flip(const int buffer) {
    // bufferId is u8 in the current PSL1GHT (older releases took u32).
    gcmSetFlip(context, static_cast<u8>(buffer));
    rsxFlushBuffer(context);
    gcmSetWaitFlip(context);
}

// Blocks until the queued flip has actually happened. This is what paces the
// frame to the console's real 50/60Hz refresh -- the PS3 equivalent of the
// PSP's sceDisplayWaitVblankStart().
//
// Bounded, not an unconditional spin. The polarity here is not actually
// settled: PSL1GHT's own samples all poll `while (gcmGetFlipStatus() != 0)`,
// i.e. 0 means "the flip retired", which matches the real Cell GCM constants
// (DISPLAY_FLIP_STATUS_DONE == 0, _WAITING == 1) -- but gcm_sys.h's doxygen
// says the opposite in as many words ("zero if no flip occured, nonzero
// otherwise"), and PSL1GHT defines no constants to arbitrate. If the doc is
// the right one, an unbounded loop here deadlocks on the FIRST wait, inside
// irq::init(), before a single pixel is presented -- and it deadlocks only
// when the flip happens to land before the first status read, which is a
// race, which is how one build can hang on one boot and fall through on the
// next.
//
// So this gives up after ~250ms (well over one frame at any mode this console
// outputs) and says so. A wrong assumption then costs tearing, which is
// visible and reportable, instead of a console that has to be powered off.
static constexpr int FLIP_WAIT_US    = 200;
static constexpr int FLIP_WAIT_LIMIT = 250000 / FLIP_WAIT_US;

static void wait_flip() {
    for (int spins = 0; gcmGetFlipStatus() != 0; spins++) {
        if (spins >= FLIP_WAIT_LIMIT) {
            // Once, not every frame: at 60Hz a per-frame trace would itself
            // become the thing slowing the loop down.
            static bool warned = false;
            if (!warned) {
                warned = true;
                ps3_trace("wait_flip: timed out -- flip never retired. "
                          "gcmGetFlipStatus polarity may be inverted "
                          "(see wait_flip in src/ps3/video.cpp)");
            }
            break;
        }
        sysUsleep(FLIP_WAIT_US);
    }
    gcmResetFlipStatus();
}

// Nearest-neighbour integer upscale of the staging buffer into the back
// buffer. Written as a row-replicate (scale one row of pixels, then memcpy
// that finished row to the other scale-1 rows beneath it) rather than a
// per-destination-pixel double loop: the inner work per output row then
// happens once instead of `scale` times, and the duplicate rows go through
// memcpy, which is what the row-copy actually is.
static void blit_scaled() {
    u32 *const dst_base = display_buf[back_buf]
                        + static_cast<size_t>(origin_y) * stride_px + origin_x;
    const int row_px = VP_W * scale;

    for (int sy = 0; sy < VP_H; sy++) {
        const u32 *src = staging + static_cast<size_t>(sy) * VP_W;
        u32 *row = dst_base + static_cast<size_t>(sy) * scale * stride_px;

        u32 *out = row;
        for (int sx = 0; sx < VP_W; sx++) {
            const u32 p = src[sx];
            for (int k = 0; k < scale; k++) *out++ = p;
        }
        for (int k = 1; k < scale; k++)
            std::memcpy(row + static_cast<size_t>(k) * stride_px, row,
                        static_cast<size_t>(row_px) * sizeof(u32));
    }
}

static void present_frame() {
    emu::GenerateFrame(staging, VP_W);
    blit_scaled();
    queue_flip(back_buf);
    wait_flip();
    back_buf = (back_buf + 1) % NUM_BUFFERS;
}

static void present_blank() {
    // Only the image area is cleared, not the whole buffer: the pillar/letterbox
    // margin is cleared once in irq::init() and nothing ever writes outside the
    // image area afterwards, so it simply stays black -- same arrangement as
    // the PSP backend's letterbox bars.
    u32 *const dst_base = display_buf[back_buf]
                        + static_cast<size_t>(origin_y) * stride_px + origin_x;
    for (int y = 0; y < VP_H * scale; y++)
        std::memset(dst_base + static_cast<size_t>(y) * stride_px, 0,
                    static_cast<size_t>(VP_W) * scale * sizeof(u32));
    queue_flip(back_buf);
    wait_flip();
    back_buf = (back_buf + 1) % NUM_BUFFERS;
}

namespace video {

void WaitForPresent() {
    if (quit) return;
    if (!init_ok) {
        // See ::init_ok. sysutil is still pumped so the XMB can get us out.
        static bool warned = false;
        if (!warned) {
            warned = true;
            ps3_trace("WaitForPresent: irq::init() did not complete -- "
                      "presenting nothing (see the init trace above)");
        }
        ps3_poll_sysutil();
        irq::irqPendingValid = false;
        nmi_vector();
        return;
    }

    if (ppu::PPUMASK & (ppu::mask::BG | ppu::mask::SPRITE)) {
        present_frame();
    } else {
        present_blank();
    }

    // The XMB "quit game" request arrives through this, so it has to be pumped
    // every frame -- see ps3_poll_sysutil().
    ps3_poll_sysutil();

    /* No IRQs permitted post-frame; discard anything still queued from this
     * frame's render before NMI enqueues for the next one. */
    irq::irqPendingValid = false;
    nmi_vector();
}

}   // namespace video

void irq::init() {
    // The core owns VideoRAM/paletteRAM. video::vram_bytes() (video.hpp)
    // resolves to the NES-hardware minimum here, the same as the PS2 branch:
    // this viewport is the NES's own native 32x30, so there is no wide-viewport
    // column-bank multiplier like the PSP's 60-tile one.
    emu::InitMemory(video::vram_bytes());

    // rsxInit needs its IO area in main memory, 1MB-aligned. memalign (not
    // rsxMemalign): this buffer is what gets MAPPED into the RSX's IO address
    // space by rsxInit itself, so it has to be ordinary main memory first.
    //
    // Note the out-parameter form. Current PSL1GHT declares
    //   s32 rsxInit(gcmContextData **context, u32 cmdSize, u32 ioSize, const void *ioAddress)
    // where older releases returned the context directly
    // (gcmContextData *rsxInit(u32, u32, const void *)). Both the return code
    // and the context are checked, matching PSL1GHT's own videoTest sample,
    // which treats a null context as the real failure signal.
    ps3_trace("irq::init: entered");

    void *host_addr = memalign(HOST_SIZE, HOST_SIZE);
    if (host_addr == nullptr) {
        ps3_trace("irq::init: memalign for the RSX IO area FAILED");
        return;
    }
    if (rsxInit(&context, CB_SIZE, HOST_SIZE, host_addr) != 0 || context == nullptr) {
        ps3_trace("irq::init: rsxInit FAILED");
        return;
    }
    ps3_trace("irq::init: rsxInit ok");

    // Take the video mode as the user has it configured rather than forcing
    // one: a 480p-only SDTV and a 1080p panel both work, they just get
    // different scale factors out of the arithmetic below.
    videoState state;
    if (videoGetState(0, 0, &state) != 0) {
        ps3_trace("irq::init: videoGetState FAILED");
        return;
    }
    // state.state != 0 means the display is disabled (no cable, or the console
    // is mid-mode-change); flipping into it would present nothing. The
    // videoTest sample bails here too.
    if (state.state != 0) {
        ps3_trace("irq::init: video output is disabled (state.state != 0)");
        return;
    }
    videoResolution res;
    if (videoGetResolution(state.displayMode.resolution, &res) != 0) {
        ps3_trace("irq::init: videoGetResolution FAILED");
        return;
    }

    screen_w  = res.width;
    screen_h  = res.height;
    stride_px = screen_w;                       // tightly packed, 4 bytes/pixel

    videoConfiguration vconfig;
    std::memset(&vconfig, 0, sizeof(vconfig));
    vconfig.resolution = state.displayMode.resolution;
    vconfig.format     = VIDEO_BUFFER_FORMAT_XRGB;
    vconfig.aspect     = state.displayMode.aspect;
    vconfig.pitch      = static_cast<u32>(stride_px) * sizeof(u32);
    if (videoConfigure(0, &vconfig, nullptr, 0) != 0) {
        ps3_trace("irq::init: videoConfigure FAILED");
        return;
    }

    // videoConfigure's last argument is its "blocking" flag and this passes 0,
    // so the call returns before the mode change has actually landed -- the
    // panel is still re-syncing. Setting display buffers and flipping into a
    // display that is mid-mode-change is not a defined thing to do, so wait
    // for the output to report itself enabled again first. Bounded (~500ms)
    // for the same reason wait_flip() is: a wait that cannot end is worse than
    // a wrong picture.
    for (int tries = 0; tries < 500; tries++) {
        if (videoGetState(0, 0, &state) == 0 && state.state == 0) break;
        sysUsleep(1000);
    }
    if (state.state != 0) {
        ps3_trace("irq::init: video output never re-enabled after "
                  "videoConfigure -- continuing anyway");
    }
    ps3_trace("irq::init: video mode configured");

    // Tie flips to vertical refresh. Without this the flip happens as soon as
    // the RSX reaches the command, which tears and flickers -- PSL1GHT's own
    // rsx.h documents GCM_FLIP_VSYNC as what you normally want, and this is
    // also what makes wait_flip() below a real 50/60Hz frame pace rather than
    // just "the RSX got there".
    gcmSetFlipMode(GCM_FLIP_VSYNC);

    // Largest integer scale that fits both axes, floor'd, never below 1 -- see
    // the file header for why integer and not stretch-to-fill. The `max(1, ...)`
    // is not hypothetical defensiveness: a 256x240 image does not fit whole
    // into some of the narrow 1080i-derived modes PSL1GHT can report
    // (960x1080 and friends), and 0 would make the blit write nothing.
    const int fit_x = screen_w / VP_W;
    const int fit_y = screen_h / VP_H;
    scale = fit_x < fit_y ? fit_x : fit_y;
    if (scale < 1) scale = 1;

    origin_x = (screen_w - VP_W * scale) / 2;
    origin_y = (screen_h - VP_H * scale) / 2;
    if (origin_x < 0) origin_x = 0;
    if (origin_y < 0) origin_y = 0;

    // Display buffers come from rsxMemalign (RSX-visible memory, 64-byte
    // aligned) rather than plain malloc: gcmSetDisplayBuffer takes an RSX
    // offset, which only exists for memory in that heap.
    const size_t buf_bytes = static_cast<size_t>(stride_px) * screen_h * sizeof(u32);
    for (int i = 0; i < NUM_BUFFERS; i++) {
        display_buf[i] = static_cast<u32 *>(rsxMemalign(64, buf_bytes));
        if (display_buf[i] == nullptr) {
            ps3_trace("irq::init: rsxMemalign for a display buffer FAILED");
            return;
        }
        rsxAddressToOffset(display_buf[i], &display_off[i]);
        gcmSetDisplayBuffer(static_cast<u8>(i), display_off[i], vconfig.pitch,
                            static_cast<u32>(screen_w), static_cast<u32>(screen_h));
        // Both buffers start as whatever was in that memory. The per-frame
        // paths only ever touch the image area, so the pillar/letterbox margin
        // needs exactly one real clear -- here -- to read as black instead of
        // boot garbage. Same reasoning as the PSP backend's one-time clear.
        std::memset(display_buf[i], 0, buf_bytes);
    }

    // Queue the first flip so buffer 0 is the one on screen, then start
    // drawing into buffer 1 -- classic double buffering, matching what the
    // PSP backend does with back_buf after its IMMEDIATE set.
    gcmResetFlipStatus();
    queue_flip(0);
    wait_flip();
    back_buf = 1;
    ps3_trace("irq::init: first flip retired");

    ps3_setup_exit_callback();
    input_init();

    init_ok = true;
    ps3_trace("irq::init: complete");
}

void irq::post() {
    input_exit();
    // The RSX context and its buffers are deliberately not torn down: the
    // process is exiting, and lv2 reclaims the RSX mapping and the memory with
    // it. Freeing them here would mean racing the in-flight flip the exit path
    // has no reason to wait on.
}
