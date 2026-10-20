/**
 * @file video.cpp
 * @brief Sony PSP presentation backend for the shared software PPU.
 *
 * The PPU emulation itself -- ::emu::GenerateFrame and every
 * nametable/attribute/palette/OAM write function plus the scroll and
 * sprite-zero machinery -- lives in src/emu/ppu.cpp and is shared verbatim
 * with the SDL3, libogc/GX, 3DS, Switch and Wii U backends. This file is only
 * the PSP-specific half: it asks the core to render straight into the VRAM
 * back buffer, at native resolution, with no scale step, and letterboxes the
 * result to fill the panel.
 *
 * No GU (the PSP's GE/3D pipeline) is used, and there is deliberately no
 * scale-blit of any kind -- every destination pixel is exactly one source
 * pixel. video::viewport_px() (video.hpp) matches the panel's width exactly
 * (480), but video::viewport_py() is capped at the NES's native 240 rather
 * than the panel's full 272: the shared core's vertical walk wraps at 240px
 * (see video.hpp's viewport_ty() comment for why -- there is no vertical
 * equivalent of the horizontal nt_cols extension, so a viewport taller than
 * that reads back into row 0 of the nametable partway down the screen,
 * which is exactly the mirroring a first cut of this backend hit when it
 * tried to render the full 272px height). So instead of either that or
 * scaling (which was this backend's actual first cut, before that: a
 * 416x240 render stretched to 480x272 -- non-integer on both axes,
 * unavoidably warped), the 480x240 render is letterboxed inside the 480x272
 * panel with a fixed 16px black bar top and bottom.
 *
 * VRAM is written through its uncached alias (0x44000000) so plain stores are
 * visible to the display controller with no explicit cache flush. The PSP
 * boots at a throttled clock (222/222/111 MHz) until a module explicitly
 * raises it, so irq::init() below calls scePowerSetClockFrequency(333, 333,
 * 166) once at startup -- without it this (and everything else on the CPU)
 * runs at 2/3 speed for no reason. The present is vsync-paced by
 * sceDisplayWaitVblankStart(), providing 60Hz frame pacing.
 */
#include "internal.hpp"

#include <pnes/video.hpp>
#include <pnes/interrupts.hpp>
#include "../emu/emu.hpp"

#include <cstring>
#include <pspkernel.h>
#include <pspdisplay.h>
#include <psppower.h>

PSP_MODULE_INFO(PROJECT_NAME, 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

// Standard PSP HOME-menu exit handling: a dedicated thread registers a
// callback that the firmware invokes when the user asks to quit (HOME menu,
// eject, power). It just flips the shared ::quit flag -- the same one
// video::WaitForPresent() and the demo's own RESET loop already poll -- so
// the process unwinds through the ordinary post()/return path instead of the
// firmware forcibly killing it mid-frame.
static int exit_callback(int, int, void *) {
    quit = 1;
    return 0;
}

static int callback_thread(SceSize, void *) {
    const int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, nullptr);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

void psp_setup_exit_callback() {
    const int thid = sceKernelCreateThread("exit_callback_thread", callback_thread, 0x11, 0xFA0, 0, nullptr);
    if (thid >= 0) sceKernelStartThread(thid, 0, nullptr);
}

// ---------------------------------------------------------------------------
// PSP-specific state. Everything the core needs (VideoRAM, paletteRAM,
// scroll, PPUCTRL/PPUMASK, the OAM snapshot, patternTable, ...) is owned by
// src/emu; only the genuine VRAM buffers live here.
// ---------------------------------------------------------------------------
static constexpr int PANEL_W = 480;                   // real screen width
static constexpr int PANEL_H = 272;                   // real screen height
static constexpr int VP_W = video::viewport_px();     // 480 -- matches PANEL_W exactly
static constexpr int VP_H = video::viewport_py();     // 240 -- capped below PANEL_H, see file header
static constexpr int LETTERBOX_Y = (PANEL_H - VP_H) / 2;   // 16px black bar, top and bottom

static_assert(VP_W == PANEL_W, "PSP viewport width must match the panel exactly -- no horizontal scale step exists here");
static_assert(VP_H <= PANEL_H, "PSP viewport height must fit within the panel -- see viewport_ty()'s comment (video.hpp) on why it can't just follow PANEL_H");

// VRAM display buffers must be a stride the display controller accepts; 512
// (a power of two, >= PANEL_W) is the standard PSP convention for every pixel
// format, 8888 included. Two such buffers (0x88000 bytes each) comfortably
// fit inside the PSP's 2MB VRAM. The uncached alias (0x44000000, not the
// cached 0x04000000) is used so CPU stores land without an explicit
// sceKernelDcacheWritebackAll -- the display controller only ever sees VRAM
// through its own bus, never the CPU cache.
static constexpr int VRAM_STRIDE = 512;
static constexpr uintptr_t VRAM_UNCACHED_BASE = 0x44000000;
static u32 *const vram_buf[2] = {
    reinterpret_cast<u32 *>(VRAM_UNCACHED_BASE),
    reinterpret_cast<u32 *>(VRAM_UNCACHED_BASE + VRAM_STRIDE * PANEL_H * sizeof(u32)),
};
static int back_buf = 0;   // index of vram_buf[] this frame draws into

// The core fills ARGB8888 (0xAARRGGBB). The PSP's 8888 display format is
// byte order R,G,B,A in memory (0xAABBGGRR as a LE u32, same convention as
// libnx's RGBA_8888), so swap the R and B channels -- same transform the
// Switch backend applies for the same reason.
static inline u32 argb_to_rgba(const u32 p) {
    return (p & 0xFF00FF00u) | ((p & 0x00FF0000u) >> 16) | ((p & 0x000000FFu) << 16);
}

// Render one frame through the shared core straight into the VRAM back
// buffer's middle VP_H rows -- GenerateFrame takes the destination stride
// directly, so this needs no separate staging buffer or blit of any kind,
// scaled or otherwise. One in-place pass over exactly what was written swaps
// channel order. The top/bottom LETTERBOX_Y bars are never touched here:
// irq::init() clears both buffers to black once, up front, and nothing after
// that ever writes outside the VP_H band, so they simply stay black.
static void present_frame() {
    u32 *const out = vram_buf[back_buf];
    u32 *const vp  = out + static_cast<size_t>(LETTERBOX_Y) * VRAM_STRIDE;
    emu::GenerateFrame(vp, VRAM_STRIDE);

    for (int py = 0; py < VP_H; py++) {
        u32 *row = vp + static_cast<size_t>(py) * VRAM_STRIDE;
        for (int px = 0; px < VP_W; px++) row[px] = argb_to_rgba(row[px]);
    }

    // Request the swap for the next vsync, then wait for it: the CPU always
    // draws into the buffer NOT currently on screen (classic PSP raw-VRAM
    // double buffering, no GU/GE swap chain involved).
    sceDisplaySetFrameBuf(vram_buf[back_buf], VRAM_STRIDE,
                          PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitVblankStart();
    back_buf ^= 1;
}

static void present_blank() {
    // Blanks the WHOLE buffer, letterbox bars included -- harmless, since
    // present_frame() never writes those bars either, so re-zeroing them here
    // is a no-op in practice, not a special case.
    std::memset(vram_buf[back_buf], 0, static_cast<size_t>(VRAM_STRIDE) * PANEL_H * sizeof(u32));
    sceDisplaySetFrameBuf(vram_buf[back_buf], VRAM_STRIDE,
                          PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitVblankStart();
    back_buf ^= 1;
}

namespace video {

void WaitForPresent() {
    if (quit) return;

    if (ppu::PPUMASK & (ppu::mask::BG | ppu::mask::SPRITE)) {
        present_frame();
    } else {
        present_blank();
    }

    /* No IRQs permitted post-frame; discard anything still queued from this
     * frame's render before NMI enqueues for the next one. */
    irq::irqPendingValid = false;
    nmi_vector();
}

}   // namespace video

void irq::init() {
    // The PSP boots at a throttled clock (222/222/111 MHz) unless a module
    // explicitly asks for more; every real homebrew title raises this once at
    // startup. Without it the CPU-side scale-blit (and everything else) runs
    // at ~2/3 its available speed for no reason. 333/333/166 is the Allegrex's
    // documented max (CPU/bus clocks; pllfreq must match cpufreq here).
    scePowerSetClockFrequency(333, 333, 166);

    // The core owns VideoRAM/paletteRAM. video::vram_bytes() (video.hpp): the
    // 60-tile-wide viewport is wider than the NES's native 32, so this
    // resolves to more than the NES-hardware minimum (see vram_bytes()'s own
    // banks_x/banks_y math -- a 60x30 viewport needs 2 column-banks x 1
    // row-bank, doubled for scroll lookahead).
    emu::InitMemory(video::vram_bytes());

    // Both VRAM buffers start as boot-time garbage. present_frame() only ever
    // writes its middle VP_H band (see that function's own comment); the
    // LETTERBOX_Y bars top and bottom are never touched again after this, so
    // they need exactly one real clear, here, to read as black instead of
    // whatever happened to be in VRAM at boot.
    std::memset(vram_buf[0], 0, static_cast<size_t>(VRAM_STRIDE) * PANEL_H * sizeof(u32));
    std::memset(vram_buf[1], 0, static_cast<size_t>(VRAM_STRIDE) * PANEL_H * sizeof(u32));

    sceDisplaySetMode(0, PANEL_W, PANEL_H);
    sceDisplaySetFrameBuf(vram_buf[0], VRAM_STRIDE,
                          PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_IMMEDIATE);
    back_buf = 1;

    psp_setup_exit_callback();
    input_init();
}

void irq::post() {
    // No handle to release: the raw VRAM buffers are static, and the exit
    // callback thread (registered in init()) is what actually ends the
    // process once quit goes true.
}
