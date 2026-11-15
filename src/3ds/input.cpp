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
 *
 * The touch screen fills ::input::click. Only the 3DS's LOWER panel is a
 * digitizer, so a touch is always on panel 1, and ::video::panel_rect(1)
 * (video.hpp) is where that panel sits inside the viewport: 320x240 inset 40px
 * from the left of the 400px-wide bounding box, 240px down. The panel is 1:1
 * with viewport pixels, so applying that rect is the whole conversion -- which
 * is also the only place the inset appears, exactly as video.hpp intends.
 */
#include "internal.hpp"
#include <pnes/input.hpp>
#include <pnes/video.hpp>

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

    // KEY_TOUCH is libctru's synthesized "screen is being touched" bit (it is
    // not a real HID button), and it is the only thing that says so here --
    // hidTouchRead() returns void and leaves a stale position behind when
    // nothing is down. ::input::Click::Set ignores the position in that case,
    // so the stale value never reaches a caller.
    touchPosition touch;
    hidTouchRead(&touch);

    constexpr video::PanelRect bottom = video::panel_rect(1);
    input::click.Set({static_cast<u16>(touch.px + bottom.x),
                      static_cast<u16>(touch.py + bottom.y)},
                     (held & KEY_TOUCH) != 0);
}
