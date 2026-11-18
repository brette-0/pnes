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

// Base values are for the 60Hz systems. Framerated<>::ScaleUp() passes rates
// through unchanged at 60Hz and scales by 1.2 (60/50) at 50Hz so real-time
// speed/gravity match across both instead of just per-frame subpixel deltas.
// Framerated<>::ScaleDown() (x5/6) does the inverse for a frame-count duration
// like RunRetain(), so it still spans the same real-time window at 50Hz.
//
// FUNCTIONS, NOT `constexpr` CONSTANTS, AND THAT IS THE WHOLE POINT. The
// refresh is measured at runtime (video::Is50Hz(), see demo/src/types.hpp for
// why), and a namespace-scope constant is initialised before main -- which off
// NES is before irq::init() has measured anything, so a constant would latch
// the pre-detection default. An accessor reads the flag at the moment of use
// instead, which is always after detection. Both candidate values are still
// compile-time constants inside Framerated, so the per-call cost is an indexed
// load rather than any arithmetic.
//
// Dropping the `k` prefix along with the `constexpr` is deliberate: these are
// no longer constants, and a missed call site is then a hard compile error
// (a function name where an integer is wanted) rather than something subtle.

// Horizontal velocity caps (subpixel/frame).
AI inline i8  MaxWalk()     { return demo::Framerated<i8, 12>::ScaleUp(); }    // 1.5 px/frame
AI inline i8  MaxRun()      { return demo::Framerated<i8, 20>::ScaleUp(); }    // 2.5 px/frame
// Per-frame acceleration accumulated in 1/256-subpixel so the ramp is gradual.
AI inline i16 WalkAccel()   { return demo::Framerated<i16, 76>::ScaleUp(); }
AI inline i16 RunAccel()    { return demo::Framerated<i16, 112>::ScaleUp(); }
AI inline i16 Friction()    { return demo::Framerated<i16, 76>::ScaleUp(); }   // coast-down when grounded with no input
AI inline i16 Skid()        { return demo::Framerated<i16, 152>::ScaleUp(); }  // turnaround deceleration when grounded
AI inline i16 AirAccel()    { return demo::Framerated<i16, 76>::ScaleUp(); }   // limited control while airborne, no friction
AI inline u8  RunRetain()   { return demo::Framerated<u8, 10>::ScaleDown(); }  // frame-count duration, not a rate

// Vertical velocity (subpixel/frame) and gravity (1/256-subpixel/frame).
AI inline i8  JumpInit()    { return demo::Framerated<i8, 32>::ScaleUp(); }    // launch speed off the ground
AI inline i8  RunJumpInit() { return demo::Framerated<i8, 36>::ScaleUp(); }    // taller launch at running speed
AI inline i16 RiseGravity() { return demo::Framerated<i16, 256>::ScaleUp(); }  // ascending with A held -> floaty
AI inline i16 FallGravity() { return demo::Framerated<i16, 896>::ScaleUp(); }  // descending or A released -> heavier
AI inline i8  MaxFall()     { return demo::Framerated<i8, 36>::ScaleUp(); }    // 4.5 px/frame terminal

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
