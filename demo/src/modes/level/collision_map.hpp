#pragma once

#include "cursor.hpp"
#include "dynamic.hpp"                 // DynamicCursor (dynamic-plane producer edge)
#include "../../graphics/metatiles.hpp"   // MetatileCollision (query decodes per cell)
#include <pnes/video.hpp>      // video::viewport_tx (window width tracks the viewport)
#include <pnes/types.hpp>      // vec2
#include <intsh>
using namespace br0::intsh;

namespace level {

// ---------------------------------------------------------------------------
// Shared composite-metatile window (1 byte per cell)
//
// Every collision / render consumer wants the SAME thing: the metatile actually
// occupying a visible cell.  The static plane (walls/ground) and the dynamic
// plane (coins) are both RLE-compressed in ROM/RAM and decoding either by an
// RLE walk is the per-query cost we keep paying.  So we decode ONCE, on column
// entry, into a flat RAM window: each cell holds the COMPOSITE metatile id
// (dynamic-over-static, dynamic wins when non-air -- identical to the render
// path's GetNextMetaTile).  Collision then becomes a flat index + a one-mask
// collision-class decode, with no RLE on the hot path; the window only re-decodes
// the single column that enters as the camera scrolls.
//
// One byte per cell is the whole point: the metatile is the unit of detail for
// this plane, so every future system (more layers, render-from-shadow, dynamic
// edits) composites into the SAME byte instead of growing a new parallel buffer.
//
// Audience split falls straight out of the metatile collision class:
//   Solid   -> blocks everyone (wall/ground)
//   Collect -> blocks the player only (coin); enemies test == Solid, so they pass
//   None    -> air
//
// Window: viewport (16 metatile cols) + 4 left + 4 right = 24 columns, centred on
// the camera, levelHeight(=14) rows each.  It is a ring: a world column maps to a
// slot base-relative to the leftmost column held, so the non-power-of-2 width
// needs only a subtract + one conditional wrap, never a modulo.
// ---------------------------------------------------------------------------

// Window width = viewport(in metatiles) + 4 left + 4 right.  This is the SOLE knob
// that makes the window track the viewport across both targets:
//   - NES: viewport is a fixed 16 metatiles, so the width folds to a compile-time 24.
//     ColMapWidth() is constexpr, so every ring compare/wrap below becomes "... 24".
//   - Desktop (LANDSCAPE): the viewport width is a runtime function of the display
//     mode (video::viewport_tx() depends on `mode`/`scale`), so the ring modulus must
//     be that runtime value -- a hardcoded 24 was too narrow for wide displays, which
//     is what dropped ground collision / coin pickups / a render column past a scroll
//     threshold.  The ViewMap array is sized to a compile-time max (kColMapWidth) so it
//     never depends on the resolved mode; only the ring math uses the runtime width.
//
// u8 ring invariant: a ring slot is (origin + delta), both < width, so slot < 2*width.
// kColMapWidth is kept <= 128 so that sum always fits a u8 (max slot 254); realistic
// desktop viewports resolve to width ~70, far under the cap.
#ifdef TARGET_NES
constexpr u8 kColMapWidth = 24;                 // array bound AND ring modulus (folds)
constexpr u8 ColMapWidth() { return kColMapWidth; }
#else
constexpr u8 kColMapWidth = 128;                // array bound only (super-ultrawide cap)
inline u16 ColMapWidth() { return static_cast<u16>((video::viewport_tx() >> 1) + 8); }
#endif

// Composite metatile per cell; ring of kColMapWidth columns x levelHeight rows.
// Column-major: slot s occupies [s*levelHeight .. s*levelHeight+levelHeight-1], so
// a slid column rewrites one contiguous levelHeight-byte span.
extern u8 ViewMap[];
// Leftmost world metatile column currently held in the window.
extern u16 ColMapBaseCol;
// Ring index of ColMapBaseCol (rotates as the window slides; avoids memmove).
extern u8  ColMapOrigin;

// Pointer to the levelHeight composited metatiles of world column `col` (top cell
// first), or nullptr if the column is outside the currently held window.  This is
// the single decompressed view both collision (Blocked/CollectCoins) and render
// (BuildNextColumn/BuildPrevColumn) read -- the same slot resolution used inside
// Blocked, exposed so the render path no longer re-walks the RLE for a column the
// window already holds.
const u8* ColMapColumn(u16 col);

// Stamp one column's levelHeight composite metatiles into ring slot `slot`.
// `stat` is a static Cursor and `dyn` a DynamicCursor, both parked on the column's
// top cell (row 0); they are walked down the column in lockstep and composited
// (dynamic wins when non-air).  Both caller copies are consumed.
void ColMapStamp(u8 slot, Cursor stat, DynamicCursor dyn);

// Seed the whole window to cover world columns [leftCol .. leftCol+kColMapWidth-1]
// from a static Cursor + dynamic Cursor parked on (leftCol, row 0).  Used at level
// load.  Also captures the internal left/right edge cursors that ColMapTrack walks.
void ColMapSeed(u16 leftCol, Cursor stat, DynamicCursor dyn);

// Per-frame producer driver.  `camLeftCol` is the camera's leftmost viewport
// metatile column (cameraX >> 4).  Lazily slides the window so its base tracks
// (camLeftCol - 4), clamped to [0, nColumns-kColMapWidth], stamping at most one
// entering column per call from the module's own edge cursors -- so the window
// stays centred on the camera WITHOUT touching the render edge bookkeeping
// (edgeR / lastXWorldSpace).  Call once per frame after the camera settles.
void ColMapTrack(u16 camLeftCol);

// Slide the window one column: the entering column's top cell is given by the
// static + dynamic cursors.  (Left slide is the mirror of right.)
void ColMapSlideRight(const Cursor &newRightStat, DynamicCursor newRightDyn);
void ColMapSlideLeft(const Cursor &newLeftStat, DynamicCursor newLeftDyn);

// AABB block test, world-pixel inputs.  py is world pixel Y; the HUD strip is
// removed internally so the row index lands in level space, consistent with how
// ColMapStamp packed it.  Columns outside the window or rows outside
// [0,levelHeight) read as air.
//
// `collectBlocks` is VESTIGIAL: every call site passes false (player.cpp's
// IsBlocked, actor.cpp's Update), so coins never stop anyone and LTO folds the
// parameter away -- the inner loop compares against Solid only.  It is kept as
// a parameter rather than deleted because the signature documents the axis, but
// do not read it as a live option: an earlier comment here claimed the player
// passed true ("coins block, pending pickup") and that was never the behaviour
// in this revision.  Coins are taken by CollectCoins on a separate pass; making
// them solid would stop the player dead on top of one.
bool Blocked(u16 px, u16 py, vec2<u8> dimensions, bool collectBlocks);

// One collected coin: its world metatile cell and the static metatile revealed
// underneath (what the cell shows now that the coin is gone).  The caller turns
// these into nametable tile writes; CollectCoins has already done the data side.
struct CoinPick { u16 col; u8 row; u8 reveal; };

// The pick from the most recent CollectCoins/CollectCoins2 that returned true.
// A single shared slot rather than a caller-supplied `CoinPick* out` + `maxOut`
// pair: the collectors take AT MOST ONE coin per call by design, so an array and
// a budget were always a one-element array and a constant 1 -- three parameter
// bytes and an indirect store that the register allocator paid for on every
// call, including the common no-coin one.  Read it ONLY when the call returned
// true; it is stale otherwise, and the next call overwrites it.  `reveal` cannot
// serve as its own "nothing here" sentinel: metatile $00 is air, which is a
// perfectly ordinary thing to reveal under a coin.
extern CoinPick coinPick;

// Collect one coin (Collect-class cell) the AABB overlaps, if any: the run is
// blanked in DynData so it never recomposites when the column re-enters the
// window, and the window cell is replaced with the revealed static metatile so
// collision + render see it gone immediately.  Returns whether a coin was taken;
// the cell + revealed metatile land in `coinPick` above.
// Same world-pixel / HUD-strip convention as Blocked.
bool CollectCoins(u16 px, u16 py, vec2<u8> dimensions);
bool CollectCoins2(u16 px, u16 py, vec2<u8> dimensions);

// ---------------------------------------------------------------------------
// Collector gate
//
// The pixel position that produced the cell window the collector last scanned,
// plus whether that scan found it empty.  One per player, mirroring the two
// CollectorAnchors inside collision_map.cpp.
//
// This exists because CollectCoins' normal answer is "nothing here", and
// reaching that answer used to cost its whole out-of-line entry: a ~165-cycle
// soft-stack prologue plus the AABB bounds math and two inlined multiply-by-14
// chains, before the in-body "same cells, already verified empty" guard could
// fire.  The guard is now hoisted HERE, in a form the caller can inline, so the
// no-op frame never makes the call at all.
struct CoinGate { u16 px; u16 py; bool dry; };
extern CoinGate coinGate1;
extern CoinGate coinGate2;

// Whether the AABB at (px, py) resolves to exactly the cell window the last call
// already scanned empty -- in which case there is nothing for the collector to
// find and the call can be skipped entirely.
//
// This is the in-body guard (`step == 0 && sameColR && sameRowB && dry`)
// restated on pixels.  The two agree: for a fixed 16px AABB, colL = px >> 4 and
// colR = colL + (px & 15 ? 1 : 0), so the column pair is unchanged exactly when
// px stays in one 16px cell AND does not move onto or off the cell boundary;
// likewise for rows, and clamping is a function of the unclamped pair so it
// cannot separate two equal ones.  `step` is therefore 0 whenever this returns
// true, which is what makes skipping the unconditional re-anchor Move safe --
// that Move must not be gated on "a coin is present" (it was, once, and the
// accumulated drift spiked) but gating on "the window did not move" is exactly
// the case where it would have moved nothing.
//
// Written as xor + mask rather than four `>> 4`s: a u16 shift-by-4 is four
// lsr/ror pairs, and the question is only whether two positions share a cell.
// AI so the concrete global folds to absolute addressing at the call site --
// same reason AdvancePairForward (collision_map.cpp) carries it.
AI inline bool CoinGateSkips(const CoinGate& g, const u16 px, const u16 py) {
    if (!g.dry) return false;
    return ((px ^ g.px) & 0xFFF0u) == 0
        && ((py ^ g.py) & 0xFFF0u) == 0
        && ((px & 0xFu) == 0) == ((g.px & 0xFu) == 0)
        && ((py & 0xFu) == 0) == ((g.py & 0xFu) == 0);
}

}   // namespace level
