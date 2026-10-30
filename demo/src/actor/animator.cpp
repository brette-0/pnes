#include "animator.hpp"

#include "../banks.hpp"   // ACTORS
#include "../types.hpp"   // demo::Regional

#include "../../animations/gen/mary_standing.hpp"
#include "../../animations/gen/mary_walking.hpp"

namespace demo::actor {

// How much of a walk frame's hold is spent per tick of speed. The designer
// authored the walk at timer 8, but that is a hold in NES frames and this is a
// hold in |vx| ticks: at kMaxRun (20 subpx/frame) it works out to 4 frames of
// hold on NTSC, at kMaxWalk (12) to about 7, and at a crawl to a slideshow.
// Weakly, which is all that is wanted -- the legs keep up with the ground
// without anyone measuring stride length.
//
// ScaleDown, as a hold spanning a span of real time: PAL's 50 frames each last
// longer, so fewer of them cover the same span. Note that the speed it is spent
// at is itself ScaleUp'd (see kMaxWalk/kMaxRun in level/player.hpp), so the two
// scalings compound -- see the comment on the decrement in ::NextFrame.
static constexpr u8 kWalkCycleBudget = demo::Regional<u8, 80>{}.ScaleDown();

// Picks the player's frame from how fast it is going. Called from
// Player::Update once the movement has been committed, so moveForce holds this
// frame's velocity and @p grounded this frame's footing.
ACTORS void NextFrame(Actor* self, const bool grounded) {
    using walk = gen::mary_walking;

    if (!grounded) {
        // Airborne -- jumping or falling -- holds the first walk frame. There
        // is no jump pose authored, and a leg cycle driven by ground speed
        // means nothing with no ground under it. Checked before speed, so a
        // run off a ledge freezes rather than carrying on striding.
        self->animationFrame = walk::kTiles_frame_1;
        self->animationTimer = kWalkCycleBudget;
        return;
    }

    // moveForce.x is subpixels/frame; -128 has no positive counterpart in i8,
    // so widen before taking the magnitude.
    const i16 vx    = self->moveForce.x;
    const u8  speed = static_cast<u8>(vx < 0 ? -vx : vx);

    if (speed == 0) {
        // Standing is a single frame at timer 0 -- the animation stops on it,
        // so there is nothing to count and the budget is left spent.
        self->animationFrame = gen::mary_standing::Get();
        self->animationTimer = 0;
        return;
    }

    // Spend the budget at the rate the player is travelling, rather than one
    // tick a frame. Faster means the hold runs out sooner means the legs turn
    // over quicker.
    //
    // Both sides of this are region-scaled: the budget down by 5/6, the speed
    // spending it up by 6/5, so a PAL walk cycle turns over about 1.2x quicker
    // in real time than an NTSC one rather than matching it. Deliberate -- the
    // budget is scaled as the frame-count hold it reads as.
    if (self->animationTimer > speed) {
        self->animationTimer -= speed;
        return;
    }

    // Budget exhausted: step the cycle and reload. Overshoot is dropped rather
    // than carried, which costs a fraction of a frame of phase and saves
    // holding a remainder.
    self->animationTimer = kWalkCycleBudget;

    // The walk's frames are public, so the cycle is written out of their own
    // names -- no indexing into the private table and no arithmetic on a
    // pointer that might still be aimed at the standing pose. Anything that is
    // not already mid-walk (the last frame, or standing a moment ago) enters at
    // the first.
    if (self->animationFrame == walk::kTiles_frame_1)
        self->animationFrame = walk::kTiles_frame_2;
    else if (self->animationFrame == walk::kTiles_frame_2)
        self->animationFrame = walk::kTiles_frame_3;
    else
        self->animationFrame = walk::kTiles_frame_1;
}

} // namespace demo::actor
