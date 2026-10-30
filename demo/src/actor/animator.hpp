#pragma once

#include "../actor.hpp"

namespace demo::actor {
    /// Picks @p actor's animation frame for this tick. @p grounded is the
    /// caller's own footing test -- the Actor carries no usable one (::gravity
    /// is written once at Start and never maintained, and moveForce.y is
    /// non-zero even at rest once gravity has ticked), so whoever ran the
    /// movement passes what it already worked out.
    void NextFrame(Actor* actor, bool grounded);
    void SwitchAnimation(u8* animTable);
}