/**
 * @file input.cpp
 * @brief Controller polling for the GameCube + Wii backend.
 *
 * Maps the two native input stacks onto the same NES button bitmask the rest of
 * the engine expects (see ::Buttons in input.hpp):
 *
 * - **GameCube** (always, on both consoles): the PAD library. The GC pad has no
 *   Select button, so the Z trigger stands in for it.
 * - **Wii** (TARGET_WII): the WPAD library. Two grips are supported, and which
 *   one applies is taken from WPAD_Probe's extension type every frame -- the
 *   grip is what decides which way the D-pad points, so it cannot be a
 *   compile-time choice:
 *   - **bare Remote**, held SIDEWAYS like an NES pad: the physical D-pad is
 *     rotated 90 degrees (pointing right is "up"), 2 is A and 1 is B.
 *   - **Remote + Nunchuk**, held UPRIGHT in two hands: the D-pad keeps its real
 *     orientation, the Nunchuk stick drives the directions as well, and A/B are
 *     the Remote's own A and B trigger (with the Nunchuk's Z/C as alternates,
 *     so the hand already holding it can play without reaching across).
 *   Either way +/- are Start/Select, and the result is OR'd onto the matching GC
 *   port so either kind of controller works.
 */
#include "internal.hpp"
#include <pnes/input.hpp>

#include <ogc/pad.h>
#ifdef TARGET_WII
#include <wiiuse/wpad.h>
#endif

#ifdef TARGET_WII
/**
 * @brief Deadzone for the Nunchuk stick, in raw stick units.
 *
 * The same magnitude the PSP and PS3 sticks use (src/psp/input.cpp,
 * src/ps3/input.cpp). There is no STICK_CENTER companion here: the Nunchuk
 * reports its own calibrated centre in the extension's calibration block
 * (joystick_t::center, wiiuse.h), so the centre is read per device rather than
 * assumed to be 128.
 */
static constexpr int STICK_DEADZONE = 40;
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
/// A bare Remote held sideways: the D-pad is rotated a quarter turn clockwise.
static u8 read_wpad_sideways(const u32 held) {
    u8 state = 0;
    if (held & WPAD_BUTTON_2)     state |= input::A;
    if (held & WPAD_BUTTON_1)     state |= input::B;
    if (held & WPAD_BUTTON_MINUS) state |= input::SELECT;
    if (held & WPAD_BUTTON_PLUS)  state |= input::START;
    if (held & WPAD_BUTTON_RIGHT) state |= input::UP;
    if (held & WPAD_BUTTON_LEFT)  state |= input::DOWN;
    if (held & WPAD_BUTTON_UP)    state |= input::LEFT;
    if (held & WPAD_BUTTON_DOWN)  state |= input::RIGHT;
    return state;
}

/**
 * @brief Remote + Nunchuk, held upright in two hands.
 *
 * The D-pad is NOT rotated here -- that rotation only makes sense for the
 * sideways grip, and a Nunchuk forces the Remote upright. Z and C double for A
 * and B rather than adding anything new, which is the same "either input works"
 * stance the Switch/3DS/PSP backends take with D-pad plus stick.
 */
static u8 read_wpad_nunchuk(const int chan, const u32 held) {
    u8 state = 0;
    if (held & (WPAD_BUTTON_A | WPAD_NUNCHUK_BUTTON_Z)) state |= input::A;
    if (held & (WPAD_BUTTON_B | WPAD_NUNCHUK_BUTTON_C)) state |= input::B;
    if (held & WPAD_BUTTON_MINUS) state |= input::SELECT;
    if (held & WPAD_BUTTON_PLUS)  state |= input::START;
    if (held & WPAD_BUTTON_UP)    state |= input::UP;
    if (held & WPAD_BUTTON_DOWN)  state |= input::DOWN;
    if (held & WPAD_BUTTON_LEFT)  state |= input::LEFT;
    if (held & WPAD_BUTTON_RIGHT) state |= input::RIGHT;

    // The stick, OR'd on top of the D-pad so either drives the directions.
    //
    // No WPAD_SetDataFormat call is needed to reach this data: wiiuse re-runs
    // wiiuse_set_report_type when an expansion completes its handshake, so the
    // extension bytes are already in the report by the time WPAD_Probe admits
    // the Nunchuk is there at all.
    expansion_t exp;
    WPAD_Expansion(chan, &exp);
    if (exp.type == WPAD_EXP_NUNCHUK) {
        const joystick_t& js = exp.nunchuk.js;
        const int dx = static_cast<int>(js.pos.x) - static_cast<int>(js.center.x);
        const int dy = static_cast<int>(js.pos.y) - static_cast<int>(js.center.y);
        if (dx < -STICK_DEADZONE) state |= input::LEFT;
        if (dx >  STICK_DEADZONE) state |= input::RIGHT;
        // Y grows UPWARD on the Nunchuk, the opposite of the PSP/PS3 sticks --
        // which is why these two comparisons are mirrored from those files.
        if (dy >  STICK_DEADZONE) state |= input::UP;
        if (dy < -STICK_DEADZONE) state |= input::DOWN;
    }
    return state;
}

static u8 read_wpad(const int chan) {
    // Probed every frame, not cached: a Nunchuk can be pulled off or pushed on
    // mid-game, and the grip -- hence the whole D-pad orientation -- has to
    // follow the hardware rather than whatever was true at startup.
    u32 type = WPAD_EXP_NONE;
    if (WPAD_Probe(chan, &type) != WPAD_ERR_NONE) return 0;

    // One read covers the Remote and whatever is plugged into it: libogc packs
    // extension buttons into the UPPER 16 bits of the same word
    // (WPAD_NUNCHUK_BUTTON_Z is 0x0001 << 16, wpad.h).
    const u32 held = WPAD_ButtonsHeld(chan);

    if (type == WPAD_EXP_NUNCHUK) return read_wpad_nunchuk(chan, held);
    return read_wpad_sideways(held);
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
