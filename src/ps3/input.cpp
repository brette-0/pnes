/**
 * @file input.cpp
 * @brief Controller polling for the Sony PlayStation 3 backend (PSL1GHT libio).
 *
 * Maps the DualShock 3's face buttons/D-pad/left stick onto the same NES
 * button bitmask the rest of the engine expects (see ::Buttons in input.hpp).
 * A single player is wired for now, so port 2 is always idle -- same as the
 * PSP and PS2 backends. Both the D-pad and the left stick drive the NES
 * directions (either works); Cross/Circle map to the NES A/B face buttons
 * (Cross as "confirm" matches the XMB's own convention on a PAL/NTSC-U
 * console), with Start/Select as Start/Select.
 */
#include "internal.hpp"
#include <platform-nes/input.hpp>

#include <io/pad.h>

// PSL1GHT reports the analog nubs as 0x00-0xFF with 0x80 nominal centre. The
// deadzone matches the loose ~25% threshold the other digital-stick-fallback
// backends use.
static constexpr int STICK_CENTER = 128;
static constexpr int STICK_DEADZONE = 40;

// ioPadInit's argument is how many pads the port scan should track, not how
// many this app reads. 7 is PSL1GHT's own conventional maximum.
static constexpr u32 MAX_TRACKED_PADS = 7;

void input_init() {
    ioPadInit(MAX_TRACKED_PADS);
}

void input_exit() {
    ioPadEnd();
}

void input::PollControllers(u8 *port1, u8 *port2) {
    *port1 = 0;
    *port2 = 0;

    // Unlike the PSP's single built-in pad, a PS3 pad can genuinely be absent
    // or asleep, so the connection state is checked rather than assumed --
    // ioPadGetData on a disconnected port returns stale/garbage button bits.
    padInfo info;
    if (ioPadGetInfo(&info) != 0) return;
    if (info.connected == 0 || info.status[0] == 0) return;

    padData pad;
    if (ioPadGetData(0, &pad) != 0) return;

    u8 state = 0;
    if (pad.BTN_CROSS)    state |= A;
    if (pad.BTN_CIRCLE)   state |= B;
    if (pad.BTN_SELECT)   state |= SELECT;
    if (pad.BTN_START)    state |= START;

    if (pad.BTN_UP)       state |= UP;
    if (pad.BTN_DOWN)     state |= DOWN;
    if (pad.BTN_LEFT)     state |= LEFT;
    if (pad.BTN_RIGHT)    state |= RIGHT;

    const int dx = static_cast<int>(pad.ANA_L_H) - STICK_CENTER;
    const int dy = static_cast<int>(pad.ANA_L_V) - STICK_CENTER;
    if (dy < -STICK_DEADZONE) state |= UP;
    if (dy >  STICK_DEADZONE) state |= DOWN;
    if (dx < -STICK_DEADZONE) state |= LEFT;
    if (dx >  STICK_DEADZONE) state |= RIGHT;

    *port1 = state;
}
