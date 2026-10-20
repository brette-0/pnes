/**
 * @file input.cpp
 * @brief Controller polling for the Nintendo 3DS backend (libctru hid).
 *
 * Maps the 3DS buttons onto the same NES button bitmask the rest of the engine
 * expects (see ::Buttons in input.hpp). The 3DS has a single player, so port 2
 * is always idle. Both the D-pad and the Circle Pad drive the NES directions so
 * either control works; A/B map to the face buttons, with X/Y as Start/Select
 * (the 3DS Start/Select are tiny, so the larger face buttons stand in -- the
 * physical Start still works too).
 */
#include "internal.hpp"
#include <pnes/input.hpp>

#include <3ds.h>

void input::PollControllers(u8* port1, u8* port2) {
    hidScanInput();
    const u32 held = hidKeysHeld();

    u8 state = 0;
    if (held & KEY_A)                       state |= A;
    if (held & KEY_B)                       state |= B;
    if (held & (KEY_SELECT | KEY_Y))        state |= SELECT;
    if (held & (KEY_START  | KEY_X))        state |= START;
    if (held & (KEY_DUP    | KEY_CPAD_UP))    state |= UP;
    if (held & (KEY_DDOWN  | KEY_CPAD_DOWN))  state |= DOWN;
    if (held & (KEY_DLEFT  | KEY_CPAD_LEFT))  state |= LEFT;
    if (held & (KEY_DRIGHT | KEY_CPAD_RIGHT)) state |= RIGHT;

    *port1 = state;
    *port2 = 0;
}
