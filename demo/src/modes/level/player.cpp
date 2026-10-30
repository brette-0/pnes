#include "player.hpp"

#include "../../technology.hpp"
#include "../../graphics/metatiles.hpp"
#include "collision_map.hpp"
#include "levels.hpp"
#include "../level.hpp"
#include "../../banks.hpp"   // ACTORS
#include "../../actor/animator.hpp"
#include <pnes/extras/debug/emphasis.hpp>

using namespace demo;
using enum level::eLevelStreamCommands;

namespace level {

// ---------------------------------------------------------------------------
// Deferred column build
//
// ProcessMovement detects the 16px camera-column boundary but no longer builds
// the column there. It records the request here and UpdateActors drains it once
// the whole roster has run -- see UpdateActors at the bottom of this file for
// why that is the right drain point and what it does and does not buy.
//
// One slot, no queue: the hysteresis in ProcessMovement admits at most one
// boundary per frame (|scroll| <= 16 + dead zone), and the drain runs in the
// same frame as the request, so this is always NONE on entry to UpdateActors.
//
// Pinned to absolute .bss on NES for the same reason as collision_map.cpp's
// window state: page zero is scarce (FamiStudio's ZEROPAGE segment sits right
// above it) and three bytes touched once per ~6 frames do not earn a slot.
#ifdef TARGET_NES
#define pending_cold __attribute__((section(".bss")))
#else
#define pending_cold
#endif

enum class ePendingColumn : u8 { NONE = 0, NEXT, PREV };

pending_cold static ePendingColumn pendingColumnKind;
pending_cold static u16            pendingColumnIndex;

ACTORS static u8 PlayerPort(const Player* p)     { return p == &player1 ? port1     : port2;     }
ACTORS static u8 PlayerLastPort(const Player* p) { return p == &player1 ? lastPort1 : lastPort2; }

ACTORS static void PlayerFollowCamera(Player* p) {
    Actor*       self = &p->actor;
    const Actor* lead = &player1.actor;
    const auto   maxX = static_cast<i16>(video::viewport_px() - 16);

    const i16 delta = static_cast<i16>(self->worldSpace.x - lead->worldSpace.x);
    i16       sx    = static_cast<i16>(lead->screen.x) + static_cast<i16>(delta >> 3);

    if (sx < 0 || sx > maxX) {
        sx = sx < 0 ? 0 : maxX;
        self->worldSpace.x = static_cast<u16>(lead->worldSpace.x
            + static_cast<u16>(static_cast<i16>(sx - static_cast<i16>(lead->screen.x)) << 3));
    }

    self->screen.x = static_cast<oam::oam_t>(sx);
}

// ---------------------------------------------------------------------------
// Collision helpers
// ---------------------------------------------------------------------------

ACTORS static u16 ActorTX(const Actor* self) { return self->worldSpace.x >> 3; }

ACTORS static bool IsBlocked(const Actor* self, const u16 wx, const u16 wy) {
    return level::Blocked(wx, wy, self->GetSize(), /*collectBlocks=*/false);
}

ACTORS static void PushCoinVram(const CoinPick& pick) {
    const u8  m  = pick.reveal;
    const u16 tx = static_cast<u16>(pick.col) << 1;
    const u16 ty = static_cast<u16>(2 + (pick.row << 1));
#ifdef TARGET_NES
    const u16 nt_h = static_cast<u16>((tx >> 5) & 1) << 10;
    const u16 base = 0x2000 + nt_h + (static_cast<u16>(ty) << 5) + (tx & 0x1F);
    CoinVramPush(base,      Metatiles_UL[m], Metatiles_UR[m]);
    CoinVramPush(base + 32, Metatiles_BL[m], Metatiles_BR[m]);
#else
    // Unpaired: four tiles, four projections. See level.cpp's kCoinVramRec for why
    // this backend does not take the pairing.
    CoinVramPush(ppu::CartesianToAddress({tx,     ty}),     Metatiles_UL[m]);
    CoinVramPush(ppu::CartesianToAddress({static_cast<u16>(tx + 1), ty}),     Metatiles_UR[m]);
    CoinVramPush(ppu::CartesianToAddress({tx,     static_cast<u16>(ty + 1)}), Metatiles_BL[m]);
    CoinVramPush(ppu::CartesianToAddress({static_cast<u16>(tx + 1), static_cast<u16>(ty + 1)}), Metatiles_BR[m]);
#endif
}

// The CoinGateSkips test is the whole point of the gate: on a frame where the
// AABB resolves to the same cells the collector already scanned empty -- which
// is most frames, since a cell is 16px and the player covers ~2.5px/frame --
// there is provably nothing to find, so we never make the call and never pay
// its entry. See CoinGateSkips (collision_map.hpp) for why the pixel test is
// equivalent to the cell-window test it replaces.
ACTORS static void CollectCoinsFor(const Actor* self, const Player* p) {
    const u16 px = ActorTX(self);
    const u16 py = self->screen.y;
    bool got;
    if (p == &player1) {
        if (CoinGateSkips(coinGate1, px, py)) return;
        got = CollectCoins(px, py, self->GetSize());
    } else {
        if (CoinGateSkips(coinGate2, px, py)) return;
        got = CollectCoins2(px, py, self->GetSize());
    }
    if (got) PushCoinVram(coinPick);
}

ACTORS static void ProcessMovement(Player* p, const vec2<i8> moveForce, const u16 tx,
                                   const bool blockedBelow) {
    Actor* self = &p->actor;
    const auto playerMaxXPos = (video::viewport_px() - 16);
    {
        const int rawY = static_cast<int>(self->worldSpace.y) + moveForce.y;
        const i16 dy   = static_cast<i16>(rawY >> 3) - static_cast<i16>(self->worldSpace.y >> 3);
        if (dy) {
            const u16 ny = static_cast<u16>(self->screen.y) + dy;
            const bool hit = dy == 1 ? blockedBelow : IsBlocked(self, tx, ny);
            if (!hit) {
                self->worldSpace.y = static_cast<u16>(rawY) & 0x7ff;
                self->screen.y     = static_cast<oam::oam_t>(self->worldSpace.y >> 3);
            } else {
                self->worldSpace.y &= ~0x7;   // bonk: rest on the pixel, drop sub-px carry
                self->moveForce.y = 0;        // land or head-bonk both kill vertical speed
            }
        } else {
            self->worldSpace.y = static_cast<u16>(rawY) & 0x7ff;
        }
    }

    if (!moveForce.x) return;

    const int rawX = static_cast<int>(self->worldSpace.x) + moveForce.x;
    const i16 dx   = static_cast<i16>(rawX >> 3) - static_cast<i16>(self->worldSpace.x >> 3);
    if (!dx) {
        if (rawX >= 0) self->worldSpace.x = static_cast<u16>(rawX);
        return;
    }

    // Left world edge: hard wall.  Also guards rawX < 0 wrapping through u16.
    if (rawX < 0 || (dx < 0 && tx == 0)) { self->moveForce.x = 0; return; }
    if (IsBlocked(self, static_cast<u16>(static_cast<i16>(tx) + dx), self->screen.y)) {
        self->moveForce.x = 0;
        return;
    }

    if (p == &player1) {
        // P1: absorb off-centre X motion into camera scroll.
        // ReSharper disable once CppVariableCanBeMadeConstexpr
        const auto playerScrollPos = (video::viewport_px() >> 1);
        const u16 maxCam = maxCameraX;   // level-constant, hoisted -- see levels.hpp
        i16       sx     = static_cast<i16>(self->screen.x) + dx;
        const i16 excess = sx - static_cast<i16>(playerScrollPos);

        i16 scroll = 0;
        if (excess > 0) {
            const u16 room = cameraX < maxCam ? maxCam - cameraX : 0;
            scroll = static_cast<u16>(excess) <= room ? excess : static_cast<i16>(room);
        } else if (excess < 0) {
            const u16 want = static_cast<u16>(-excess);
            scroll = want <= cameraX ? excess : -static_cast<i16>(cameraX);
        }

        if (scroll) {
            cameraX = static_cast<u16>(static_cast<i16>(cameraX) + scroll);
            sx     -= scroll;

            // Hysteresis: a column build fires only once the camera has travelled a
            // full 16px column past the last build.  |scroll| <= 16 + dead zone =>
            // at most one boundary per frame, so one non-looping build suffices.
            //
            // The build itself is deferred to UpdateActors (see ::pendingColumnKind).
            // lastXWorldSpace is still advanced HERE, not at the drain: it is what
            // the hysteresis above tests, so leaving it stale would re-trigger the
            // same boundary next frame. The NMI also reads it, but only under
            // STREAM_LEVEL_DONE, and DONE is now published by the drain once the
            // buffers are actually complete -- so the advance is invisible until
            // the column matching it exists.
            if (scroll > 0) {
                if (cameraX >= lastXWorldSpace + 16 && lastXWorldSpace + 16 != maxCam) {
                    levelStreamCommand = STREAM_LEVEL_RIGHT;
                    if (lastDeltaScroll < 0)
                        levelStreamCommand = levelStreamCommand | STREAM_LEVEL_SWAP;
                    pendingColumnIndex = ((lastXWorldSpace + 16) >> 4) + viewport_mx();
                    pendingColumnKind  = ePendingColumn::NEXT;
                    lastDeltaScroll = static_cast<i8>(scroll);
                    lastXWorldSpace += 16;
                }
            } else if (cameraX <= lastXWorldSpace - 16 && lastXWorldSpace != 16) {
                levelStreamCommand = STREAM_LEVEL_LEFT;
                if (lastDeltaScroll > 0)
                    levelStreamCommand = levelStreamCommand | STREAM_LEVEL_SWAP;
                lastDeltaScroll = static_cast<i8>(scroll);
                pendingColumnIndex = ((lastXWorldSpace - 16) >> 4) - 1;
                pendingColumnKind  = ePendingColumn::PREV;
                lastXWorldSpace -= 16;
            }
        }

        if (sx < 0) sx = 0;
        else if (sx > static_cast<i16>(playerMaxXPos)) sx = static_cast<i16>(playerMaxXPos);
        self->screen.x = static_cast<oam::oam_t>(sx);
        // Recompose canonical sub-pixel X from camera + clamped screen position.
        self->worldSpace.x = static_cast<u16>(((cameraX + self->screen.x) << 3) | (static_cast<u16>(rawX) & 7));
    } else {
        // P2: worldSpace.x is canonical; screen.x follows from P1 (and so from
        // the camera P1 drives) in PlayerFollowCamera, which also handles the
        // viewport-edge clamp.
        self->worldSpace.x = static_cast<u16>(rawX);
        PlayerFollowCamera(p);
    }
}

ACTORS void Player::Update() {
    if (paused) return;
    Actor* self = &actor;

    debug::SetColour(ppu::mask::RED);
    if (this != &player1) PlayerFollowCamera(this);

    const u16 tx  = ActorTX(self);
    debug::SetColour(ppu::mask::GREEN);
    const bool blockedBelow = IsBlocked(self, tx, static_cast<u16>(self->screen.y + 1));
    bool grounded = blockedBelow;

    const u8   port     = PlayerPort(this);
    const u8   lastPort = PlayerLastPort(this);
    const bool left     = port & input::LEFT;
    const bool right    = port & input::RIGHT;
    const i8   dir      = (right && !left) ? 1 : ((left && !right) ? -1 : 0);

    // Facing follows the last directional input, not the velocity. dir is 0
    // both when nothing is held and when left and right are held together, and
    // a zero leaves the latch alone -- so releasing the d-pad, skidding to a
    // halt, or mashing the pair all keep the player looking the way they last
    // actually asked for. Nothing here cares how long the input was held or
    // whether it was tapped: one frame of it is enough to turn.
    //
    // Grounded only: air control still steers (see kAirAccel below), but it
    // cannot turn the sprite round. You leave the ground facing the way you
    // last faced on it and land that way. Note this reads the footing from
    // before the jump launch zeroes it further down, so the frame you press A
    // can still turn you -- you were on the ground when you asked.
    if (grounded && dir != 0) self->horizontalFacingDir = dir < 0;

    // -- horizontal: build speed toward a cap, shed it via friction/skid --
    const i8 vx0 = self->moveForce.x;
    i8       vx  = vx0;

    if ((port & input::B) && grounded && dir != 0 && (vx == 0 || (vx > 0) == (dir > 0)))
        runTimer = kRunRetain;
    else if (runTimer)
        runTimer--;
    const i8 maxSpeed = runTimer ? kMaxRun : kMaxWalk;

    if (dir != 0) {
        // dir in {-1,1}; negate-by-sign avoids __mulhi3 (i8*i16 = ~100 cycles).
        const auto dirForce = [dir](i16 k) -> i16 { return dir > 0 ? k : static_cast<i16>(-k); };
        const bool sameDir = vx == 0 || (vx > 0) == (dir > 0);
        if (!sameDir)
            xForce += dirForce(grounded ? kSkid : kAirAccel);
        else if (Abs(vx) < maxSpeed)
            xForce += dirForce(grounded ? (runTimer ? kRunAccel : kWalkAccel) : kAirAccel);
        else if (Abs(vx) > maxSpeed && grounded)
            xForce += dir > 0 ? -kFriction : kFriction;
    } else if (grounded && vx != 0) {
        xForce += vx > 0 ? -kFriction : kFriction;
    }

    while (xForce >= 256)  { xForce -= 256; if (vx < kMaxRun)  vx++; }
    while (xForce <= -256) { xForce += 256; if (vx > -kMaxRun) vx--; }

    // Friction must settle to rest, not creep into the opposite direction.
    if (dir == 0 && grounded && vx0 != 0 && (vx0 > 0) != (vx > 0)) {
        vx = 0;
        xForce = 0;
    }
    self->moveForce.x = vx;

    // -- vertical: launch on A, then variable gravity until something stops us --
    if (grounded && (port & ~lastPort & input::A)) {
        self->moveForce.y = static_cast<i8>(-(Abs(vx) >= kMaxWalk ? kRunJumpInit : kJumpInit));
        yForce = 0;
        grounded = false;
    }
    if (grounded) {
        self->moveForce.y = 0;
        yForce = 0;
    }

    const bool rising = self->moveForce.y < 0;
    yForce += (rising && (port & input::A)) ? kRiseGravity : kFallGravity;
    i8 vy = self->moveForce.y;
    // yForce is always non-negative; extracting whole-256 ticks is a high-byte
    // load + mask, not a loop.  kFallGravity=896 previously cost 3-4 iterations.
    {
        const i8 ticks = static_cast<i8>(static_cast<u16>(yForce) >> 8);
        yForce &= 0xFF;
        const i8 newVy = static_cast<i8>(static_cast<i16>(vy) + ticks);
        vy = newVy > kMaxFall ? kMaxFall : newVy;
    }
    self->moveForce.y = vy;

    debug::SetColour(ppu::mask::BLUE);
    ProcessMovement(this, self->moveForce, tx, blockedBelow);

    // Pickup runs on the committed position; queues nametable writes for NMI drain.
    debug::SetColour(ppu::mask::RED | ppu::mask::GREEN);
    CollectCoinsFor(self, this);

    // Animation last, on the committed state: ProcessMovement may have zeroed
    // moveForce against a wall, and that should show in the frame picked.
    debug::SetColour(ppu::mask::RED | ppu::mask::BLUE);
    demo::actor::NextFrame(self, grounded);
    self->Update();
    debug::SetColour(0);
}

ACTORS void Player::Start() {
    actor.actorType = Actor::eActorType::Player;
    actor.gravity   = 0;
    actor.moveForce = vec2<i8>{0, 0};
    xForce   = 0;
    yForce   = 0;
    runTimer = 0;
    actor.Start();
}

// Run the column build recorded by ProcessMovement and publish it to the NMI.
//
// NI: UpdateActors is itself inlined into mmc3::Call's ::FIXED window
// trampoline, so without this the two builders follow it in and ~600 bytes of
// column-building land in the always-mapped $E000-$FFFF bank -- measured, not
// theoretical: the first cut of this change moved .prg_rom_actors from 3983 to
// 3399 bytes and grew .prg_rom_fixed by the same amount. Same failure mode
// ColMapSeed and LoadLevel carry their own NI for. One jsr into the actors
// bank keeps the builders where they belong.
NI ACTORS static void DrainPendingColumn() {
    // A column composes its own half of its attribute byte and preserves the
    // other, which in steady scroll holds the partner column the previous build
    // just composed. A REVERSAL (::STREAM_LEVEL_SWAP, set at detection) breaks
    // that: the previous build was a column a whole screen away, so the half
    // being preserved is unrelated to this byte. When that stale half is the one
    // already ON SCREEN -- the partner towards the middle of the view, i.e. the
    // column before this one going right / after it going left -- recompose it
    // first, so the byte written this frame is whole and correct. (The other
    // parity needs nothing: that build clears the byte and its partner half
    // covers the column just beyond the fill, still off-screen.) TileBuffer is
    // fully overwritten by the second call, so the throwaway build costs only
    // its own 14 rows of attribute work, on reversal frames alone.
    //
    // This is what the NMI's unconditional attribute write rests on: before it,
    // a reversal's write was skipped outright and the revealed column wore the
    // previous content's palettes until the pair completed a column later.
    const bool reversed = static_cast<bool>(levelStreamCommand & eLevelStreamCommands::STREAM_LEVEL_SWAP);
    if (pendingColumnKind == ePendingColumn::NEXT) {
        if (reversed && (pendingColumnIndex & 1))
            BuildNextColumn(TileBuffer, static_cast<u16>(pendingColumnIndex - 1));
        BuildNextColumn(TileBuffer, pendingColumnIndex);
    } else {
        if (reversed && !(pendingColumnIndex & 1))
            BuildPrevColumn(TileBuffer, static_cast<u16>(pendingColumnIndex + 1));
        BuildPrevColumn(TileBuffer, pendingColumnIndex);
    }
    pendingColumnKind = ePendingColumn::NONE;
    // Publish last: the NMI reads TileBuffer/AttributeBuffer (and the
    // lastXWorldSpace that addresses them) only once DONE is set, so the
    // direction/swap bits set at detection time sit inert until here. An NMI
    // landing in that gap simply writes nothing and the drain lands on the
    // next one, with lastXWorldSpace and the column still agreeing.
    levelStreamCommand = levelStreamCommand | STREAM_LEVEL_DONE;
}

// Single per-frame entry point into the actors bank: updates the whole roster
// behind ONE window switch instead of one per actor.
//
// The deferred column build is drained HERE rather than from level::main()'s
// loop for two reasons. BuildNextColumn/BuildPrevColumn are ::ACTORS, so a
// drain from main() (::LEVEL_CODE) would need a second mmc3 window switch,
// against the one-switch-per-frame design this function exists to enforce. And
// it has to run before ColMapTrack, which is the next thing main() does: the
// build reads the composite window through ColMapColumn, so draining here keeps
// it reading the exact ViewMap state the inline build used to see. Nothing
// about the produced column changes.
//
// What this buys: the ~1300-1500 cycle build leaves Player::Update's body and
// its soft-stack frame, and gets its own emphasis band. What it does NOT buy:
// any reduction in total frame work -- the drain is in the same frame as the
// request, before the same WaitForPresent. Splitting the build across two
// frames is what would cut the worst-case frame, and that needs a real state
// machine here (direction reversal mid-build, level edges), not just a move.
ACTORS void UpdateActors() {
    player1.Update();
#ifdef PLAYER2_SUPPORTED
    if (multiplayer) {
        player2.Update();
    }
#endif

    if (pendingColumnKind != ePendingColumn::NONE) {
        DrainPendingColumn();
    }
}

}   // namespace level
