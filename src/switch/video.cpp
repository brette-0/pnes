/**
 * @file video.cpp
 * @brief Nintendo Switch presentation backend for the shared software PPU.
 *
 * The PPU emulation itself -- ::emu::GenerateFrame and every
 * nametable/attribute/palette/OAM write function plus the scroll and
 * sprite-zero machinery -- lives in src/emu/ppu.cpp and is shared verbatim with
 * the SDL3, libogc/GX and 3DS backends. This file is only the Switch-specific
 * half: it opens a libnx framebuffer on the default window, asks the core to
 * render one widescreen ARGB frame into a staging buffer, and scale-blits that
 * to fill the 720p framebuffer (the Tegra is hugely overpowered for
 * NES-resolution pixels, so a straight CPU scale-blit is effectively free and
 * avoids pulling in deko3d/EGL for a first pass).
 *
 * Widescreen: the Switch is a 16:9 console, so rather than pillarbox a 4:3 NES
 * frame we render a WIDER viewport -- video::viewport_px() x viewport_py()
 * (416x240, ~16:9) -- which shows MORE of the game world horizontally, exactly
 * the same "render more world" model the SDL desktop LANDSCAPE path uses. The
 * 240-tall source maps to 720 at an exact integer 3x vertically; horizontally
 * 416 -> 1280 is a near-3x nearest-neighbour stretch (a handful of duplicated
 * columns, imperceptible) so the image fills the whole panel edge-to-edge with
 * no bars. The present is vsync-locked by libnx, providing 60Hz frame pacing.
 */
#include "internal.hpp"

#include <pnes/video.hpp>
#include <pnes/interrupts.hpp>
#include "../emu/emu.hpp"

#include <cstring>
#include <switch.h>

// ---------------------------------------------------------------------------
// Switch-specific state. Everything the core needs (VideoRAM, paletteRAM,
// scroll, PPUCTRL/PPUMASK, the OAM snapshot, patternTable, ...) is owned by
// src/emu; only the genuine libnx objects (and the nearest-neighbour scale
// maps) live here.
// ---------------------------------------------------------------------------
static constexpr int VP_W = video::viewport_px();   // 416 (widescreen viewport)
static constexpr int VP_H = video::viewport_py();   // 240
static constexpr int FB_W = 1280;
static constexpr int FB_H = 720;

static Framebuffer fb;
static u32 staging[VP_W * VP_H];

// Nearest-neighbour source-index maps: dest column dx samples staging column
// colmap[dx], dest row dy samples staging row rowmap[dy]. Precomputed once in
// init() so the per-frame blit is a pure gather (no per-pixel divides).
static int colmap[FB_W];
static int rowmap[FB_H];

// The core fills ARGB8888 (0xAARRGGBB). The libnx framebuffer is
// PIXEL_FORMAT_RGBA_8888 (byte order R,G,B,A == 0xAABBGGRR as a LE u32), so swap
// the R and B channels. Done once per *source* pixel (VP_W*VP_H/frame), in place
// in staging, before the scale expansion -- negligible cost.
static inline u32 argb_to_rgba(const u32 p) {
    return (p & 0xFF00FF00u) | ((p & 0x00FF0000u) >> 16) | ((p & 0x000000FFu) << 16);
}

// Render one frame through the shared core into the staging buffer, then
// nearest-neighbour scale it to fill the entire libnx framebuffer.
static void present_frame() {
    u32 stride_bytes;
    auto *fbptr = static_cast<u8 *>(framebufferBegin(&fb, &stride_bytes));
    const u32 stride_px = stride_bytes / 4;

    emu::GenerateFrame(staging, VP_W);

    // Pre-swap the whole source frame to RGBA so the gather below is a plain copy.
    for (int i = 0; i < VP_W * VP_H; i++) staging[i] = argb_to_rgba(staging[i]);

    auto *out = reinterpret_cast<u32 *>(fbptr);
    for (int dy = 0; dy < FB_H; dy++) {
        const u32 *srow = &staging[rowmap[dy] * VP_W];
        u32 *drow = out + static_cast<size_t>(dy) * stride_px;
        for (int dx = 0; dx < FB_W; dx++) drow[dx] = srow[colmap[dx]];
    }

    framebufferEnd(&fb);
}

static void present_blank() {
    u32 stride_bytes;
    auto *fbptr = static_cast<u8 *>(framebufferBegin(&fb, &stride_bytes));
    std::memset(fbptr, 0, static_cast<size_t>(stride_bytes) * FB_H);
    framebufferEnd(&fb);
}

namespace video {

void WaitForPresent() {
    // appletMainLoop() goes false when the system asks us to exit (HOME-menu
    // close, etc.); mirror the SDL backend's quit flag so main() unwinds.
    if (!appletMainLoop()) {
        quit = 1;
        return;
    }

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
    // The core owns VideoRAM/paletteRAM. video::vram_bytes() (video.hpp): the
    // 52-tile-wide widescreen viewport is wider than the NES's native 32, so
    // this resolves to double the NES-hardware minimum (4 pages/0x1000 bytes).
    emu::InitMemory(video::vram_bytes());

    // Build the nearest-neighbour scale maps once. Vertically this is exact 3x
    // (240*3 == 720); horizontally it is a near-3x stretch (416 -> 1280).
    for (int dx = 0; dx < FB_W; dx++) colmap[dx] = dx * VP_W / FB_W;
    for (int dy = 0; dy < FB_H; dy++) rowmap[dy] = dy * VP_H / FB_H;

    NWindow *win = nwindowGetDefault();
    framebufferCreate(&fb, win, FB_W, FB_H, PIXEL_FORMAT_RGBA_8888, 2);
    framebufferMakeLinear(&fb);

    input_init();
}

void irq::post() {
    framebufferClose(&fb);
}
