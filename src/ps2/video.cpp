/**
 * @file video.cpp
 * @brief Sony PlayStation 2 presentation backend for the shared software PPU
 *        (ps2sdk + gsKit).
 *
 * The PPU emulation itself -- ::emu::GenerateFrame and every
 * nametable/attribute/palette/OAM write function plus the scroll and
 * sprite-zero machinery -- lives in src/emu/ppu.cpp and is shared verbatim
 * with the SDL3, libogc/GX, 3DS, Switch, Wii U and PSP backends. This file is
 * only the PS2-specific half.
 *
 * Unlike PSP (which writes straight into VRAM through an uncached alias) the
 * Graphics Synthesiser's VRAM is not CPU-addressable at all: every pixel has
 * to arrive via a GIF/DMA transfer, which is exactly what a textured quad is
 * for. So this renders the NES's native 256x240 frame into an ARGB8888
 * staging buffer in ordinary EE RAM (video::viewport_px() x viewport_py(),
 * TARGET_PS2 in video.hpp -- native resolution, not scaled up at the source,
 * since the GS's own quad-scaling makes that unnecessary; see that branch's
 * comment for why), uploads it as one GS texture each frame (gsKit_texture_
 * upload, a synchronous GIF transfer), and draws it as a single scaled
 * gsKit_prim_sprite_texture quad -- the same "render once, let the GPU scale
 * it" shape as the Switch/Wii U SDL backends, just through gsKit instead of
 * SDL2.
 *
 * The PS2 is a 4:3-only console with no hardware widescreen path worth using
 * here, so unlike the Switch/Wii U branch this does not render extra world:
 * it stretches the native NES frame to fill a true 4:3 area of the screen
 * (matching the NES's own non-square-pixel CRT aspect, same convention most
 * NES emulators default to) and pillarboxes the remainder, rather than
 * inventing a wider viewport the way GC/Switch/Wii U do for their own 16:9
 * or runtime-detected panels.
 *
 * Pixel format: the core fills ARGB8888 (0xAARRGGBB as a native u32). GS_PSM_
 * CT32 is byte order R,G,B,A in memory (0xAABBGGRR as a LE u32) -- the exact
 * same convention the Switch and PSP backends hit for the exact same reason
 * -- so this applies the identical R/B channel swap those backends use.
 *
 * Display mode is fixed at NTSC 640x448 for now; there is no PAL branch yet
 * (unlike the NES target's own nes/nes_pal split) -- REGION only ever
 * affects playback timing/tables on this backend, never the screen mode.
 *
 * Only NTSC/no-region-switch is implemented. The present is vsync-paced by
 * an INTC vblank handler and a kernel semaphore rather than gsKit_sync_flip
 * -- see ::vblank_sema for why that distinction matters well beyond this
 * file.
 */
#include "internal.hpp"

// gsKit/dmaKit must be fully parsed before pnes/interrupts.hpp: off
// NES that header's RESET macro expands to a whole main()-defining sequence
// wherever the bare token RESET appears afterward, and gsInit.h's own
// gsRegisters struct happens to have a bitfield literally named RESET. Macro
// expansion only affects tokens seen AFTER the #define, so parsing gsKit
// first (its RESET is an ordinary struct member, no macro active yet) and
// only defining the engine's RESET macro afterward avoids the collision
// entirely -- no other backend's SDK has a symbol named RESET, so this
// ordering requirement is PS2-only.
#include <sifrpc.h>
#include <kernel.h>
#include <gsKit.h>
#include <dmaKit.h>

#include <pnes/video.hpp>
#include <pnes/interrupts.hpp>
#include <pnes/logger.hpp>
#include "../emu/emu.hpp"

#include <cstring>

// ---------------------------------------------------------------------------
// PS2-specific state. Everything the core needs (VideoRAM, paletteRAM,
// scroll, PPUCTRL/PPUMASK, the OAM snapshot, patternTable, ...) is owned by
// src/emu; only the gsKit/GS objects live here.
// ---------------------------------------------------------------------------
static constexpr int VP_W = video::viewport_px();   // 256 -- NES-native, see video.hpp's TARGET_PS2 branch
static constexpr int VP_H = video::viewport_py();   // 240 -- NES-native

static GSGLOBAL  *gsGlobal = nullptr;
static GSTEXTURE  frameTex;

// The core's ARGB8888 staging buffer. gsKit DMAs straight out of this on
// every gsKit_texture_upload call, so it doubles as frameTex.Mem -- no
// separate "staging then upload" copy.
alignas(64) static u32 frameBuf[VP_W * VP_H];

// Fixed NTSC output size (see file header: no PAL branch yet).
static constexpr float SCREEN_W = 640.0f;
static constexpr float SCREEN_H = 448.0f;

// Stretch the native NES frame to a true 4:3 area of the screen and
// pillarbox the rest, rather than filling the whole (non-4:3-shaped) 640x448
// buffer -- see the file header for why this differs from the Switch/Wii U
// "render extra world" approach.
static constexpr float QUAD_H  = SCREEN_H;
static constexpr float QUAD_W  = QUAD_H * 4.0f / 3.0f;
static constexpr float QUAD_X0 = (SCREEN_W - QUAD_W) / 2.0f;
static constexpr float QUAD_Y0 = 0.0f;

// ---------------------------------------------------------------------------
// Frame pacing.
//
// gsKit's own gsKit_sync_flip() paces by busy-polling the GS CSR vsync bit
// (that's all gsKit_vsync_wait does), which never enters an EE kernel wait.
// A main thread that only ever spins is impossible for the kernel to schedule
// around: there is no point at which it hands control back, so a worker
// thread at equal or lower priority is never picked up at all.
//
// So vblank is taken as a real interrupt here and turned into a semaphore.
// WaitForPresent() blocks in WaitSema, parking the main thread in the kernel
// for the gap between "frame submitted" and "next vblank" instead of burning
// the EE on a register poll for it.
//
// Note what this does NOT buy, because an earlier revision of src/ps2/audio.
// cpp assumed it did: it is not a way to run a background thread below main.
// WaitSema returns immediately whenever a vblank is already pending, which is
// exactly the case when a frame overruns its budget -- so the donated window
// shrinks to nothing under load, and a strictly-lower-priority worker starves
// precisely when the frame is heaviest. The audio decode thread therefore
// sits ABOVE main and blocks on its own hardware event; see that file's
// Init() comment.
//
// max_count 1: the count saturates instead of accumulating a vblank backlog,
// so a frame that overruns its budget costs exactly one frame of pacing
// rather than leaving behind a queue of instantly-satisfied waits. A vblank
// landing while main is still rendering is still recorded, so the next
// WaitSema returns immediately rather than idling an entire extra field.
static s32 vblank_sema = -1;

// Interrupt context: iSignalSema, not SignalSema (the i-prefixed variants are
// the ones callable from an ISR). A signal past max_count is rejected by the
// kernel, which is exactly the saturation described above -- there is nothing
// to report, so the return value is deliberately ignored.
//
// ExitHandler() is not optional: ps2sdk documents it (ee/include/kernel.h) as
// a workaround for an EE kernel bug that must be run immediately before
// returning from ANY interrupt handler. It expands to `sync; ei`, nothing
// heavier.
static s32 vblank_handler(s32) {
    iSignalSema(vblank_sema);
    ExitHandler();
    return 0;
}

// Installed from irq::init(); leaves ::vblank_sema at -1 if either half fails,
// which present_flip() below falls back on.
static void vblank_pacing_init() {
    ee_sema_t sema{};
    sema.init_count = 0;
    sema.max_count  = 1;
    const s32 sid = CreateSema(&sema);
    if (sid < 0) {
        log("irq::init: CreateSema failed (%d), falling back to gsKit CSR spin", sid);
        return;
    }
    vblank_sema = sid;

    if (AddIntcHandler(INTC_VBLANK_S, vblank_handler, 0) < 0) {
        log("irq::init: AddIntcHandler(INTC_VBLANK_S) failed, falling back to gsKit CSR spin");
        vblank_sema = -1;
        return;
    }
    EnableIntc(INTC_VBLANK_S);
}

// Stands in for gsKit_sync_flip(): the same double-buffer swap, but the wait
// ahead of it is the kernel semaphore above rather than gsKit's CSR spin.
//
// gsKit_sync_flip() cannot simply be called after the semaphore wait instead:
// its internal gsKit_vsync_wait() clears the CSR vsync flag and then waits
// for the NEXT one, so calling it from inside vblank would block out a whole
// additional field and halve the frame rate. The two documented halves of its
// double-buffer body are called directly instead -- gsKit_display_buffer
// points the GS at the buffer just drawn, gsKit_switch_context moves drawing
// to the other one. gsKit_setactive() is then called explicitly: switch_
// context is documented to establish the new active buffer itself, and
// re-writing the same FRAME/SCISSOR register values is idempotent, so this is
// correct whether or not it already did.
static void present_flip() {
    gsKit_queue_exec(gsGlobal);

    if (vblank_sema >= 0) WaitSema(vblank_sema);
    else                  gsKit_vsync_wait();

    gsKit_display_buffer(gsGlobal);
    gsKit_switch_context(gsGlobal);
    gsKit_setactive(gsGlobal);
}

// The core fills ARGB8888 (0xAARRGGBB); GS_PSM_CT32 wants byte order
// R,G,B,A in memory (0xAABBGGRR as a LE u32) -- see the file header.
static inline u32 argb_to_rgba(const u32 p) {
    return (p & 0xFF00FF00u) | ((p & 0x00FF0000u) >> 16) | ((p & 0x000000FFu) << 16);
}

// Render one frame through the shared core into frameBuf, swap channel
// order in place, then hand the whole buffer to the GS as a texture and
// draw it as a single scaled quad.
static void present_frame() {
    emu::GenerateFrame(frameBuf, VP_W);

    for (int i = 0; i < VP_W * VP_H; i++) frameBuf[i] = argb_to_rgba(frameBuf[i]);

    // gsKit_texture_upload is a synchronous GIF/DMA transfer (it waits for
    // its own DMA to finish before returning), and present_flip below
    // blocks until the GS has actually finished drawing this frame before
    // the next present_frame() call is reached -- so re-uploading the same
    // VRAM texture region every frame is safe with no double buffering of
    // the texture itself, unlike the screen buffers (which DoubleBuffering
    // handles automatically).
    gsKit_texture_upload(gsGlobal, &frameTex);

    gsKit_clear(gsGlobal, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x00, 0x00));
    gsKit_prim_sprite_texture(gsGlobal, &frameTex,
                               QUAD_X0, QUAD_Y0, 0.0f, 0.0f,
                               QUAD_X0 + QUAD_W, QUAD_Y0 + QUAD_H, static_cast<float>(VP_W), static_cast<float>(VP_H),
                               1, GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00));

    present_flip();
}

static void present_blank() {
    gsKit_clear(gsGlobal, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x00, 0x00));
    present_flip();
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
    log("irq::init: entered");
    // Brings up the IOP side RPC link; every SifLoadModule/pad/audsrv call
    // below depends on it, so it has to be the very first thing this does.
    sceSifInitRpc(0);
    log("irq::init: sceSifInitRpc returned");

    // The core owns VideoRAM/paletteRAM. video::vram_bytes() (video.hpp): the
    // viewport is the NES's own native 32x30, so this resolves to the
    // NES-hardware minimum (2 pages/0x800 bytes), same as the plain SDL path.
    emu::InitMemory(video::vram_bytes());
    log("irq::init: emu::InitMemory returned");

    gsGlobal = gsKit_init_global();
    log("irq::init: gsKit_init_global returned");
    gsGlobal->Mode            = gsKit_check_rom();   // GS_MODE_NTSC or GS_MODE_PAL, from the console's own ROM
    // 640x448 is the standard NTSC *interlaced* display size -- gsKit_init_
    // global_custom() leaves these two at 0 (GS_NONINTERLACED/GS_FIELD), an
    // interlace/field combination that doesn't match a 448-line framebuffer.
    // Every gsKit sample sets this pair explicitly for exactly that reason;
    // leaving it unset is an invalid video-timing combination that some
    // displays/emulators show as no signal at all rather than a garbled
    // picture.
    gsGlobal->Interlace       = GS_INTERLACED;
    gsGlobal->Field           = GS_FIELD;
    gsGlobal->Width           = static_cast<int>(SCREEN_W);
    gsGlobal->Height          = static_cast<int>(SCREEN_H);
    gsGlobal->PSM             = GS_PSM_CT24;
    gsGlobal->PSMZ            = GS_PSMZ_16S;
    gsGlobal->DoubleBuffering = GS_SETTING_ON;
    gsGlobal->ZBuffering      = GS_SETTING_OFF;       // flat 2D textured quad; no depth test needed
    // The single quad this backend draws is fully opaque (every pixel's
    // alpha byte is a constant 0xFF -- see emu::GenerateFrame) and the
    // screen's own PSM (GS_PSM_CT24) carries no alpha channel at all, so
    // enabling GS alpha blending here bought nothing but a destination
    // format/blend-equation mismatch that gsKit doesn't configure an
    // explicit GS_SETREG_ALPHA for -- in practice that left the quad
    // blending to black instead of just drawing over the cleared buffer.
    gsGlobal->PrimAlphaEnable = GS_SETTING_OFF;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
                D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    log("irq::init: dmaKit init returned");

    gsKit_init_screen(gsGlobal);
    log("irq::init: gsKit_init_screen returned");
    gsKit_mode_switch(gsGlobal, GS_ONESHOT);
    gsKit_clear(gsGlobal, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x00, 0x00));
    log("irq::init: gsKit_clear returned");

    // gsKit_init_screen leaves FirstFrame set so that the first gsKit_sync_
    // flip() skips its vsync-and-swap; present_flip() doesn't consult it (it
    // always swaps, which is correct from the very first frame -- there is
    // nothing displayed yet that the swap could discard), so clear it here
    // rather than leave a stale "no frame presented yet" flag behind for any
    // other gsKit entry point to read.
    gsGlobal->FirstFrame = GS_SETTING_OFF;

    vblank_pacing_init();
    log("irq::init: vblank pacing installed (sema=%d)", vblank_sema);

    frameTex.Width  = VP_W;
    frameTex.Height = VP_H;
    frameTex.PSM    = GS_PSM_CT32;
    frameTex.Filter = GS_FILTER_NEAREST;   // crisp NES pixels, same convention every other backend keeps
    frameTex.Mem    = frameBuf;
    frameTex.Vram   = gsKit_vram_alloc(gsGlobal,
                                        gsKit_texture_size_ee(frameTex.Width, frameTex.Height, frameTex.PSM),
                                        GSKIT_ALLOC_USERBUFFER);

    std::memset(frameBuf, 0, sizeof(frameBuf));
    log("irq::init: texture/frameBuf setup done");

    input_init();
    log("irq::init: input_init returned");
}

void irq::post() {
    // No GS/gsKit teardown call exists in this codebase's other console
    // backends either (OGC/3DS/Switch/Wii U all leave their GPU state as-is
    // on exit) -- there is no supported "return to the BIOS browser cleanly"
    // path for PS2 homebrew in general; a real reset is the only exit.
}
