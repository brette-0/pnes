/**
 * @file input.cpp
 * @brief Controller polling for the native Wii U backend (SDL2 GameController).
 *
 * The wiiu portlib SDL2 exposes the GamePad (DRC), Pro Controllers and Wiimotes
 * through the standard SDL_GameController API, so this maps them onto the same
 * NES button bitmask the rest of the engine expects (see ::Buttons in
 * input.hpp) -- identical layout to the SDL3 desktop backend. Two players are
 * wired, assigned by DEVICE IDENTITY rather than enumeration order: the
 * GamePad is always player 1 and the lowest-numbered Wii Remote / Pro
 * Controller is player 2. See ::rebuild_pads for why identity is the right key
 * and enumeration index is not.
 *
 * A/B map to the face buttons (SDL's SOUTH/EAST), Back/Start to Select/Start,
 * and the D-pad to the four directions.
 *
 * The GamePad's touch screen fills ::input::click. It comes through SDL2's own
 * touch device rather than wut's VPAD: the wiiu SDL2 port's joystick driver is
 * already calling VPADRead/VPADGetTPCalibratedPoint and feeding the result to
 * SDL_SendTouch, so reading VPAD a second time here would be two consumers of
 * one sample stream. SDL_GameControllerUpdate() below is what runs that driver,
 * which is why the touch read sits after it.
 *
 * The GamePad mirrors the TV frame, so a normalized touch maps straight onto
 * the viewport with no panel offset -- unlike the dual-screen handhelds, where
 * ::video::panel_rect decides where the digitizer's panel sits.
 */
#include "internal.hpp"
#include <pnes/input.hpp>
#include <pnes/video.hpp>

#include <SDL2/SDL.h>

static SDL_GameController *pads[2] = { nullptr, nullptr };

/**
 * @brief Re-derives both port slots from the driver's device identities.
 *
 * The wiiu SDL2 joystick driver keeps a five-entry table mapping an SDL
 * device_index to a stable identity: 0 is the GamePad, 1..4 are Wii Remote
 * #1..#4 (a Remote, a Remote + Nunchuk, a Classic Controller or a Wii U Pro
 * Controller -- the driver carries a separate button map per extension). That
 * identity is what SDL_JoystickGetDevicePlayerIndex returns, and it is the only
 * reliable key: device_index is merely a position in the registration list, so
 * "slot 0" means "whoever registered first", not "the GamePad".
 *
 * Called on every add/remove rather than incrementally patched. The table holds
 * five entries at most, so a full rescan costs nothing and removes any need to
 * reason about which slot a vanished device left behind.
 */
static void rebuild_pads() {
    for (auto &pad : pads) {
        if (pad) {
            SDL_GameControllerClose(pad);
            pad = nullptr;
        }
    }

    int gamepad_dev = -1;   // device_index holding identity 0
    int wpad_dev    = -1;   // device_index of the lowest-numbered Remote
    int wpad_id     = 0;    // that Remote's identity, 0 = none found yet

    const int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) {
        if (!SDL_IsGameController(i)) continue;

        const int id = SDL_JoystickGetDevicePlayerIndex(i);
        if (id == 0) {
            gamepad_dev = i;
        } else if (id >= 1 && id <= 4 && (wpad_id == 0 || id < wpad_id)) {
            wpad_dev = i;
            wpad_id  = id;
        }
    }

    if (gamepad_dev >= 0) pads[0] = SDL_GameControllerOpen(gamepad_dev);
    if (wpad_dev    >= 0) pads[1] = SDL_GameControllerOpen(wpad_dev);

    // No GamePad -- asleep, flat, or out of range -- but a Remote is present.
    // Promote it, rather than leaving port 1 dead and the game unplayable on a
    // controller the player is actively holding.
    if (!pads[0] && pads[1]) {
        pads[0] = pads[1];
        pads[1] = nullptr;
    }
}

void input_init() {
    rebuild_pads();
}

void input_handle_event(const union SDL_Event *e) {
    // Joystick-level events, not SDL_CONTROLLERDEVICE*: the driver posts
    // SDL_PrivateJoystickAdded/Removed, and these are the events that are
    // guaranteed to arrive for every one of its devices.
    if (e->type == SDL_JOYDEVICEADDED || e->type == SDL_JOYDEVICEREMOVED) {
        rebuild_pads();
    }
}

static u8 read_pad(SDL_GameController *gc) {
    if (!gc) return 0;

    u8 state = 0;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A))          state |= input::A;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B))          state |= input::B;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK))       state |= input::SELECT;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START))      state |= input::START;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP))    state |= input::UP;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  state |= input::DOWN;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  state |= input::LEFT;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) state |= input::RIGHT;
    return state;
}

void input::PollControllers(u8 *port1, u8 *port2) {
    // Runs the driver's own device scan, which is what posts the add/remove
    // events input_handle_event acts on -- so a controller connected mid-game
    // is picked up on the next pumped frame (video.cpp's WaitForPresent).
    SDL_GameControllerUpdate();

    // Straight by slot now that the slots mean something: rebuild_pads has
    // already put the GamePad in pads[0] and the lowest Remote in pads[1].
    // An absent controller reads as idle, and a player who sets their pad down
    // for a moment no longer gets their port reassigned under them.
    *port1 = read_pad(pads[0]);
    *port2 = read_pad(pads[1]);

    // One finger, the first, on the first touch device -- the GamePad is the
    // only one the port registers, and a UI hit test has one point to spend
    // (see ::input::Click). Every step is guarded because a device with no
    // finger down reports zero fingers, and SDL_GetTouchFinger returns null
    // for an index it does not have.
    bool touching = false;
    vec2<u16> at{0, 0};
    if (SDL_GetNumTouchDevices() > 0) {
        const SDL_TouchID dev = SDL_GetTouchDevice(0);
        if (const SDL_Finger* finger = SDL_GetTouchFinger(dev, 0)) {
            touching = true;
            // SDL_Finger carries NORMALIZED coordinates (0..1), so the
            // viewport is the only scale needed -- and the clamp is for the
            // exact 1.0f a touch on the last column reports, which would
            // otherwise land one pixel past the viewport and inside no rect.
            const float fx = finger->x * static_cast<float>(video::viewport_px());
            const float fy = finger->y * static_cast<float>(video::viewport_py());
            const u32 x = fx < 0.0f ? 0u : static_cast<u32>(fx);
            const u32 y = fy < 0.0f ? 0u : static_cast<u32>(fy);
            at = {static_cast<u16>(x < video::viewport_px() ? x : video::viewport_px() - 1),
                  static_cast<u16>(y < video::viewport_py() ? y : video::viewport_py() - 1)};
        }
    }

    input::click.Set(at, touching);
}
