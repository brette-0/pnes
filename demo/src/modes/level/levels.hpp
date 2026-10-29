#pragma once

#include "types.hpp"
#include "cursor.hpp"
#include "dynamic.hpp"   // dynEdgeR / dynEdgeL, lockstep with edgeR / edgeL

#include <intsh>
#include <pnes/technology.hpp>
#include <pnes/types.hpp>      // vec2
using namespace br0::intsh;

namespace level {
    constexpr auto levelHeight = 14;
    constexpr i16 kHudRows = 1;
    extern u16 nColumns;

    // Highest cameraX the level allows: (nColumns - viewport_mx()) << 4.
    // Derived from nColumns, so constant for the whole level -- but it sits in
    // ProcessMovement's camera-scroll path, which recomputed it every frame P1
    // moved horizontally. A 16-bit subtract plus a 16-bit shift-by-4 (eight
    // shift/rotate pairs on a 6502) to reach an answer that cannot change.
    // Set by LoadLevel alongside nColumns.
    extern u16 maxCameraX;
    extern const u8* TileData;
    extern const u8 LevelDataAttributes[];
    extern Cursor edgeR;
    extern Cursor edgeL;   // seeded valid at reset, kept in lockstep with edgeR
    extern u8 attr_half;
    extern u8 AttributeBuffer[8];
    extern const Level Levels[];
    u8 GetPrevWrite(u8 step);
    u8 GetNextWrite(u8 step);
    u8 GetCurrentNext(u8 step);
    u8 GetCurrentPrev(u8 step);
    u8 GetPrevMetaTile();
    u8 GetNextMetaTile();
    void BuildNextColumn(u8* buf, u16 worldCol);   // right-scroll: reads window column
    void BuildPrevColumn(u8* buf, u16 worldCol);   // left-scroll: reads window column
    bool LoadLevel(u16 n);

    // Resets edgeR/dynEdgeR to the level start (world column 0) and fills
    // nCols nametable columns (i in [0, nCols), so nCols should be even -- it
    // is two tile columns per metatile column) from there via
    // GetNextWrite/GetCurrentNext -- the same streaming
    // fill EnterLevelSetup uses to seed the level view, and title.cpp's
    // level preview to render a shorter version of the same thing. Always
    // writes at nametable row 2 (just below the 2-row HUD) and 28 rows
    // tall (levelHeight metatiles) -- both GetNextWrite/GetCurrentNext's
    // own attribute math are hardwired to that placement.
    //
    // An ODD number of METATILE columns (nCols == 2 mod 4) is allowed: the last
    // attribute byte then carries only its left half, whose right half covers
    // the metatile column after the fill, and nothing downstream inherits a
    // parity from the count (see ::attr_half, levels.cpp).
    void PopulateNameTableColumns(u16 nCols);
}
