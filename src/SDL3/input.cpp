// internal.hpp FIRST, which is the order every other backend's input.cpp
// already uses. It pulls in <atomic>, and ::atomic is a MACRO on every engine
// header's side (technology.hpp: `volatile` on NES, `_Atomic` elsewhere, and
// pnes/input.hpp and pnes/video.hpp both reach it) -- so a pnes header seen
// first turns libstdc++'s own `std::atomic` declarations into syntax errors.
#include "internal.hpp"

#include <SDL3/SDL.h>
#include <pnes/input.hpp>
#include <pnes/video.hpp>


static SDL_Gamepad *gamepads[2] = {nullptr};

static void open_gamepad_id(const SDL_JoystickID which) {
    for (auto & gamepad : gamepads) {
        if (!gamepad) {
            gamepad = SDL_OpenGamepad(which);
            if (gamepad) {
                SDL_Log("Controller Connected: %u (%s)",
                        static_cast<unsigned>(which), SDL_GetGamepadName(gamepad));
            } else {
                SDL_Log("SDL_OpenGamepad(%u) failed: %s",
                        static_cast<unsigned>(which), SDL_GetError());
            }
            return;
        }
    }
}

void input_init() {
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_INIT_GAMEPAD failed: %s", SDL_GetError());
        return;
    }

    auto count = 0;
    if (SDL_JoystickID *ids = SDL_GetGamepads(&count)) {
        for (auto i = 0; i < count; i++) open_gamepad_id(ids[i]);
        SDL_free(ids);
    }
}

void input_handle_event(const SDL_Event *e) {
    if (e->type == SDL_EVENT_GAMEPAD_ADDED) {
        for (auto & gamepad : gamepads) {
            if (gamepad &&
                SDL_GetGamepadID(gamepad) == e->gdevice.which) {
                return;
            }
        }
        open_gamepad_id(e->gdevice.which);
    }
    if (e->type == SDL_EVENT_GAMEPAD_REMOVED) {
        for (auto & gamepad : gamepads) {
            if (gamepad &&
                SDL_GetGamepadID(gamepad) == e->gdevice.which) {
                SDL_CloseGamepad(gamepad);
                gamepad = nullptr;
                break;
                }
        }
    }
}

static u8 read_gamepad(SDL_Gamepad *pad) {
    if (!pad) return 0;

    auto state = 0;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH))          state |= 0x01;  // A
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST))           state |= 0x02;  // B
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK))           state |= 0x04;  // Select
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START))          state |= 0x08;  // Start
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP))        state |= 0x10;  // Up
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))      state |= 0x20;  // Down
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))      state |= 0x40;  // Left
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))     state |= 0x80;  // Right

    return state;
}

void input::PollControllers(u8* port1, u8* port2) {
    *port1 = read_gamepad(gamepads[0]);
    *port2 = read_gamepad(gamepads[1]);

    // The mouse fills ::input::click. The window is the only scale involved:
    // present_frame() blits the whole viewport texture to the whole window
    // (SDL_RenderTexture with a null destination rect, video.cpp), so there is
    // no letterbox to subtract and window space maps to viewport space by a
    // plain ratio. Whenever that blit gains a destination rect, this is the
    // other half that has to learn about it.
    //
    // Position is read whether or not a button is down, unlike every touch
    // backend: a mouse always has one, and ::input::Click::Set keeps it so a
    // game can follow the pointer without a click (see ::input::Click::Get).
    float mx = 0.0f, my = 0.0f;
    const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mx, &my);

    int ww = 0, wh = 0;
    SDL_GetWindowSize(window, &ww, &wh);

    vec2<u16> at{0, 0};
    if (ww > 0 && wh > 0) {
        // Clamped at both ends: the pointer is reported outside the window
        // while a drag is in progress, and SDL hands back negative values for
        // it. Clamping to the last pixel rather than the width keeps the
        // result inside the viewport, which is where every rect a hit test
        // could match lives.
        const float sx = mx * static_cast<float>(video::viewport_px()) / static_cast<float>(ww);
        const float sy = my * static_cast<float>(video::viewport_py()) / static_cast<float>(wh);
        const u32 x = sx < 0.0f ? 0u : static_cast<u32>(sx);
        const u32 y = sy < 0.0f ? 0u : static_cast<u32>(sy);
        at = {static_cast<u16>(x < video::viewport_px() ? x : video::viewport_px() - 1),
              static_cast<u16>(y < video::viewport_py() ? y : video::viewport_py() - 1)};
    }

    input::click.Set(at, (buttons & SDL_BUTTON_LMASK) != 0);
}