#pragma once
#include <pnes>
#include "modes/level/types.hpp"

class Actor {
    public:
    enum class eActorType {
        Player,
        Mushlet,

        End
    };

    struct ActorTypeMetadata_t {
        vec2<u8> collisionShape;
    };

    static constexpr ActorTypeMetadata_t ActorTypeMetadata[static_cast<size_t>(eActorType::End)] = {
        { {16, 16} }, // player
        { {16, 16} }, // mushlet
    };

    enum eActorState {
        Normal,     // default for that object (alive, moving whatever)
    };

    // Collision is now a shared composite-metatile window read (see collision_map.hpp
    // / Blocked), so the actor no longer carries any per-actor collision cursors.  The
    // old surround constellation (four static column cursors + a dynamic anchor) lived
    // in zero page on every actor and is gone.  Every caller passes
    // collectBlocks=false, so coins pass for actors and player alike -- see
    // collision_map.hpp on why that parameter is vestigial.
    vec2<u16> worldSpace;          // top-left origin, sub-pixel: bottom 3 bits = subpx, so >>3 = px
    vec2<oam::oam_t> screen;       // on-screen sprite position (OAM coords)
    vec2<i8> moveForce;            // subpixel movement force (8 subpx = 1px)
    i8 gravity;                    // accumulated downward accel (subpx/frame); 0 when grounded
    eActorType actorType;
    eActorState actorState;
    u8 animationFrame;

    static constexpr vec2<u8> GetSize(const eActorType actorType) {
        return actorType < eActorType::End
            ? ActorTypeMetadata[static_cast<size_t>(actorType)].collisionShape
            : vec2<u8>{16, 16};
    }

    [[nodiscard]] constexpr vec2<u8> GetSize() const {
        return GetSize(this->actorType);
    }

    // Behavior is dispatched on actorType (see Actor::Start/Update in
    // actor.cpp), not through per-actor function pointers: no 2 bytes of hook
    // storage per actor and no indirect JSR per call.
    void Start();
    void Update();

    void Move(vec2<i8> delta);
};

// Metasprite field providers: derive the sprite coordinate from the actor's
// location.  `i` is the metasprite cell index (2 wide x 4 tall: col = i&1, row = i>>1).
oam::oam_t AdjustSpriteY(const Actor* self, u16 i);
oam::oam_t AdjustSpriteX(const Actor* self, u16 i);

// the cursors for an NPC can be set on spawn from their metadata
// the cursors can be moved by 1/-1 when moving up/down
// the cursors can be moved by levelSize/-levelSize when moving forward/backward
// the cursors can be reset