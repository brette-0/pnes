#include "levels.hpp"
#include "collision_map.hpp"   // ColMapColumn: read the composited column the window already built
#include "../../graphics/metatiles.hpp"
#include "../../banks.hpp"     // ACTORS, level_graphics_tag
#include <pnes/mappers/mmc3.hpp>  // mmc3::CallInBlock
#include <pnes/video.hpp>         // ppu::WriteFromProviderToNameTable/WriteFromBufferToAttributeTable

#include <intsh>
#include <pnes/technology.hpp>

#include "types.hpp"
using namespace br0::intsh;

namespace level {
    Cursor edgeR;
    Cursor edgeL;
    u16 nColumns;
    u16 maxCameraX;   // derived from nColumns; see levels.hpp

    const u8* TileData;   // static plane: flat column-major ROM array, see types.hpp

    u8 MetatileBuffer[14];
    u8 AttributeBuffer[8];
    u8 attr_half;

    // pal is a 2-bit palette index (MetatilePaletteMask) and shift is always one
    // of {0,2,4,6} (an attribute-byte nibble position), so `pal << shift` only
    // ever takes 16 distinct values. The shift amount isn't a compile-time
    // constant at any one call site (it's chosen by a runtime parity/half
    // branch), so `pal << shift` as written compiles to a call to __ashlqi3 --
    // a loop of up to 6 ASLs plus call overhead -- once per metatile row.
    // Indexing this instead is a handful of cycles regardless of shift amount.
    static constexpr u8 kPalShifted[4][4] = {
        { 0,  0,   0,   0 },
        { 1,  4,  16,  64 },
        { 2,  8,  32, 128 },
        { 3, 12,  48, 192 },
    };

    // BuildNextColumn/BuildPrevColumn walk a fixed 14-row column with
    // tile_row = 2+2*mt (Next) / 29-2*mt (Prev), so attr_idx (tile_row>>2) is
    // fully determined by the loop counter mt -- not something that needs
    // recomputing (add + 2 shifts) every row. Likewise is_bottom (tile_row>>1&1)
    // reduces to a plain function of mt's parity (Next: 1 on even mt, 0 on odd;
    // Prev: 0 on even mt, 1 on odd), so the two shift-index values a call can
    // ever produce are known before the loop starts, not per row. Tabling
    // attr_idx and hoisting the shift-index pair out of the loop turns each
    // row's bookkeeping into one indexed load + one already-computed pick,
    // instead of an add, three shifts, and a nested branch every iteration.
    static constexpr u8 kNextAttrIdx[14] = {0,1,1,2,2,3,3,4,4,5,5,6,6,7};
    static constexpr u8 kPrevAttrIdx[14] = {7,6,6,5,5,4,4,3,3,2,2,1,1,0};

    // --- Which half of its attribute byte a column owns ----------------------
    // One NES attribute byte spans 32px == two metatile columns, a left/right
    // nibble pair, so a column's half is simply its ABSOLUTE metatile-column
    // parity: even -> the left nibbles, odd -> the right ones. ::attr_half is
    // that bit, set by whichever composer is about to run, and the shift tables
    // below pick quadrants from it.
    //
    // It used to be tracked instead by a monotonic `attr_column` counter whose
    // phase was seeded once at init and relied on staying in lockstep with the
    // camera (forward: both +1; backward: camera -1 and counter +1). That held
    // the parity but nothing else: it made the half a function of HISTORY rather
    // than of the column, so a viewport spanning a fractional attribute byte
    // (the GBA's 30 tiles = 15 metatiles) needed a hardcoded re-flip after a
    // direction reversal, a preview drawn with an odd column count would silently
    // invert every palette the level later streamed, and nothing could compose a
    // column out of scroll order. Parity taken from the column itself has no
    // phase to keep, so all three go away with the counter.

    // LEVEL_DATA: static plane + DynLengths, read CONTINUOUSLY -- see
    // banks.hpp's ::LevelDataBank.
    LEVEL_DATA const u8 TileData_1_1[] = {
        #include "../../../tiled/include/1-1_st"
    };

    // LEVEL_DYNAMIC, not LEVEL_DATA: read ONCE at load -- see
    // banks.hpp's ::LevelDynamicBank.
    LEVEL_DYNAMIC const u8 DynData_1_1[] = {
        #include "../../../tiled/include/1-1_dt"
    };

    LEVEL_DATA const u8 DynLengths_1_1[] = {
        #include "../../../tiled/include/1-1_dl"
    };

    const Level Levels[] = {
        {TileData_1_1, sizeof(TileData_1_1) / levelHeight,
         DynData_1_1, DynLengths_1_1, sizeof(DynLengths_1_1)}
    };

    // LEVEL_CODE, in window 1 -- see banks.hpp. NI: sole call site is
    // level.cpp's CallInBlock<level_code_tag>, so without it LTO inlines this
    // into the FIXED trampoline instead of LEVEL_CODE -- same failure mode as
    // EnterLevelSetup's own NI.
    NI LEVEL_CODE bool LoadLevel(const u16 n) {
        // TileData holds the active level's STATIC PLANE (walls/ground).
        TileData = Levels[n].TileData;
        nColumns = Levels[n].nColumns;
        // Derived once here rather than per frame in ProcessMovement's camera
        // path. Guarded: a level narrower than the viewport cannot scroll.
        maxCameraX = nColumns > viewport_mx()
                   ? static_cast<u16>((nColumns - viewport_mx()) << 4) : 0;

        // Window 2's bank is a RUNTIME choice keyed by level index, not a
        // compile-time tag -- see banks.hpp's ::LevelDynamicBank/
        // ::LevelDataBank. RAW mmc3::SwitchBank, not mmc3::CallInBlock<Tag>:
        // there is nothing meaningful to restore window 2 TO here. This is
        // level entry, establishing window 2's content for the whole
        // session, not a nested call handing control back to a caller that
        // needs its own prior window 2 state preserved.
        mmc3::SwitchBank(mmc3::window2Control, LevelDynamicBank(n));
        LoadDynamicLayer(Levels[n].DynLengths, Levels[n].DynData, Levels[n].DynRuns);

        // ROM -> RAM copy of the metatile planes (see metatiles.hpp's
        // LoadMetatilePlanes), so collision checks and coin-tile fetches
        // read ambient PRG-RAM afterward instead of farcalling into a
        // paged bank several times per actor per frame.
        mmc3::SwitchBank(mmc3::window2Control, LevelGraphicsBank(n));
        LoadMetatilePlanes();

        // Settle window 2 on the level's STATIC data for the rest of the
        // session: TileData/DynLengths are read every frame via plain
        // pointers, never through a farcall, so window 2 must stay here,
        // untouched, until the level ends.
        mmc3::SwitchBank(mmc3::window2Control, LevelDataBank(n));
        return true;
    }

    AI
    u8 GetNextMetaTile() {
        const u8 s = edgeR.Fetch();
        const u8 d = dynEdgeR.Fetch();
        edgeR.Move(1);
        dynEdgeR.Move(1);
        return d ? d : s;
    }

    AI
    u8 GetPrevMetaTile() {
        const u8 s = edgeL.Fetch();
        const u8 d = dynEdgeL.Fetch();
        edgeL.Move(-1);
        dynEdgeL.Move(-1);
        return d ? d : s;
    }

    // AI again: the metatile planes are ambient PRG-RAM now (see
    // metatiles.hpp's LoadMetatilePlanes), so this no longer crosses a
    // window-2 farcall boundary.
    AI
    u8 GetNextWrite(const u8 step) {
        u8 m;
        if (~step & 1) {
            if (step == 0) {
                // Second of the pair (an odd column) keeps the partner's left
                // nibbles; the first clears the whole byte.
                const u8 mask = attr_half ? 0x33 : 0x00;
                for (auto & j : AttributeBuffer)
                    j &= mask;
                AttributeBuffer[0] |= 0x0F;   // HUD rows 0-1 share attr_idx 0's top quadrant with level rows 2-3; pin it to palette 3
                AttributeBuffer[7] |= 0xF0;   // mirror of the above at the other end: the 8-byte attribute write spans tile rows 0-31, but the level is only rows 0-29, so attr_idx 7's BOTTOM quadrant (rows 30-31) is UI band -- pin it to palette 3 too
            }
            m = GetNextMetaTile();
            MetatileBuffer[step >> 1] = m;
        } else {
            m = MetatileBuffer[step >> 1];
        }
        if (~step & 1) {
            const u8 tile_row  = 2 + step;
            const u8 attr_idx  = tile_row >> 2;
            const u8 pal       = Metatiles_ATTR[m] & MetatilePaletteMask;
            const u8 is_bottom = tile_row >> 1 & 1;
            const u8 shift     = attr_half
                                    ? (is_bottom ? 6 : 2)
                                    : is_bottom ? 4 : 0;
            AttributeBuffer[attr_idx] |= kPalShifted[pal][shift >> 1];
        }
        return step & 1 ? Metatiles_UR[m] : Metatiles_UL[m];
    }

    AI
    u8 GetPrevWrite(const u8 step) {
        u8 m;
        if (~step & 1) {
            if (step == 0) {
                // Travelling left, the pair's RIGHT column is revealed first, so
                // it is the even (left-half) column that keeps its partner.
                const u8 mask = attr_half ? 0x00 : 0xCC;
                for (auto & j : AttributeBuffer)
                    j &= mask;
                AttributeBuffer[0] |= 0x0F;   // HUD rows 0-1 share attr_idx 0's top quadrant with level rows 2-3; pin it to palette 3
                AttributeBuffer[7] |= 0xF0;   // mirror of the above at the other end: the 8-byte attribute write spans tile rows 0-31, but the level is only rows 0-29, so attr_idx 7's BOTTOM quadrant (rows 30-31) is UI band -- pin it to palette 3 too
            }
            m = GetPrevMetaTile();
            MetatileBuffer[step >> 1] = m;
        } else {
            m = MetatileBuffer[step >> 1];
        }
        if (~step & 1) {
            const u8 tile_row  = 29 - step;
            const u8 attr_idx  = tile_row >> 2;
            const u8 pal       = Metatiles_ATTR[m] & MetatilePaletteMask;
            const u8 is_bottom = tile_row >> 1 & 1;
            const u8 shift     = attr_half
                                    ? (is_bottom ? 6 : 2)
                                    : is_bottom ? 4 : 0;
            AttributeBuffer[attr_idx] |= kPalShifted[pal][shift >> 1];
        }
        return step & 1 ? Metatiles_UL[m] : Metatiles_UR[m];
    }

    // GetCurrentNext/GetCurrentPrev only ever read MetatileBuffer (RAM,
    // already filled by GetNextWrite/GetPrevWrite's own pass over the same
    // step) and the metatile planes (RAM mirrors) -- no window-2 dependency.
    AI
    u8 GetCurrentNext(const u8 step) {
        const u8 m = MetatileBuffer[step >> 1];
        return step & 1 ? Metatiles_BR[m] : Metatiles_BL[m];
    }

    AI
    u8 GetCurrentPrev(const u8 step) {
        const u8 m = MetatileBuffer[step >> 1];
        return step & 1 ? Metatiles_BL[m] : Metatiles_BR[m];
    }

    // ACTORS: sole caller is ProcessMovement (player.cpp), in the actors
    // bank -- see banks.hpp's ::actor_tag comment. col[] comes from ColMapColumn
    // (ViewMap, RAM); the metatile planes are RAM mirrors now too, so this
    // fires on every scroll-column boundary crossing (lastXWorldSpace
    // hysteresis in player.cpp) with no window-2 switch at all.
    ACTORS void BuildNextColumn(u8* buf, const u16 worldCol) {
        // Fetched BEFORE the attribute buffer is touched: a column the window
        // cannot supply (never in steady scroll) must leave the buffer exactly
        // as it was, not half-cleared. The NMI's attribute write is
        // unconditional now, so a buffer cleared for a column that was then
        // never composed would publish palette 0 over real content.
        const u8* col = ColMapColumn(worldCol);
        if (!col) return;

        attr_half = worldCol & 1;
        const u8 mask = attr_half ? 0x33 : 0x00;
        for (auto & j : AttributeBuffer) j &= mask;
        AttributeBuffer[0] |= 0x0F;   // HUD rows 0-1 share attr_idx 0's top quadrant with level rows 2-3; pin it to palette 3
        AttributeBuffer[7] |= 0xF0;   // mirror of the above at the other end: the 8-byte attribute write spans tile rows 0-31, but the level is only rows 0-29, so attr_idx 7's BOTTOM quadrant (rows 30-31) is UI band -- pin it to palette 3 too

        // mt even -> is_bottom=1, mt odd -> is_bottom=0 (see kNextAttrIdx comment).
        const u8 shiftIdxEven = attr_half ? 3 : 2;
        const u8 shiftIdxOdd  = attr_half ? 1 : 0;

        for (u8 mt = 0; mt < 14; ++mt) {
            const u8 m   = col[mt];                  // top-to-bottom, slot s row mt
            const u8 pal = Metatiles_ATTR[m] & MetatilePaletteMask;
            const u8 shiftIdx = mt & 1 ? shiftIdxOdd : shiftIdxEven;
            AttributeBuffer[kNextAttrIdx[mt]] |= kPalShifted[pal][shiftIdx];

            const u8 step      = mt << 1;             // even tile-step: 0,2,..,26
            buf[step]          = Metatiles_UL[m];
            buf[step + 1]      = Metatiles_UR[m];
            buf[28 + step]     = Metatiles_BL[m];
            buf[28 + step + 1] = Metatiles_BR[m];
        }
    }

    // NI LEVEL_CODE: same reasoning as LoadLevel's own NI -- this is called
    // from both level.cpp's EnterLevelSetup and title.cpp's level preview,
    // each an out-of-bank farcall target, not worth letting LTO duplicate
    // inline at either call site.
    NI LEVEL_CODE void PopulateNameTableColumns(const u16 nCols) {
        edgeR    = { TileData };
        dynEdgeR = { DynLengths, DynData, 0 };

        for (u16 i = 0; i < nCols; i += 2) {
            // World column i >> 1, drawn from column 0 -- so its own parity is
            // the half it owns (see attr_half). Setting it here rather than
            // letting a counter run is what lets nCols be odd in metatile
            // columns: a trailing half-byte then writes only the half it has,
            // and the level's own streaming is unaffected by how wide the
            // preview that ran before it happened to be.
            attr_half = (i >> 1) & 1;
            ppu::WriteFromProviderToNameTable({i, 2}, GetNextWrite, 28, 1);
            ppu::WriteFromProviderToNameTable({static_cast<u16>(i + 1), 2}, GetCurrentNext, 28, 1);
            ppu::WriteFromBufferToAttributeTable({static_cast<u16>(i & ~3), 2}, AttributeBuffer, 8, 1);
        }
    }

    // ACTORS: see BuildNextColumn's comment.
    ACTORS void BuildPrevColumn(u8* buf, const u16 worldCol) {
        // Fetched first for the same reason as BuildNextColumn's.
        const u8* col = ColMapColumn(worldCol);
        if (!col) return;

        attr_half = worldCol & 1;
        // Travelling left the pair's RIGHT column lands first, so `ap` -- this
        // column being the one that keeps its already-composed partner -- is the
        // inverse of the half, where the forward composer's is the half itself.
        const u8 ap = attr_half ^ 1;
        const u8 mask = ap ? 0xCC : 0x00;
        for (auto & j : AttributeBuffer) j &= mask;
        AttributeBuffer[0] |= 0x0F;   // HUD rows 0-1 share attr_idx 0's top quadrant with level rows 2-3; pin it to palette 3
        AttributeBuffer[7] |= 0xF0;   // mirror of the above at the other end: the 8-byte attribute write spans tile rows 0-31, but the level is only rows 0-29, so attr_idx 7's BOTTOM quadrant (rows 30-31) is UI band -- pin it to palette 3 too

        // mt even -> is_bottom=0, mt odd -> is_bottom=1 (see kPrevAttrIdx comment).
        const u8 shiftIdxEven = ap ? 0 : 1;
        const u8 shiftIdxOdd  = ap ? 2 : 3;

        for (u8 mt = 0; mt < 14; ++mt) {
            const u8 m   = col[13 - mt];              // bottom-to-top, mirroring edgeL's walk
            const u8 pal = Metatiles_ATTR[m] & MetatilePaletteMask;
            const u8 shiftIdx = mt & 1 ? shiftIdxOdd : shiftIdxEven;
            AttributeBuffer[kPrevAttrIdx[mt]] |= kPalShifted[pal][shiftIdx];

            const u8 step  = mt << 1;
            buf[54 - step] = Metatiles_UL[m];
            buf[55 - step] = Metatiles_UR[m];
            buf[26 - step] = Metatiles_BL[m];
            buf[27 - step] = Metatiles_BR[m];
        }
    }

}