/**
 * @file input.cpp
 * @brief Controller polling for the Nintendo Switch backend (libnx hid/pad).
 *
 * Maps the Switch controller onto the same NES button bitmask the rest of the
 * engine expects (see ::Buttons in input.hpp). A single player is wired for now,
 * so port 2 is always idle. Both the D-pad and the left analog stick drive the
 * NES directions (either works); A/B map to the Switch face buttons, with
 * Plus/Minus as Start/Select.
 *
 * The touch screen fills ::input::click. Unlike the dual-screen handhelds the
 * Switch has one panel and it IS the viewport, so the conversion is a scale
 * rather than an offset: the digitizer always reports in the handheld panel's
 * own 1280x720 regardless of what the console is scanning out, and
 * ::video::viewport_px()/py() is what a game's UI is laid out in.
 *
 * Nothing here tests for docked mode. Docked, there is no panel to touch and
 * no touches arrive, which is already indistinguishable from a finger that is
 * not touching -- so the absent case needs no code of its own.
 */
#include "internal.hpp"
#include <pnes/input.hpp>
#include <pnes/video.hpp>

#include <switch.h>

static PadState pad;

void input_init() {
    // One player, standard (handheld + any single attached controller).
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    // Separate from the pad: libnx treats the digitizer as its own HID device,
    // and hidGetTouchScreenStates() reads nothing until this has run.
    hidInitializeTouchScreen();
}

void input::PollControllers(u8 *port1, u8 *port2) {
    padUpdate(&pad);
    const u64 held = padGetButtons(&pad);

    u8 state = 0;
    if (held & HidNpadButton_A)                              state |= A;
    if (held & HidNpadButton_B)                              state |= B;
    if (held & HidNpadButton_Minus)                          state |= SELECT;
    if (held & HidNpadButton_Plus)                           state |= START;
    if (held & (HidNpadButton_Up    | HidNpadButton_StickLUp))    state |= UP;
    if (held & (HidNpadButton_Down  | HidNpadButton_StickLDown))  state |= DOWN;
    if (held & (HidNpadButton_Left  | HidNpadButton_StickLLeft))  state |= LEFT;
    if (held & (HidNpadButton_Right | HidNpadButton_StickLRight)) state |= RIGHT;

    *port1 = state;
    *port2 = 0;

    // The physical handheld panel, which is the space the digitizer reports in
    // whatever the console is scanning out to. Not a video:: value: it is a
    // property of the touch hardware, not of the frame being rendered, and the
    // two differ the moment the Switch is docked.
    constexpr u32 kTouchPanelW = 1280;
    constexpr u32 kTouchPanelH = 720;

    // One finger, the first. A UI hit test has one point to spend, and the
    // extra touches a multi-finger gesture would need are not something the
    // engine models (see ::input::Click).
    HidTouchScreenState touch = {};
    const bool touching = hidGetTouchScreenStates(&touch, 1) && touch.count > 0;

    vec2<u16> at{0, 0};
    if (touching) {
        // Scaled, then clamped to the last pixel rather than the width: a
        // touch on the panel's final column maps to viewport_px() exactly,
        // which is one past the last pixel and outside every rect a hit test
        // could match.
        const u32 x = static_cast<u32>(touch.touches[0].x) * video::viewport_px() / kTouchPanelW;
        const u32 y = static_cast<u32>(touch.touches[0].y) * video::viewport_py() / kTouchPanelH;
        at = {static_cast<u16>(x < video::viewport_px() ? x : video::viewport_px() - 1),
              static_cast<u16>(y < video::viewport_py() ? y : video::viewport_py() - 1)};
    }

    input::click.Set(at, touching);
}
