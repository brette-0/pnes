#include "collision_map.hpp"

#include "levels.hpp"                  // levelHeight, kHudRows
#include "../../graphics/metatiles.hpp"   // GetMetatileCollisions, MetatileCollision
#include "../../banks.hpp"             // FIXED, LEVEL_CODE
#include "../../raster_band.hpp"      // demo::RasterBand (profiling aid, off by default)

#include <array>

using namespace br0::intsh;

namespace level {

// Composite-metatile window. ViewMap[slot*levelHeight + row] holds the metatile
// for that cell (dynamic-over-static). ColMapBaseCol is the leftmost world
// column held; ColMapOrigin is its ring slot. slot = (origin + (col-base)) wrapped.
// Pinned to absolute .bss (NES): page zero is at the cliff over FamiStudio's own
// ZEROPAGE segment, and none of this state is hot enough to spend scarce ZP on.
#ifdef TARGET_NES
#define colmap_cold __attribute__((section(".bss")))
#else
#define colmap_cold
#endif

colmap_cold u8  ViewMap[kColMapWidth * levelHeight];
colmap_cold u16 ColMapBaseCol;
colmap_cold u8  ColMapOrigin;

// Precomputed slot -> ViewMap column-base offsets: slot*levelHeight would need a
// 16-bit software multiply on 6502 (322 > 255). One indexed load instead, ~6
// cycles vs ~80-100.
static constexpr std::array<u16, kColMapWidth> kSlotOffset = []() constexpr {
    std::array<u16, kColMapWidth> a{};
    for (u8 i = 0; i < kColMapWidth; i++) a[i] = static_cast<u16>(i * levelHeight);
    return a;
}();

// Producer edge cursors, internal to this TU. *Left sits on the row-0 cell of the
// leftmost held column, *Right on the rightmost, for both planes. ColMapTrack
// walks all four in lockstep with the window's base, so a slide only hops one
// edge one column -- no walk back from the level start.
namespace {
    colmap_cold Cursor        colLeftStat;
    colmap_cold Cursor        colRightStat;
    colmap_cold DynamicCursor colLeftDyn;
    colmap_cold DynamicCursor colRightDyn;

    // The collector's PERSISTENT cursor pair. Unlike the edge cursors above (which
    // track the window), this tracks the PLAYER: parked on the AABB's top-left cell,
    // nudged only by per-frame travel, seeded once at load and never rebuilt -- so a
    // collected coin costs a tiny AABB-local delta, not a walk from the window edge.
    // dyn indexes DynData to blank a coin; stat reveals the metatile underneath.
    // cell is the delta reference for re-anchoring; colR/rowB are the last AABB's
    // edges for the early-exit guard. dry is set only once the CURRENT AABB has
    // scanned empty -- guards against skipping a coin left by the one-coin-per-call
    // cap (see CollectCoinsImpl) before it's ever seen again.
    //
    // One struct, two instances (P1/P2): CollectCoins/CollectCoins2 used to be
    // ~2.3 KiB apiece of near-identical logic -- see CollectCoinsImpl.
    struct CollectorAnchor {
        Cursor        stat;
        DynamicCursor dyn;
        u16           cell;
        u16           colR;
        u8            rowB;
        bool          dry;
    };
    colmap_cold CollectorAnchor colPlayerAnchor;
    // Independent anchor for player 2, same invariants -- avoids bouncing a
    // shared cursor across the inter-player distance every frame.
    colmap_cold CollectorAnchor colPlayer2Anchor;

    // Highest base column the window may hold: nColumns - ColMapWidth(), floored
    // at 0 for a level narrower than the window. Derived from nColumns, which
    // LoadLevel sets once and nothing changes afterwards -- so it is constant for
    // the whole level, and ColMapTrack was recomputing it (a 16-bit subtract, a
    // sign test and a conditional zero) every single frame to reach the same
    // answer. Seeded in ColMapSeed, which runs after LoadLevel at level entry.
    colmap_cold u16 ColMapMaxBase;
}

// The one shared pick slot -- see collision_map.hpp for why this is not a
// caller-supplied out-pointer.
colmap_cold CoinPick coinPick;

// Stamp one column's composite metatiles into ring slot `slot`. Walks stat/dyn
// down levelHeight cells in lockstep, dynamic-over-static -- same compositing
// as GetNextMetaTile. Cursor copies are consumed (passed by value).
// NI: reached from THREE sites; inlining duplicated the walk and blew up PRG.
// unroll(disable): rolled form is smaller, per-iter cost is negligible next to
// the dyn-cursor RLE work.
// LEVEL_CODE, window 1 -- stat/dyn read out of window 2, whose bank is
// LoadLevel's runtime choice (banks.hpp's ::LevelDataBank).
NI LEVEL_CODE void ColMapStamp(const u8 slot, Cursor stat, DynamicCursor dyn) {
    u8* col = ViewMap + kSlotOffset[slot];

    // Static is a flat ROM pointer; dynamic still walks RLE.
    const u8* sdat = stat.dp;
    const u8* ddat = dyn.dp;
    const u8* dlen = dyn.lp;
    u8        dp   = dyn.progress;

    // ~90 cycles per cell, and ~36 of that is register shuffling, not memory:
    // the loop touches four pointers, the 6502 has one register that can index
    // `(zp),y`, so Y alternates between 0 (for *ddat / *sdat / *dlen) and the
    // row (for col[r]) on every cell, each alternation a txa/tay pair with
    // pha/pla around it to save the value in flight.
    //
    // Two source-level fixes were tried (2026-10-24) and BOTH failed; do not
    // re-try them. Walking `col` as a pointer (`*col++`) gave a BYTE-IDENTICAL
    // function -- LLVM strength-reduces it straight back to the indexed form.
    // Replacing the counter with a pointer-compare loop (`while (col != end)`)
    // was WORSE: 144 bytes and 17 shuffle ops against 142 and 13. The shuffling
    // is llvm-mos's register allocator, not the way this is written, so moving
    // it needs asm or a different data layout.
    //
    // Note what is NOT the problem, because the obvious reading is wrong and an
    // earlier analysis acted on it: `*dlen` and `*sdat` read banked PRG-ROM,
    // but window 2 is pinned to the level's bank for the whole session
    // (levels.cpp), and a read from MAPPED ROM costs a 6502 exactly what a read
    // from RAM costs. There is no "windowed fetch" penalty per cell. Copying
    // DynLengths into RAM would buy nothing and spend 512 bytes of it.
    #pragma clang loop unroll(disable)
    for (u8 r = 0; r < levelHeight; r++) {
        const u8 d = *ddat;
        col[r] = d ? d : *sdat;                 // dynamic-over-static
        ++sdat;                                  // flat: one byte per cell
        if (++dp >= *dlen) { ++dlen; ++ddat; dp = 0; }
    }
}

// Seed the whole window from a single static+dynamic Cursor parked on (leftCol,
// row 0). Origin starts at 0 so column (leftCol+i) lands in slot i.
// LEVEL_CODE: called from EnterLevelSetup (::COLD) via an explicit
// mmc3::CallInBlock<level_code_tag> -- see that call site in level.cpp.
// NI: sole call site is that same lambda, so without it LTO inlines the whole
// body into the FIXED trampoline -- same failure mode as LoadLevel's NI.
NI LEVEL_CODE void ColMapSeed(const u16 leftCol, Cursor stat, DynamicCursor dyn) {
    ColMapBaseCol = leftCol;
    ColMapOrigin  = 0;
    ColMapMaxBase = nColumns > ColMapWidth()
                  ? static_cast<u16>(nColumns - ColMapWidth()) : 0;
    colLeftStat   = stat;          // leftmost held column (base), row 0
    colLeftDyn    = dyn;
    colPlayerAnchor.stat = stat;   // collector anchor: parked on (leftCol, row 0) now;
    colPlayerAnchor.dyn  = dyn;    //   the first CollectCoins re-anchors it to the player
    colPlayerAnchor.cell = static_cast<u16>(leftCol) * levelHeight;
    colPlayerAnchor.colR = leftCol;
    colPlayerAnchor.rowB = 0;
    colPlayerAnchor.dry  = false;  // unverified: first call for this AABB must scan
    colPlayer2Anchor = colPlayerAnchor;   // P2 anchor seeded identically; diverges as P2 moves
    const auto width = ColMapWidth();
    for (u8 i = 0; i < width; i++) {
        if (i == width - 1) {          // rightmost held, row 0
            colRightStat = stat;
            colRightDyn  = dyn;
        }
        ColMapStamp(i, stat, dyn);     // stamps column i into slot i (copies consumed)
        stat.Move(levelHeight);        // advance masters to the next column's row 0
        dyn.Move(levelHeight);
    }
}

// Pair advance helpers: static cursor is a flat pointer (just add/subtract
// levelHeight); only the dynamic cursor still needs RLE walking.
// always_inline: both callers are in this TU; the compiler sees the concrete
// global addresses and emits absolute addressing instead of the indirect ZP
// addressing that noinline + reference parameters forced.
AI
static void AdvancePairForward(Cursor& s, DynamicCursor& d) {
    s.dp += levelHeight;
    const u8* dlp  = d.lp;  u8* ddp = d.dp;  u8 dprog = d.progress;
    #pragma clang loop unroll(disable)
    for (u8 i = levelHeight; i != 0; --i) {
        if (++dprog >= *dlp) { ++dlp; ++ddp; dprog = 0; }
    }
    d.lp = dlp; d.dp = ddp; d.progress = dprog;
}

AI
static void AdvancePairBackward(Cursor& s, DynamicCursor& d) {
    s.dp -= levelHeight;
    const u8* dlp  = d.lp;  u8* ddp = d.dp;  u8 dprog = d.progress;
    #pragma clang loop unroll(disable)
    for (u8 i = levelHeight; i != 0; --i) {
        if (dprog == 0) {
            if (dlp == DynLengths) break;
            --dlp; --ddp; dprog = *dlp;
        }
        --dprog;
    }
    d.lp = dlp; d.dp = ddp; d.progress = dprog;
}

// The slide itself. Split out of ColMapTrack so the common frame does not pay
// for it: inlining AdvancePairForward/Backward and ColMapSlideRight/Left here
// costs a 3-byte soft-stack frame, 5 pushes and 3 register spills, and leaving
// that in ColMapTrack meant paying the whole prologue/epilogue on EVERY frame
// to then decide not to slide at all -- ~75 cycles a frame for nothing, on the
// ~169 frames in 175 that hold still. Now it is paid only when the window
// actually moves.
// NI: keep this frame out of ColMapTrack's (and out of the giant PlayerUpdate ZP
// frame, which is what the original note here was guarding against).
NI LEVEL_CODE static void ColMapSlideTo(const u16 desired) {
    u8 colIter = 0;
    while (ColMapBaseCol < desired) {         // window slides right
        demo::RasterBand(static_cast<u8>((++colIter << 5) & 0xE0));
        AdvancePairForward(colRightStat, colRightDyn);    // entering column
        ColMapSlideRight(colRightStat, colRightDyn);      // base++
        AdvancePairForward(colLeftStat, colLeftDyn);      // re-anchor left edge
    }
    while (ColMapBaseCol > desired) {          // window slides left
        demo::RasterBand(static_cast<u8>((++colIter << 5) & 0xE0));
        AdvancePairBackward(colLeftStat, colLeftDyn);     // entering column
        ColMapSlideLeft(colLeftStat, colLeftDyn);         // base--
        AdvancePairBackward(colRightStat, colRightDyn);   // re-anchor right edge
    }
    demo::RasterBand(0);
}

// Lazily centre the window on the camera. Desired base = camLeftCol - 4, clamped
// to the level. Steady scroll does 0 or 1 slides/call, so this is written to be
// as close to free as possible on the no-slide frame: the whole body is two
// compares and a branch, which is why it carries no soft-stack frame at all --
// see ColMapSlideTo for what used to be inlined here.
//
// All UNSIGNED. This used to run in i16 because `camLeftCol - 4` can go
// negative, but only the first guard below needs that: after clamping, `desired`
// and ColMapBaseCol are both non-negative, so the loop tests were paying signed
// 16-bit compares, and the sign-extending casts feeding them, for a range that
// cannot be negative.
// LEVEL_CODE: called only from level::main()'s loop.
NI LEVEL_CODE void ColMapTrack(const u16 camLeftCol) {
    u16 desired = camLeftCol > 4 ? static_cast<u16>(camLeftCol - 4) : 0;
    if (desired > ColMapMaxBase) desired = ColMapMaxBase;   // hoisted, see its decl
    if (ColMapBaseCol != desired) ColMapSlideTo(desired);
}

// Slide the window right: the dropped leftmost slot (ColMapOrigin) is reused
// for the admitted rightmost column. Stamp it, advance base, rotate origin.
// LEVEL_CODE: called only from ColMapTrack.
LEVEL_CODE void ColMapSlideRight(const Cursor &newRightStat, DynamicCursor newRightDyn) {
    ColMapStamp(ColMapOrigin, newRightStat, newRightDyn);
    ColMapBaseCol++;
    if (++ColMapOrigin >= ColMapWidth()) ColMapOrigin = 0;
}

// Mirror: drop the rightmost column, admit (base-1) on the left. Rotate origin
// back onto the freed slot, drop base, stamp the new leftmost column.
// LEVEL_CODE: called only from ColMapTrack.
LEVEL_CODE void ColMapSlideLeft(const Cursor &newLeftStat, DynamicCursor newLeftDyn) {
    ColMapBaseCol--;
    if (ColMapOrigin == 0) ColMapOrigin = ColMapWidth() - 1;
    else                   ColMapOrigin--;
    ColMapStamp(ColMapOrigin, newLeftStat, newLeftDyn);
}

// Resolve world column `col` to its levelHeight metatiles in the ring, or nullptr
// if outside the window. Same slot math as Blocked's inner resolve; shared so the
// render path reads the SAME decompressed column the window already produced.
// FIXED: sole callers are BuildNextColumn/BuildPrevColumn (::ACTORS) -- grouped
// with sibling Blocked (same static state, already FIXED) so both actor-bank and
// level-domain callers reach it by ordinary call.
FIXED const u8* ColMapColumn(const u16 col) {
    const u16 d = col - ColMapBaseCol;
    if (d >= ColMapWidth()) return nullptr;       // outside window = air (caller skips)
    u8 slot = ColMapOrigin + static_cast<u8>(d);
    if (slot >= ColMapWidth()) slot -= ColMapWidth();
    return ViewMap + kSlotOffset[slot];
}

// AABB block test on world-pixel inputs. HUD strip removed so the row index lands
// in level space. Resolves the ring slot ONCE per swept column, then the inner row
// loop is a flat index + one-mask decode -- no RLE. `collectBlocks` picks the
// audience: the player (true) bonks on coins too, enemies (false) only stop on
// walls. Columns outside the window read as air, no clamp needed.
// NI: called 4x from Player::Update plus Actor::Move; LTO was duplicating the
// whole body into every call site instead of one JSR target.
// FIXED: 402 bytes standalone, always mapped whenever level code runs.
NI FIXED bool Blocked(const u16 px, const u16 py, const vec2<u8> dimensions, const bool collectBlocks) {
    const u8 w = dimensions.x; const u8 h = dimensions.y;
    i16 rowTop = static_cast<i16>(py >> 4) - kHudRows;
    i16 rowBot = static_cast<i16>((py + h - 1) >> 4) - kHudRows;
    if (rowBot < 0 || rowTop >= levelHeight) return false;   // AABB entirely off-field
    if (rowTop < 0)            rowTop = 0;
    if (rowBot >= levelHeight) rowBot = levelHeight - 1;

    // Clamp to u8 now that bounds are validated -- eliminates i16 indexing below.
    const u8 rowT = static_cast<u8>(rowTop);
    const u8 rowB = static_cast<u8>(rowBot);

    const u16 colL = px >> 4;
    const u16 colR = (px + w - 1) >> 4;
    // ViewMap (RAM) and the metatile planes (now PRG-RAM mirrors, see
    // metatiles.hpp's LoadMetatilePlanes) are both ambient -- no window-2
    // switch needed here anymore.
    for (u16 c = colL; c <= colR; c++) {
        const u16 d = c - ColMapBaseCol;
        if (d >= ColMapWidth()) continue;       // column outside window = air
        u8 slot = ColMapOrigin + static_cast<u8>(d);
        if (slot >= ColMapWidth()) slot -= ColMapWidth();   // non-pow2 ring: one wrap
        // kSlotOffset replaces slot*levelHeight (16-bit software multiply) with a
        // ROM table lookup.  Start colp at rowT so the inner walk is a bare *colp.
        const u8* colp = ViewMap + kSlotOffset[slot] + rowT;
        for (u8 cnt = rowB - rowT + 1; cnt != 0; --cnt, ++colp) {
            const MetatileCollision cls = GetMetatileCollisions(*colp);
            if (cls == MetatileCollision::Solid) return true;
            if (collectBlocks && cls == MetatileCollision::Collect) return true;
        }
    }
    return false;
}

// Collect coins overlapping the AABB. Same column/row projection as Blocked. Uses
// a PERSISTENT collector pair (anchor.dyn/anchor.stat) parked on the player instead
// of walking from the window's left edge every call, so cost doesn't grow with the
// player's depth into the window. Each call re-anchors to the AABB's top-left cell
// with a single Move (<= ~1 cell of travel), then walks ONE probe pair monotonically
// through the AABB in column-major order. Blank the run in DynData (permanent) and
// overwrite the window cell with the revealed static tile (immediate).
//
// Collects AT MOST ONE coin per call, even if the AABB straddles several. Bounds
// both the per-frame VRAM-queue burst (PushCoinVram queues 2 paired records, i.e.
// 4 tile writes, per coin) and the CPU burst the same way. Spreading a multi-coin cluster's collection over a few
// frames as the player passes through is not perceptible.
//
// The "same cell as last frame -> skip the walk" guard below is a POSITION check,
// which is unsafe alone under the one-coin cap: a cell is 16px wide and the player
// moves ~2.5px/frame, so "same cell" holds for many frames just from walking
// through it. If a coin was left behind when the cap kicked in, a position-only
// guard would skip re-scanning until the player leaves the AABB ("phase through").
// anchor.dry closes that gap: the guard may only skip once this cell is actually
// VERIFIED empty, so a stationary AABB keeps getting re-scanned until drained.
//
// Shared body for CollectCoins/CollectCoins2: the two used to be independent
// ~2.3 KiB copies differing only in which CollectorAnchor they touched.
// NI FIXED: same rationale as Blocked above.
NI FIXED static bool CollectCoinsImpl(CollectorAnchor& anchor, const u16 px, const u16 py,
                            const vec2<u8> dimensions) {
    const u8 w = dimensions.x; const u8 h = dimensions.y;
    i16 sTop = static_cast<i16>(py >> 4) - kHudRows;
    i16 sBot = static_cast<i16>((py + h - 1) >> 4) - kHudRows;
    if (sBot < 0 || sTop >= levelHeight) return false;
    if (sTop < 0)            sTop = 0;
    if (sBot >= levelHeight) sBot = levelHeight - 1;
    // Signed only until clamped. Past this point both bounds are in
    // [0, levelHeight), so they -- and every row loop counter derived from them --
    // are u8: an i16 row index costs a 16-bit compare and increment per cell in
    // both AABB walks below, and this function's cost is dominated by what it
    // pays per cell.
    const u8 rowTop = static_cast<u8>(sTop);
    const u8 rowBot = static_cast<u8>(sBot);

    const u16 colL = px >> 4;
    const u16 colR = (px + w - 1) >> 4;

    // Re-anchor the persistent collector pair EVERY frame -- step is the player's
    // travel since last frame, a tiny RLE nudge. Must stay unconditional: gating
    // it on "a coin is present" lets anchor.cell freeze and the next collection
    // pays the whole accumulated drift in one spike (tried, and it spiked).
    const u16 anchorCell = static_cast<u16>(colL) * levelHeight + rowTop;
    const i16 step      = static_cast<i16>(anchorCell) - static_cast<i16>(anchor.cell);
    const bool sameColR = (colR == anchor.colR);
    const bool sameRowB = (rowBot == anchor.rowB);
    anchor.cell = anchorCell;
    anchor.colR = colR;
    anchor.rowB = rowBot;
    // Same cells as last frame AND already verified empty -> skip the walk. Guards
    // all four AABB edges since colR/rowB can change independently of the anchor.
    // anchor.dry is what keeps this safe under the one-coin cap (see file comment).
    if (step == 0 && sameColR && sameRowB && anchor.dry) return false;
    // After level load step is bounded to ±(levelHeight+1) ≤ 15; use the i8 path
    // (single-byte loop counter on 6502) except on the first call after ColMapSeed
    // where the player may be far from the seed point.
    if (step >= -128 && step <= 127) {
        anchor.dyn.Move(static_cast<i8>(step));
        anchor.stat.Move(static_cast<i8>(step));
    } else {
        anchor.dyn.Seek(step);
        anchor.stat.Seek(step);
    }

    // ViewMap pre-scan: a cheap gate before the cursor probe walk. Fires for static
    // Collect tiles too (ViewMap is composite), but the probe's pd.Fetch()!=0 guard
    // rejects those. Skips the probe walk entirely on the common no-coin frame.
    {
        // Same reasoning as Blocked(): ViewMap and the metatile planes are
        // both ambient RAM now, no window-2 switch needed.
        bool anyCoin = false;
        for (u16 c = colL; !anyCoin && c <= colR; c++) {
            const u16 d = c - ColMapBaseCol;
            if (d >= ColMapWidth()) continue;
            u8 slot = ColMapOrigin + static_cast<u8>(d);
            if (slot >= ColMapWidth()) slot -= ColMapWidth();
            const u8* colp = ViewMap + kSlotOffset[slot];
            for (u8 r = rowTop; !anyCoin && r <= rowBot; r++) {
                if (GetMetatileCollisions(colp[r]) == MetatileCollision::Collect)
                    anyCoin = true;
            }
        }
        if (!anyCoin) { anchor.dry = true; return false; }   // verified empty: safe to skip next time
    }

    // ONE probe pair copied off the anchor, walked monotonically column-major through
    // the AABB -- no per-coin re-walk (that quadratic cost is the thing being avoided).
    // Stops at the FIRST coin found -- see the one-coin-per-call note above.
    DynamicCursor pd   = anchor.dyn;
    Cursor        ps   = anchor.stat;
    // Cells to step to reach the next column's top row: one full column minus the
    // rows the walk already descended. Hoisted and narrowed to u8 -- it was a
    // 16-bit `levelHeight - rows` recomputed at every column boundary.
    const u8      cross = static_cast<u8>(levelHeight - (rowBot - rowTop));
    bool got = false;
    for (u16 c = colL; ; c++) {
        const u16  d     = c - ColMapBaseCol;
        const bool inWin = d < ColMapWidth();   // column outside window = air
        u8* colp = nullptr;
        if (inWin) {
            u8 slot = ColMapOrigin + static_cast<u8>(d);
            if (slot >= ColMapWidth()) slot -= ColMapWidth();
            colp = ViewMap + kSlotOffset[slot];
        }
        for (u8 r = rowTop; r <= rowBot; r++) {
            // pd.Fetch() reads DynData and GetMetatileCollisions reads the
            // RAM metatile mirror -- both ambient, no window-2 switch.
            const bool isCollect = inWin && pd.Fetch() != 0 &&
                GetMetatileCollisions(colp[r]) == MetatileCollision::Collect;
            if (isCollect) {
                // *pd.dp = 0 is DynData[pd.Run()] = 0 (Run() is just dp-DynData)
                // without the round trip: pd.dp already points at that DynData
                // byte, so this skips a 16-bit subtract (Run()) immediately
                // followed by a 16-bit re-add (indexing back into DynData) --
                // 64 bytes -> 30 bytes measured in isolation.
                *pd.dp = 0;                     // permanent removal (length-1 coin run)
                const u8 reveal = ps.Fetch();   // static metatile to expose
                colp[r] = reveal;               // window now shows the static cell
                coinPick = { c, r, reveal };
                got = true;
                break;   // one coin per call -- see the comment above
            }
            if (r != rowBot) { pd.Move(1); ps.Move(1); }   // step down to the next row
        }
        if (got || c == colR) break;
        pd.Move(cross);                         // cross to the next column's top row
        ps.Move(cross);
    }
    // got: we deliberately stopped after the first coin, so this AABB may still hold
    // another one -- keep re-scanning next frame even if the player doesn't move.
    // !got here would mean anyCoin lied (shouldn't happen); treat as verified-empty
    // either way so a stuck AABB can't spin forever re-scanning something that isn't there.
    anchor.dry = !got;
    return got;
}

// Thin wrappers selecting which player's persistent anchor to use -- see
// CollectCoinsImpl's own comment for why the body lives there and not here.
FIXED bool CollectCoins(const u16 px, const u16 py, const vec2<u8> dimensions) {
    return CollectCoinsImpl(colPlayerAnchor, px, py, dimensions);
}

FIXED bool CollectCoins2(const u16 px, const u16 py, const vec2<u8> dimensions) {
    return CollectCoinsImpl(colPlayer2Anchor, px, py, dimensions);
}

}   // namespace level
