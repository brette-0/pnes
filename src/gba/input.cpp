/**
 * @file input.cpp
 * @brief Controller polling for the Game Boy Advance backend (libgba keypad).
 *
 * Maps the GBA buttons onto the same NES button bitmask the rest of the engine
 * expects (see ::Buttons in input.hpp). The GBA is a single-player console, so
 * port 2 is always idle. A/B map straight to the GBA A/B face buttons; the
 * GBA's Start/Select map straight through. The GBA's two shoulder buttons (L/R)
 * are mirrored onto Select/Start too so they are reachable without the tiny
 * centre buttons, matching the DS backend's X/Y mirroring.
 */
#include "internal.hpp"
#include <pnes/input.hpp>

#include <gba.h>

void input::PollControllers(u8* port1, u8* port2) {
    scanKeys();
    const u16 held = keysHeld();

    u8 state = 0;
    if (held & KEY_A)                state |= A;
    if (held & KEY_B)                state |= B;
    if (held & (KEY_SELECT | KEY_L)) state |= SELECT;
    if (held & (KEY_START  | KEY_R)) state |= START;
    if (held & KEY_UP)               state |= UP;
    if (held & KEY_DOWN)             state |= DOWN;
    if (held & KEY_LEFT)             state |= LEFT;
    if (held & KEY_RIGHT)            state |= RIGHT;

    *port1 = state;
    *port2 = 0;
}
