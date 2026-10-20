/**
 * @file input.cpp
 * @brief Controller polling for the GameCube + Wii backend.
 *
 * Maps the two native input stacks onto the same NES button bitmask the rest of
 * the engine expects (see ::Buttons in input.hpp):
 *
 * - **GameCube** (always, on both consoles): the PAD library. The GC pad has no
 *   Select button, so the Z trigger stands in for it.
 * - **Wii** (TARGET_WII): the WPAD library, with the Wii Remote held sideways
 *   like an NES pad -- so the physical D-pad is rotated 90 degrees (pointing
 *   right is "up"), the 2 button is A and 1 is B, and +/- are Start/Select.
 *   Wii state is OR'd onto the matching GC port so either controller works.
 */
#include "internal.hpp"
#include <pnes/input.hpp>

#include <ogc/pad.h>
#ifdef TARGET_WII
#include <wiiuse/wpad.h>
#endif

void ogc_input_init() {
    PAD_Init();
#ifdef TARGET_WII
    WPAD_Init();
#endif
}

static u8 read_pad(const int chan) {
    const u32 held = PAD_ButtonsHeld(chan);
    u8 state = 0;
    if (held & PAD_BUTTON_A)     state |= input::A;
    if (held & PAD_BUTTON_B)     state |= input::B;
    if (held & PAD_TRIGGER_Z)    state |= input::SELECT;  // GC pad has no Select
    if (held & PAD_BUTTON_START) state |= input::START;
    if (held & PAD_BUTTON_UP)    state |= input::UP;
    if (held & PAD_BUTTON_DOWN)  state |= input::DOWN;
    if (held & PAD_BUTTON_LEFT)  state |= input::LEFT;
    if (held & PAD_BUTTON_RIGHT) state |= input::RIGHT;
    return state;
}

#ifdef TARGET_WII
static u8 read_wpad(const int chan) {
    const u32 held = WPAD_ButtonsHeld(chan);
    u8 state = 0;
    if (held & WPAD_BUTTON_2)     state |= input::A;
    if (held & WPAD_BUTTON_1)     state |= input::B;
    if (held & WPAD_BUTTON_MINUS) state |= input::SELECT;
    if (held & WPAD_BUTTON_PLUS)  state |= input::START;
    // Remote held sideways: the D-pad is rotated a quarter turn clockwise.
    if (held & WPAD_BUTTON_RIGHT) state |= input::UP;
    if (held & WPAD_BUTTON_LEFT)  state |= input::DOWN;
    if (held & WPAD_BUTTON_UP)    state |= input::LEFT;
    if (held & WPAD_BUTTON_DOWN)  state |= input::RIGHT;
    return state;
}
#endif

void input::PollControllers(u8* port1, u8* port2) {
    PAD_ScanPads();
    u8 p1 = read_pad(0);
    u8 p2 = read_pad(1);

#ifdef TARGET_WII
    WPAD_ScanPads();
    p1 |= read_wpad(WPAD_CHAN_0);
    p2 |= read_wpad(WPAD_CHAN_1);
#endif

    *port1 = p1;
    *port2 = p2;
}
