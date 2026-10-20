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
    extern const u8* TileData;
    extern const u8 LevelDataAttributes[];
    extern Cursor edgeR;
    extern Cursor edgeL;   // seeded valid at reset, kept in lockstep with edgeR
    extern u8 attr_column;
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
    // nCols nametable columns (i in [0, nCols), so nCols should be even)
    // from there via GetNextWrite/GetCurrentNext -- the same streaming
    // fill EnterLevelSetup uses to seed the level view, and title.cpp's
    // level preview to render a shorter version of the same thing. Always
    // writes at nametable row 2 (just below the 2-row HUD) and 28 rows
    // tall (levelHeight metatiles) -- both GetNextWrite/GetCurrentNext's
    // own attribute math are hardwired to that placement.
    void PopulateNameTableColumns(u16 nCols);
}
