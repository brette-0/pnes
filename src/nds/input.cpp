/**
 * @file input.cpp
 * @brief Controller polling for the Nintendo DS / DSi backend (libnds keypad).
 *
 * Maps the DS buttons onto the same NES button bitmask the rest of the engine
 * expects (see ::Buttons in input.hpp). The DS is a single-player console, so
 * port 2 is always idle. A/B map to the DS A/B face buttons; the DS's tiny
 * Start/Select map straight through, with X/Y mirrored onto Start/Select too so
 * the larger buttons also work.
 *
 * The touch screen fills ::input::click. The DS's two panels are identical
 * 256x192 screens and only the LOWER one is a digitizer, so a touch is always
 * on panel 1 -- and ::video::panel_rect(1) is where that panel sits inside the
 * viewport (video.hpp). Which makes the whole conversion into the viewport
 * pixels ::input::Click reports a single add per axis: libnds already hands
 * back panel pixels, and this panel is 1:1 with the viewport.
 */
#include "internal.hpp"
#include <pnes/input.hpp>
#include <pnes/video.hpp>

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

    // touchRead() reports whether the screen is being touched in its RETURN
    // value, so there is no KEY_TOUCH test here -- that bit and this call
    // would be two answers to one question. The position it writes is left
    // alone when nothing is down (::input::Click::Set ignores it then, keeping
    // the point the finger lifted from).
    touchPosition touch;
    const bool touching = touchRead(&touch);

    constexpr video::PanelRect bottom = video::panel_rect(1);
    input::click.Set({static_cast<u16>(touch.px + bottom.x),
                      static_cast<u16>(touch.py + bottom.y)}, touching);
}
