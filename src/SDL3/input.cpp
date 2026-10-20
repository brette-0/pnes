#include <SDL3/SDL.h>
#include <pnes/input.hpp>
#include "internal.hpp"


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
}