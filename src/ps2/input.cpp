/**
 * @file input.cpp
 * @brief Controller polling for the Sony PlayStation 2 backend (libpad).
 *
 * Maps the DualShock's face buttons/D-pad onto the same NES button bitmask
 * the rest of the engine expects (see ::Buttons in input.hpp). A single
 * player is wired for now, so port 2 (the second physical controller port)
 * is always idle; Cross/Circle map to the NES A/B face buttons (Cross as
 * the "confirm" button matches every PS2 menu convention), with Start/
 * Select as Start/Select.
 *
 * SIO2MAN and PADMAN are BIOS-resident modules (unlike audsrv, see
 * src/ps2/audio.cpp) -- SifLoadModule("rom0:...") loads them straight out of
 * the console's own ROM, with no IRX to embed.
 *
 * libpad's padButtonStatus::btns is active-LOW hardware convention (a
 * pressed button reads as a CLEARED bit) -- every ps2sdk pad sample XORs
 * against 0xffff to get an active-HIGH mask before testing it, so this does
 * the same.
 */
#include "internal.hpp"
#include <platform-nes/input.hpp>

#include <sifrpc.h>
#include <loadfile.h>
#include <libpad.h>

static char padBuf[256] __attribute__((aligned(64)));

void input_init() {
    SifLoadModule("rom0:SIO2MAN", 0, nullptr);
    SifLoadModule("rom0:PADMAN", 0, nullptr);

    padInit(0);
    padPortOpen(0, 0, padBuf);
}

void input::PollControllers(u8 *port1, u8 *port2) {
    struct padButtonStatus pad;

    u8 state = 0;
    if (padGetState(0, 0) == PAD_STATE_STABLE && padRead(0, 0, &pad) != 0) {
        const u16 buttons = 0xffffu ^ pad.btns;

        if (buttons & PAD_CROSS)  state |= A;
        if (buttons & PAD_CIRCLE) state |= B;
        if (buttons & PAD_SELECT) state |= SELECT;
        if (buttons & PAD_START)  state |= START;
        if (buttons & PAD_UP)     state |= UP;
        if (buttons & PAD_DOWN)   state |= DOWN;
        if (buttons & PAD_LEFT)   state |= LEFT;
        if (buttons & PAD_RIGHT)  state |= RIGHT;
    }

    *port1 = state;
    *port2 = 0;
}
