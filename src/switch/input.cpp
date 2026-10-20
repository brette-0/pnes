/**
 * @file input.cpp
 * @brief Controller polling for the Nintendo Switch backend (libnx hid/pad).
 *
 * Maps the Switch controller onto the same NES button bitmask the rest of the
 * engine expects (see ::Buttons in input.hpp). A single player is wired for now,
 * so port 2 is always idle. Both the D-pad and the left analog stick drive the
 * NES directions (either works); A/B map to the Switch face buttons, with
 * Plus/Minus as Start/Select.
 */
#include "internal.hpp"
#include <pnes/input.hpp>

#include <switch.h>

static PadState pad;

void input_init() {
    // One player, standard (handheld + any single attached controller).
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
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
}
