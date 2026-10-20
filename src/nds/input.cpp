/**
 * @file input.cpp
 * @brief Controller polling for the Nintendo DS / DSi backend (libnds keypad).
 *
 * Maps the DS buttons onto the same NES button bitmask the rest of the engine
 * expects (see ::Buttons in input.hpp). The DS is a single-player console, so
 * port 2 is always idle. A/B map to the DS A/B face buttons; the DS's tiny
 * Start/Select map straight through, with X/Y mirrored onto Start/Select too so
 * the larger buttons also work. The touch screen is intentionally left off for
 * this first pass.
 */
#include "internal.hpp"
#include <pnes/input.hpp>

#include <nds.h>

void input::PollControllers(u8* port1, u8* port2) {
    scanKeys();
    const u32 held = keysHeld();

    u8 state = 0;
    if (held & KEY_A)                 state |= A;
    if (held & KEY_B)                 state |= B;
    if (held & (KEY_SELECT | KEY_Y))  state |= SELECT;
    if (held & (KEY_START  | KEY_X))  state |= START;
    if (held & KEY_UP)                state |= UP;
    if (held & KEY_DOWN)              state |= DOWN;
    if (held & KEY_LEFT)              state |= LEFT;
    if (held & KEY_RIGHT)             state |= RIGHT;

    *port1 = state;
    *port2 = 0;
}
