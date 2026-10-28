#include "dynamic.hpp"

#include "levels.hpp"
#include "../../graphics/metatiles.hpp"

#include <array>
#include <intsh>

using namespace br0::intsh;

namespace level {
    // ---------------------------------------------------------------------------
    // Dynamic-plane definitions.  See dynamic.hpp for the model.  This TU is fully
    // additive: it touches no existing engine state, only its own RAM/ROM handles.
    // ---------------------------------------------------------------------------

    // RAM-resident run-data (BSS, DynRunCapacity bytes).  Zero-initialized => all
    // air until a level loads a dynamic layer via LoadDynamicLayer.
    u8 DynData[DynRunCapacity];

    // Null until a level ships a dynamic plane.  CollidesDynamic / DynamicCursor are
    // only ever reached through that path, so the null state is never walked.
    const u8* DynLengths = nullptr;
    const u8* DynDataROM = nullptr;

    // Edge walkers, kept in lockstep with level::edgeR / edgeL (see dynamic.hpp).
    // Seeded to the level start alongside their static partners in RESET.
    DynamicCursor dynEdgeR;
    DynamicCursor dynEdgeL;

    // Hot-path walk: i8 covers ±levelHeight.  lp/dp hoisted to locals (ZP).
    // Floor guard mirrors Cursor::Move: stop at level start, don't underflow.
    //
    // Counts an unsigned magnitude down to zero instead of testing signed `amt`
    // against zero each iteration: 6502 has no signed-compare instruction, so a
    // `>0`/`<0` test costs an extra branch+eor (N/V overflow correction) per
    // iteration that a plain dec+bne countdown doesn't pay. Mirrors the unsigned
    // countdown AdvancePairForward/AdvancePairBackward (collision_map.cpp) already
    // use for the same RLE-walk shape. `0 - static_cast<u8>(amt)` is the
    // wraparound two's-complement magnitude of a negative amt (defined, no UB),
    // including the i8::min edge.
    void DynamicCursor::Move(i8 amt) {
        const u8* dlp = lp;
        u8*       ddp = dp;
        u8 p = progress;
        if (amt > 0) {
            #pragma clang loop unroll(disable)
            for (u8 n = static_cast<u8>(amt); n != 0; --n) {
                if (++p >= *dlp) { ++dlp; ++ddp; p = 0; }
            }
        } else if (amt < 0) {
            #pragma clang loop unroll(disable)
            for (u8 n = static_cast<u8>(0 - static_cast<u8>(amt)); n != 0; --n) {
                if (p == 0) {
                    if (dlp == DynLengths) break;
                    --dlp; --ddp; p = *dlp;
                }
                --p;
            }
        }
        lp = dlp; dp = ddp; progress = p;
    }

    // Large-displacement walk (re-anchor, seek): same engine, i16 counter.
    // See DynamicCursor::Move for why the loop counts an unsigned magnitude
    // instead of testing signed `amt` against zero each iteration.
    void DynamicCursor::Seek(i16 amt) {
        const u8* dlp = lp;
        u8*       ddp = dp;
        u8 p = progress;
        if (amt > 0) {
            #pragma clang loop unroll(disable)
            for (u16 n = static_cast<u16>(amt); n != 0; --n) {
                if (++p >= *dlp) { ++dlp; ++ddp; p = 0; }
            }
        } else if (amt < 0) {
            #pragma clang loop unroll(disable)
            for (u16 n = static_cast<u16>(0 - static_cast<u16>(amt)); n != 0; --n) {
                if (p == 0) {
                    if (dlp == DynLengths) break;
                    --dlp; --ddp; p = *dlp;
                }
                --p;
            }
        }
        lp = dlp; dp = ddp; progress = p;
    }

    // AABB sweep over the dynamic plane.  Same column-major cell walk as
    // CollidesSolid, but returns the FIRST overlapped non-air dynamic tile (id +
    // owning run) instead of a bool, so the caller can dispatch a removal rule.
    DynHit CollidesDynamic(const DynamicCursor& origin, const u16 px, const u16 py, const u8 w, const u8 h) {
        const u16 rowTop = py >> 4;
        if (rowTop >= levelHeight) return {false, 0, 0};

        u16 rowBot = (py + h - 1) >> 4;
        if (rowBot >= levelHeight) rowBot = levelHeight - 1;

        const u8 cols = ((px + w - 1) >> 4) - (px >> 4);
        const u8 rows = static_cast<u8>(rowBot - rowTop);

        // ONE cursor, walked monotonically forward through the AABB cells in
        // column-major order -- NEVER reset to origin.  Within a column each row is a
        // single Move(1); crossing to the next column steps the remaining
        // (levelHeight - rows) cells.  Total RLE stepping is the last cell's offset
        // (~levelHeight for a 2-wide box), not the SUM of every cell's offset: the old
        // per-cell `probe = origin; Move(c*H + r)` re-walked the whole first column
        // from scratch for every second-column cell -- O(cols*levelHeight) of pure
        // redundant 16-bit-indexed stepping, the exact RLE walk the static plane was
        // moved off of.
        DynamicCursor probe = origin;
        for (u8 c = 0; ; c++) {
            for (u8 r = 0; r <= rows; r++) {
                if (const u8 id = probe.Fetch(); id != 0)
                    return {true, id, probe.Run()};
                if (r != rows) probe.Move(1);                // next row, same column
            }
            if (c == cols) break;
            probe.Move(static_cast<i16>(levelHeight - rows)); // cross to next column, row 0
        }
        return {false, 0, 0};
    }

    // ---------------------------------------------------------------------------
    // Removal jump table.  Each dynamic metatile id maps to a rule run when the tile
    // is consumed.  File-local (internal linkage) so the constexpr table folds at
    // build time with no global symbol; RemoveDynamic below is the public, non-inline
    // trampoline into it.  Mirrors the Actor plain-function-pointer dispatch style.
    // ---------------------------------------------------------------------------
    namespace {

        using DynRule = void (*)(const DynHit&, Cursor);

        // Default: tile has no removal behavior (shouldn't be reached for air, which
        // CollidesDynamic filters, but keeps the table total).
        void RuleNop(const DynHit&, Cursor) {}

        // Blank the run in RAM (so the tile stops colliding / rendering) and reveal what
        // the static plane holds underneath -- avoiding the SMB1 "coin on a wall becomes
        // air" artifact.  `under` is a static cursor already parked on the same cell.
        void RuleReveal(const DynHit& self, const Cursor &under) {
            DynData[self.run] = 0;            // consume: this run is now air
            const u8 beneath = under.Fetch(); // static metatile to expose
            (void)beneath;                    // STUB: queue a nametable patch of `beneath`
        }

        // Coin: reveal-underneath, plus the game-feel response.
        void RuleCoin(const DynHit& self, Cursor under) {
            RuleReveal(self, under);
            // STUB: score++, coin SFX, HUD refresh.
        }

        constexpr u8 DYN_COIN = 0x62;   // coin metatile id (see metatiles.cpp)

        constexpr std::array<DynRule, 256> BuildDynRules() {
            std::array<DynRule, 256> t{};
            for (auto& e : t) e = &RuleNop;
            t[DYN_COIN] = &RuleCoin;
            return t;
        }

        constexpr std::array<DynRule, 256> DynRemovalRules = BuildDynRules();

    }   // namespace

    // Public dispatch trampoline: look the hit's id up in the jump table and run its
    // rule.  Non-inline so the internal-linkage table never leaks into a header.
    void RemoveDynamic(const DynHit& h, const Cursor &under) {
        DynRemovalRules[h.id](h, under);
    }

    // ROM->RAM load for a level's dynamic plane.  STUB: uncalled until a level ships
    // a dynamic layer -- LoadLevel will call this with the level's dynamic lengths /
    // data (or nullptr / 0 for a level that has none).
    void LoadDynamicLayer(const u8* dynLengthsROM, const u8* dynDataROM, u16 runCount) {
        DynLengths = dynLengthsROM;
        DynDataROM = dynDataROM;
        for (u16 i = 0; i < runCount; ++i)
            DynData[i] = dynDataROM ? dynDataROM[i] : 0;
    }

}   // namespace level
