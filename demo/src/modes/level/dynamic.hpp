#pragma once

#include "cursor.hpp"
#include <intsh>

using namespace br0::intsh;

namespace level {

// ---------------------------------------------------------------------------
// Dynamic tile layer
//
// A second RLE plane authored as a separate Tiled layer, compressed with the
// same {length, data} scheme and covering the same WxH metatile grid as the
// static plane. It holds the interactive / animated tiles -- coins, blocks,
// ... -- that the game mutates at runtime. It has no terminator of its own: it
// spans the same number of cells as the static plane, whose 0x00 terminator
// already bounds the walk.
//
// Unlike the static plane (run-data lives in ROM and is fetched in place), the
// dynamic plane's run DATA is copied once into a fixed RAM array so individual
// runs can be blanked when their tile is consumed. The run LENGTHS stay in ROM
// (they never change). So a DynamicCursor streams lengths from ROM but peeks
// data from RAM.
//
// All three integration points are live: LoadDynamicLayer is called from
// LoadLevel (levels.cpp), ColMapStamp composites dynamic-over-static into
// ViewMap, and CollectCoinsImpl walks the probe pair (collision_map.cpp).
// 1-1 ships a real dynamic layer.
// ---------------------------------------------------------------------------

// Max dynamic RUNS held in RAM. The dynamic plane is sparse (mostly air), so a
// long level still compresses to well under this. Overflow becomes a build-time
// check once real export exists.
constexpr u16 DynRunCapacity = 512;

// Mutable, RAM-resident copy of the dynamic plane's run-data bytes (metatile
// ids; 0 = air = "no dynamic tile here"). Indexed by run, exactly like the
// static TileData / Cursor.offset relationship.
extern u8 DynData[DynRunCapacity];

// ROM sources for the active level's dynamic plane. Null until a level provides
// one. DynLengths mirrors HunkLengths (run lengths); DynDataROM is the run-data
// copied into DynData at load.
extern const u8* DynLengths;
extern const u8* DynDataROM;

// RLE walker over the dynamic plane: lengths from ROM (DynLengths), data from
// RAM (DynData). Mirrors Cursor, but Fetch() peeks the mutable RAM copy so a
// removal can blank a run in place. (Walk logic is duplicated from Cursor for
// the prototype; unify behind a shared walker once the dynamic path is proven.)
class DynamicCursor {
public:
    const u8* lp;   // pointer into DynLengths at the current run
    u8*       dp;   // pointer into DynData (RAM) at the current run
    u8        progress;

    // Fast hot-path walk: i8 covers ±levelHeight.
    void Move(i8 amt);
    // Large-displacement walk for re-anchor / seek paths.
    void Seek(i16 amt);

    [[nodiscard]] u8  Fetch() const { return *dp; }                          // RAM peek
    [[nodiscard]] u16 Run()   const { return static_cast<u16>(dp - DynData); } // run index
};

// Forward/backward dynamic-plane edge walkers, the dynamic analogue of
// level::edgeR / edgeL.  Held in lockstep with those static cursors: every Move
// applied to an edge cursor is mirrored here, so a dyn cursor always sits on the
// SAME absolute metatile as its static partner.  The metatile fetch primitives
// read these to composite the dynamic tile over the static one (non-zero wins).
extern DynamicCursor dynEdgeR;   // lockstep with level::edgeR (forward/right edge)
extern DynamicCursor dynEdgeL;   // lockstep with level::edgeL (backward/left edge)

// Result of a dynamic-plane collision probe: whether a non-air dynamic tile was
// overlapped, its id (for rule dispatch) and the run that owns it (to blank).
struct DynHit {
    bool hit;
    u8   id;
    u16  run;
};

// Probe an AABB against the dynamic plane. Mirrors CollidesSolid, but instead of
// a bool it returns the first overlapped non-air dynamic tile (id + owning run)
// so the caller can dispatch its removal rule. `origin` must already sit on the
// metatile containing the AABB's top-left corner (px, py) -- same contract as
// CollidesSolid.
DynHit CollidesDynamic(const DynamicCursor& origin, u16 px, u16 py, u8 w, u8 h);

// Dispatch the removal rule for a hit through the jump table (see dynamic.cpp).
// `under` is a static cursor already positioned on the SAME cell, so the rule
// can read the metatile underneath and reveal it -- avoiding the SMB1 "coin on a
// wall turns to air" artefact.
void RemoveDynamic(const DynHit& h, const Cursor &under);

// Copy the active level's dynamic ROM run-data into DynData and point DynLengths
// at the level's dynamic lengths. Called from LoadLevel; pass nullptrs / 0 for a
// level that ships no dynamic layer.
void LoadDynamicLayer(const u8* dynLengthsROM, const u8* dynDataROM, u16 runCount);

}   // namespace level
