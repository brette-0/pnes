/**
 * @file video.cpp
 * @brief SDL3 presentation backend for the shared software PPU.
 *
 * The PPU emulation itself -- ::emu::GenerateFrame and every
 * nametable/attribute/palette/OAM write function plus the scroll and
 * sprite-zero machinery -- lives in src/emu/ppu.cpp and is shared verbatim with
 * the libogc/GX backend. This file is only the desktop-specific half: it opens
 * the window, paces frames off an SDL timer, locks a streaming texture, asks the
 * core to render one frame into it, and lets SDL's renderer scale it to the
 * window with nearest-neighbour filtering.
 */
#include "../SDL3/internal.hpp"

#include <pnes/video.hpp>
#include <pnes/interrupts.hpp>
#include "../emu/emu.hpp"

#include <cstdlib>
#include <atomic>
#include <SDL3/SDL.h>

// ---------------------------------------------------------------------------
// SDL-specific state. Everything the core needs (VideoRAM, paletteRAM, scroll,
// PPUCTRL/PPUMASK, the OAM snapshot, patternTable, ...) is owned by src/emu;
// only the genuine SDL objects live here.
// ---------------------------------------------------------------------------
SDL_Window   *window;
SDL_Renderer *renderer;
SDL_TimerID   timer_id;
std::atomic_int    _vblank_flag;
void        (*_nmi_callback)();
const SDL_DisplayMode *mode;
u8       scale;

static SDL_Texture *bgTexture;
static u64          last_frame;

u32 vblank_tick(void *userdata, SDL_TimerID id, const u32 interval) {
    _vblank_flag = 1;
    return interval;  // repeat every 16ms
}

static void toggle_fullscreen() {
    if (const u32 flags = SDL_GetWindowFlags(window); flags & SDL_WINDOW_FULLSCREEN) {
        SDL_SetWindowFullscreen(window, false); // back to windowed
    } else {
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN);
    }
}

// Render one frame through the shared core into the streaming texture, then
// blit it to the window. The core fills ARGB8888 pixels; SDL scales the
// fixed NES surface up with SCALEMODE_NEAREST, matching the GX backend's quad.
static void present_frame() {
    const int vpw = video::viewport_px();
    const int vph = video::viewport_py();

    if (!bgTexture) {
        bgTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STREAMING, vpw, vph);
        if (!bgTexture) return;
        SDL_SetTextureScaleMode(bgTexture, SDL_SCALEMODE_NEAREST);
    }

    void *raw; int pitch;
    if (!SDL_LockTexture(bgTexture, nullptr, &raw, &pitch)) return;
    emu::GenerateFrame(static_cast<u32 *>(raw), pitch / 4);
    SDL_UnlockTexture(bgTexture);

    SDL_RenderTexture(renderer, bgTexture, nullptr, nullptr);
}

namespace video {

void WaitForPresent() {
    // pump events once
    SDL_Event e;
    while (SDL_PollEvent(&e)) {

        switch (e.type) {

        case SDL_EVENT_QUIT:
            quit = 1;
            break;

        case SDL_EVENT_KEY_DOWN: {
                const SDL_Keycode key = e.key.key;

                if (const SDL_Keymod mod = SDL_GetModState(); key == SDLK_RETURN && (mod & SDL_KMOD_ALT)) {
                    toggle_fullscreen();
                }

                if (key == SDLK_F11) {
                    toggle_fullscreen();
                }

                input_handle_event(&e);
                break;
        }

        default:
            input_handle_event(&e);
            break;
        }
    }

    constexpr u64 target = 16666667;   // 60 Hz frame budget, ns
    const u64 elapsed = SDL_GetTicksNS() - last_frame;
    if (elapsed < target) {
        SDL_DelayPrecise(target - elapsed);
    }
    last_frame = SDL_GetTicksNS();

    if (ppu::PPUMASK & (ppu::mask::BG | ppu::mask::SPRITE)) {
        present_frame();
        SDL_RenderPresent(renderer);
    } else {
        SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);
    }
    /* Clear the pending IRQ slot between frames so a handler that was not
     * reached this frame (e.g. fired too late) does not re-fire next frame. */
    irq::irqPendingValid = false;
    nmi_vector();
}

}   // namespace video

void irq::init() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return;
    }

    const SDL_DisplayID display = SDL_GetPrimaryDisplay();
    mode = SDL_GetCurrentDisplayMode(display);

    // A tiny monitor crops the camera (scale/viewport_tx()/ty() can resolve
    // below the NES's own 32x30); video::vram_bytes() (video.hpp) never sizes
    // the nametable VRAM below the NES-hardware minimum regardless.
#ifdef LANDSCAPE
    scale = mode->h / 240;
#endif
#if PORTRAIT
    scale = mode->w / 256;
#endif
    if (scale < 1) scale = 1;
    const unsigned vram = video::vram_bytes();

    // The core owns VideoRAM/paletteRAM; the VRAM size policy is the backend's
    // (window-derived on SDL, fixed two pages on OGC), so size it here.
    emu::InitMemory(vram);

    if (!SDL_CreateWindowAndRenderer(PROJECT_NAME, mode->w >> 1, mode->h >> 1, 0, &window, &renderer)) {
        SDL_Log("Window creation failed: %s", SDL_GetError());
        return;
    }
    timer_id = SDL_AddTimer(16, vblank_tick, nullptr);
}

void irq::post() {
    SDL_RemoveTimer(timer_id);
    if (bgTexture) SDL_DestroyTexture(bgTexture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
