/**
 * @file video.cpp
 * @brief Native Wii U presentation backend for the shared software PPU (SDL2).
 *
 * The PPU emulation itself -- ::emu::GenerateFrame and every
 * nametable/attribute/palette/OAM write function plus the scroll and
 * sprite-zero machinery -- lives in src/emu/ppu.cpp and is shared verbatim with
 * the SDL3, libogc/GX, 3DS and Switch backends. This file is only the Wii-U-
 * specific half: it opens an SDL2 window+renderer (the wiiu portlib drives GX2 +
 * ProcUI + the TV scan buffer underneath), asks the core to render one widescreen
 * ARGB frame straight into a streaming texture, and lets the GPU scale-blit that
 * to fill the window.
 *
 * Widescreen: rather than pillarbox a 4:3 NES frame we render a WIDER viewport
 * -- video::viewport_px() x viewport_py() -- which shows MORE of the game world
 * horizontally, exactly the same "render more world" model the SDL desktop
 * LANDSCAPE path and the Switch backend use. SDL_RenderCopy stretches that to
 * the whole window with bilinear/nearest filtering on the GPU, so the cost is
 * negligible.
 *
 * How wide depends on the TV, which on this console is a system setting rather
 * than a fixed property: Cafe OS supports a Wii U wired to a 4:3 set, and
 * AVMGetTVAspectRatio (wut, avm/tv.h) reports which one the user configured.
 * The viewport follows it -- 52 tiles (416x240, ~16:9) on a widescreen set, 40
 * (320x240, exactly 4:3) on a 4:3 one -- so the aspect of the rendered frame
 * matches the aspect SDL_RenderCopy stretches it into and tiles stay square.
 * The 52-tile viewport used to be hardcoded, which stretched every tile by 4/3
 * on a 4:3 set. This is the same probe-the-display decision src/ogc/video.cpp
 * makes for GC/Wii; the Switch needs none, having no 4:3 mode to configure.
 *
 * This assumes what AVMGetTVAspectRatio exists to tell an app in the first
 * place: that the scan buffer SDL hands us covers the whole picture, so
 * matching the frame's aspect to the configured display aspect is what makes
 * pixels square. If a particular 4:3 setup turns out to pillarbox a 16:9
 * buffer instead of presenting a 4:3 one, the 40-tile branch is the thing to
 * revisit -- the widescreen branch is unchanged from before either way.
 *
 * Pixel format: the core fills ARGB8888 (0xAARRGGBB as a native u32). SDL's
 * SDL_PIXELFORMAT_ARGB8888 is exactly that logical layout regardless of machine
 * endianness, so -- unlike the Switch's byte-order RGBA framebuffer -- no channel
 * swap is needed here even though Espresso is big-endian: GenerateFrame writes
 * straight into the locked texture.
 *
 * The present is vsync-locked by the renderer, providing 60Hz frame pacing.
 */
#define SDL_MAIN_HANDLED   // the demo supplies its own main() (RESET macro)
#include "internal.hpp"

#include <pnes/video.hpp>
#include <pnes/interrupts.hpp>
#include "../emu/emu.hpp"

#include <SDL2/SDL.h>
#include <avm/tv.h>

// ---------------------------------------------------------------------------
// Wii-U-specific state. Everything the core needs (VideoRAM, paletteRAM,
// scroll, PPUCTRL/PPUMASK, the OAM snapshot, patternTable, ...) is owned by
// src/emu; only the SDL2 objects live here.
// ---------------------------------------------------------------------------
// World columns visible this run (tiles) -- declared in video.hpp, resolved
// once in irq::init() below from the TV's configured aspect. NOT static:
// video.hpp's video::viewport_tx() reads it from every other translation unit,
// so it must be set BEFORE anything derived from the viewport is computed --
// the nametable VRAM allocation and the streaming texture both are.
u16 wiiu_world_tx;

// Viewport in pixels. Runtime, not constexpr: video::viewport_px() reads
// wiiu_world_tx above. Only the height is fixed, at the NES's own 240.
static constexpr int VP_H = video::viewport_py();   // 240
static constexpr int WIN_W = 1280;
static constexpr int WIN_H = 720;

static SDL_Window   *window  = nullptr;
static SDL_Renderer *renderer = nullptr;
static SDL_Texture  *frame    = nullptr;

// Render one frame through the shared core directly into the streaming texture,
// then let the GPU scale it to fill the whole window.
static void present_frame() {
    void *pixels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(frame, nullptr, &pixels, &pitch) == 0) {
        // pitch is in bytes; GenerateFrame wants a stride in pixels. For a
        // freshly created streaming texture pitch == viewport_px()*4, but
        // honour it.
        emu::GenerateFrame(static_cast<u32 *>(pixels), pitch / 4);

        // Cemu (Vulkan backend) appears to skip presenting a frame whose
        // composited pixels are bit-identical to the last one it actually
        // pushed -- harmless for a genuinely static scene, but can drop a
        // real transition if its "last frame" reference is stale by one,
        // wedging the display until forced different again. Flip the LSB
        // of one pixel every frame so no two consecutive frames are ever
        // bit-identical; the change is below perceptible contrast.
        static u32 parity = 0;
        parity ^= 1;
        static_cast<u32 *>(pixels)[0] ^= parity;

        SDL_UnlockTexture(frame);
    }

    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, frame, nullptr, nullptr);  // src=full tex, dst=full window
    SDL_RenderPresent(renderer);
}

static void present_blank() {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);
}

namespace video {

void WaitForPresent() {
    // Pump the SDL event queue so the wiiu portlib can service ProcUI (HOME
    // menu, foreground/background transitions). SDL_QUIT fires when the system
    // asks us to exit; mirror the desktop backend's quit flag so main() unwinds.
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            quit = 1;
            return;
        }
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
    // FIRST, before anything reads the viewport: resolve how wide it is from
    // the TV aspect the user configured in System Settings. AVMGetTVAspectRatio
    // is documented as always returning TRUE, but it is checked anyway so a
    // failure falls back to widescreen (the overwhelmingly common case, and the
    // behaviour every earlier build had unconditionally) rather than to an
    // uninitialised reading of `aspect`. Both values are multiples of 4 tiles,
    // keeping the 32px attribute regions aligned -- see video::viewport_tx()'s
    // own comment in video.hpp.
    AVMTvAspectRatio aspect = AVM_TV_ASPECT_RATIO_16_9;
    if (!AVMGetTVAspectRatio(&aspect)) aspect = AVM_TV_ASPECT_RATIO_16_9;
    wiiu_world_tx = (aspect == AVM_TV_ASPECT_RATIO_4_3) ? 40 : 52;

    // The core owns VideoRAM/paletteRAM. video::vram_bytes() (video.hpp): both
    // viewports are wider than the NES's native 32, so this resolves to double
    // the NES-hardware minimum (4 pages/0x1000 bytes) either way -- but it is
    // still computed from wiiu_world_tx above, not assumed, which is why that
    // has to be set first.
    emu::InitMemory(video::vram_bytes());

    // The demo owns main() (the RESET macro), so SDL2main must not hijack it;
    // SDL_MAIN_HANDLED (above) + SDL_SetMainReady() tell SDL we did the startup.
    SDL_SetMainReady();
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO);

    window = SDL_CreateWindow(PROJECT_NAME,
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              WIN_W, WIN_H, SDL_WINDOW_SHOWN);
    renderer = SDL_CreateRenderer(window, -1,
                                  SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    // Streaming texture sized to this run's viewport; RenderCopy scales it to
    // the window. ARGB8888 matches the core's 0xAARRGGBB output one-to-one.
    frame = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING,
                              video::viewport_px(), VP_H);

    input_init();
}

void irq::post() {
    if (frame)    SDL_DestroyTexture(frame);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window)   SDL_DestroyWindow(window);
    SDL_Quit();
}
