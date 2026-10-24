#pragma once
#include "../../actor.hpp"
#include "../../types.hpp"

// Two-player mode requires a second controller port and enough RAM/ROM budget.
// GBA (TARGET_GBA), DS/DSi (TARGET_NDS), and 3DS (TARGET_CTR) are excluded:
// those targets have constrained resources and no second controller port in the
// standard handheld configuration.
#if !defined(TARGET_GBA) && !defined(TARGET_NDS) && !defined(TARGET_CTR)
#  define PLAYER2_SUPPORTED
#endif

namespace level {

// ---------------------------------------------------------------------------
// Player physics tuning constants (subpixel units; 8 subpx = 1 world pixel)
// ---------------------------------------------------------------------------

// Base values are NTSC (60Hz). Regional<>::ScaleUp() passes rates through
// unchanged on NTSC and scales by 1.2 (60/50) on PAL so real-time speed/
// gravity match across regions instead of just per-frame subpixel deltas.
// Regional<>::ScaleDown() (x5/6) does the inverse for a frame-count duration
// like kRunRetain, so it still spans the same real-time window on PAL. All
// constexpr, so this is compile-time only -- no runtime cost or ROM size
// change vs the plain literals they replace.

// Horizontal velocity caps (subpixel/frame).
constexpr i8  kMaxWalk     = demo::Regional<i8, 12>{}.ScaleUp();    // 1.5 px/frame
constexpr i8  kMaxRun      = demo::Regional<i8, 20>{}.ScaleUp();    // 2.5 px/frame
// Per-frame acceleration accumulated in 1/256-subpixel so the ramp is gradual.
constexpr i16 kWalkAccel   = demo::Regional<i16, 76>{}.ScaleUp();
constexpr i16 kRunAccel    = demo::Regional<i16, 112>{}.ScaleUp();
constexpr i16 kFriction    = demo::Regional<i16, 76>{}.ScaleUp();   // coast-down when grounded with no input
constexpr i16 kSkid        = demo::Regional<i16, 152>{}.ScaleUp();  // turnaround deceleration when grounded
constexpr i16 kAirAccel    = demo::Regional<i16, 76>{}.ScaleUp();   // limited control while airborne, no friction
constexpr u8  kRunRetain   = demo::Regional<u8, 10>{}.ScaleDown();  // frame-count duration, not a rate

// Vertical velocity (subpixel/frame) and gravity (1/256-subpixel/frame).
constexpr i8  kJumpInit    = demo::Regional<i8, 32>{}.ScaleUp();    // launch speed off the ground
constexpr i8  kRunJumpInit = demo::Regional<i8, 36>{}.ScaleUp();    // taller launch at running speed
constexpr i16 kRiseGravity = demo::Regional<i16, 256>{}.ScaleUp();  // ascending with A held -> floaty
constexpr i16 kFallGravity = demo::Regional<i16, 896>{}.ScaleUp();  // descending or A released -> heavier
constexpr i8  kMaxFall     = demo::Regional<i8, 36>{}.ScaleUp();    // 4.5 px/frame terminal

// ---------------------------------------------------------------------------

struct Player {
    Actor actor;
    i16 xForce;    // horizontal acceleration remainder (1/256-subpixel/frame)
    i16 yForce;    // gravity remainder (1/256-subpixel/frame)
    u8  runTimer;
    // Per-frame entry points for a player: input and the camera/scroll
    // coupling live here, then each calls the matching Actor method for the
    // half shared with every actor (switched on actorType, not a hook pointer).
    void Start();
    void Update();
};

}   // namespace level
