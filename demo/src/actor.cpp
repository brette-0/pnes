#include "actor.hpp"
#include "modes/level.hpp"
#include "modes/level/levels.hpp"
#include "modes/level/collision_map.hpp"
#include "banks.hpp"   // ACTORS

static void TranslateWorldSpace(vec2<u16>& worldSpace, vec2<i8> delta);

// ACTORS: Actor is global-namespace, so without this it would fall into the
// resident bank by default instead of the actor bank. TranslateWorldSpace
// stays untagged -- static/file-local, follows whichever section Move lands in.
ACTORS void Actor::Move(const vec2<i8> delta /* sub px */) {
    TranslateWorldSpace(worldSpace, delta);

    const bool inField = !(worldSpace.y & 0x8000)
                       && (worldSpace.y >> 7) < static_cast<u16>(level::levelHeight);

    if (inField) {
        // Block test against the shared composite-metatile window. Enemies pass
        // collectBlocks=false: coins are walked through, only walls stop them.
        // World-pixel inputs; the HUD strip is removed internally.
        const bool solid = level::Blocked(worldSpace.x >> 3, worldSpace.y >> 3,
                                                GetSize(), /*collectBlocks=*/false);
        (void)solid;   // TODO: collision response
    }
    // Off-field (above the screen): no collision runs this frame.
}

static void TranslateWorldSpace(vec2<u16>& worldSpace, const vec2<i8> delta) {
    // delta is in sub-pixels; positions are sub-pixels too, so add directly.
    if (delta.x > 0) {
        if ((worldSpace.x + delta.x < worldSpace.x) || ((worldSpace.x + delta.x) >> 7 >= level::nColumns)) {
            worldSpace.x = (level::nColumns - 1) << 7;
        } else {
            worldSpace.x += delta.x;
        }
    } else if (delta.x < 0) {
        if (worldSpace.x + delta.x > worldSpace.x) {   // underflow past column 0
            worldSpace.x = 0;
        } else {
            worldSpace.x += delta.x;
        }
    }

    worldSpace.y += delta.y;
}

ACTORS void Actor::Start() {
    switch (this->actorType) {
        case eActorType::Player:
            break;
        case eActorType::Mushlet:
            break;
        case eActorType::End:
            break;
    }
}
ACTORS void Actor::Update() {
    switch (this->actorType) {
        case eActorType::Player:
            break;
        case eActorType::Mushlet:
            break;
        case eActorType::End:
            break;
    }
}

// NOT ::ACTORS: called from EnterLevelSetup (level.cpp), which is ::COLD --
// COLD code can only reach FIXED, resident ($C000-$DFFF), and its own bank,
// not a different bank-5-window-1 domain like actors. Left in the default
// (resident) placement, which both COLD and ACTORS can already reach.
oam::oam_t AdjustSpriteY(const Actor* self, const u16) {
    return static_cast<oam::oam_t>(self->screen.y);
}

oam::oam_t AdjustSpriteX(const Actor* self, const u16 i) {
    return static_cast<oam::oam_t>(self->screen.x + (i & 1) * 8);
}