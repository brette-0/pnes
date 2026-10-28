#include "collision_map.hpp"

#include "levels.hpp"                  // levelHeight, kHudRows
#include "../../graphics/metatiles.hpp"   // GetMetatileCollisions, MetatileCollision
#include "../../banks.hpp"             // FIXED, LEVEL_CODE

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

// Row -> single-row mask bit, one table per plane. A `1 << r` on 6502 is a shift
// LOOP (no variable shift instruction), so every mask build and every mask clear
// would pay ~8 iterations; this is one indexed load.
static constexpr std::array<u8, levelHeight> kBitLo = []() constexpr {
    std::array<u8, levelHeight> a{};
    for (u8 r = 0; r < levelHeight; r++) a[r] = r < 8 ? static_cast<u8>(1u << r) : 0;
    return a;
}();
static constexpr std::array<u8, levelHeight> kBitHi = []() constexpr {
    std::array<u8, levelHeight> a{};
    for (u8 r = 0; r < levelHeight; r++) a[r] = r >= 8 ? static_cast<u8>(1u << (r - 8)) : 0;
    return a;
}();

// Prefix masks: kPref*[k] has bits 0..k-1 set, so the mask for the row RANGE
// [t, b] is kPref[b + 1] ^ kPref[t]. Four indexed loads and two eors, general
// for any row count, 30 bytes of ROM -- versus a per-row loop to OR kBit
// together, or a 2D span table. This is what turns an AABB's row extent into a
// constant-time query.
static constexpr std::array<u8, levelHeight + 1> kPrefLo = []() constexpr {
    std::array<u8, levelHeight + 1> a{};
    for (u8 k = 0; k <= levelHeight; k++)
        a[k] = k >= 8 ? 0xFF : static_cast<u8>((1u << k) - 1u);
    return a;
}();
static constexpr std::array<u8, levelHeight + 1> kPrefHi = []() constexpr {
    std::array<u8, levelHeight + 1> a{};
    for (u8 k = 0; k <= levelHeight; k++)
        a[k] = k <= 8 ? 0 : static_cast<u8>((1u << (k - 8)) - 1u);
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

    // ---- Per-slot derived state, rebuilt on column entry -------------------
    //
    // The window already removed the RLE from collision. These remove the
    // PER-CELL LOOP and the collector's cursors, by deriving everything a query
    // wants ONCE per column entry instead of once per query.
    //
    // There used to be a persistent per-player cursor pair here (CollectorAnchor)
    // parked on the player's AABB and nudged every frame. It was the collector's
    // whole cost: `Cursor::Move` is a pointer add and free, but DynamicCursor::Move
    // walks the RLE at ~24 cycles a step, the nudge had to stay unconditional (a
    // gated one lets the delta accumulate and spike later -- tried, and it spiked),
    // and crossing a metatile column makes the delta levelHeight cells, so a
    // running player paid ~340 cycles EVERY sixth frame, per player, plus a
    // 16-bit multiply-by-levelHeight to compute the delta reference at all.
    // Deriving per column instead makes that cost proportional to column entries
    // (one per 16 pixels of scroll) rather than to player frames, and the walk
    // that remains is bounded by levelHeight and only runs on a frame a coin is
    // actually taken.

    // Collision-class occupancy, one bit per row: bit r set means row r of that
    // column holds a cell of that class. levelHeight(=14) rows, held as two
    // parallel u8 planes rather than one u16 array -- a u8 array indexed by slot
    // is `lda abs,x`, where a u16 array needs the index doubled first. Same
    // reason kSlotOffset exists instead of a multiply.
    colmap_cold u8 ColSolidLo[kColMapWidth];
    colmap_cold u8 ColSolidHi[kColMapWidth];
    colmap_cold u8 ColCollLo[kColMapWidth];
    colmap_cold u8 ColCollHi[kColMapWidth];

    // The column's flat static bytes. The static plane is column-major and
    // uncompressed, so a whole static Cursor collapses to this one pointer and
    // the metatile revealed under a collected coin is ColStatBase[slot][row] --
    // no cursor, no walk.
    colmap_cold const u8* ColStatBase[kColMapWidth];

    // The DYNAMIC cursor's state at the column's row 0. The dynamic plane is RLE,
    // so unlike the static plane a row cannot be indexed -- it has to be walked.
    // Storing the column head and walking down on demand confines that walk to
    // <= levelHeight steps on a take frame. Split into parallel arrays for the
    // same indexing reason as the masks: a 5-byte DynamicCursor array would need
    // a multiply by 5 to index.
    colmap_cold const u8* ColRunLen[kColMapWidth];    // -> DynLengths
    colmap_cold u8*       ColRunData[kColMapWidth];   // -> DynData
    colmap_cold u8        ColRunProg[kColMapWidth];

    // Highest base column the window may hold: nColumns - ColMapWidth(), floored
    // at 0 for a level narrower than the window. Derived from nColumns, which
    // LoadLevel sets once and nothing changes afterwards -- so it is constant for
    // the whole level, and ColMapTrack was recomputing it (a 16-bit subtract, a
    // sign test and a conditional zero) every single frame to reach the same
    // answer. Seeded in ColMapSeed, which runs after LoadLevel at level entry.
    colmap_cold u16 ColMapMaxBase;

    // Memoized column resolution for Blocked.
    //
    // Resolving a world column to its ViewMap base is the expensive half of a
    // Blocked call: a 16-bit subtract against ColMapBaseCol, a window test, the
    // non-pow2 ring wrap, a 16-bit kSlotOffset load and a 16-bit add -- ~58
    // cycles, twice, for the two columns a 16px AABB spans. Blocked runs up to
    // three times per player per frame on positions that differ by one pixel,
    // so it was resolving the SAME column pair two or three times over.
    //
    // Keyed on the resolved column PAIR rather than on px: the pair is what the
    // bases depend on, so the memo stays correct for any AABB width, and the
    // hit test is two 16-bit compares instead of the slot math.
    //
    // The bases depend on ColMapBaseCol/ColMapOrigin, so the slide paths
    // invalidate (see ColMapSlideRight/Left and ColMapSeed). Nothing else moves
    // the window.
    colmap_cold u16  colMemoL;
    colmap_cold u16  colMemoR;
    colmap_cold u8   colMemoA;   // slot for colMemoL; kNoSlot = outside window = air
    colmap_cold u8   colMemoB;   // slot for colMemoR; == colMemoA when single-column
    colmap_cold bool colMemoValid;

    // The memo caches the resolved SLOT rather than a ViewMap base pointer now:
    // every per-slot array above is indexed by it, so a pointer would have to be
    // turned back into an index to reach them.
    constexpr u8 kNoSlot = 0xFF;
}

// The one shared pick slot -- see collision_map.hpp for why this is not a
// caller-supplied out-pointer.
colmap_cold CoinPick coinPick;

// Caller-side collector gates, one per player -- see CoinGateSkips.
colmap_cold CoinGate coinGate1;
colmap_cold CoinGate coinGate2;

// CollectCoinsImpl's three outcomes.  It no longer returns a bare bool because
// the wrappers need to distinguish "scanned, nothing there" (the window is
// verified empty, so the gate may skip next frame) from "never got far enough
// to look" (off-field: the gate must NOT skip).
enum : u8 { kScanOffField = 0, kScanEmpty = 1, kScanTook = 2 };

// Rebuild slot `slot`'s collision occupancy planes from the composite bytes just
// stamped into it. Every Blocked / collector query then reads two bytes instead
// of decoding Metatiles_ATTR per cell, so the decode cost moves from "once per
// cell per query" to "once per cell per column entry".
//
// Deliberately a SECOND pass over the finished levelHeight bytes rather than
// extra work inside ColMapStamp's loop: that loop is already contended on four
// pointers against the 6502's single `(zp),y` index register (see its comment),
// and folding an ATTR decode plus two table loads into it would spill further.
// Here there is one pointer and three ROM tables, and it runs only on entry.
// AI: all three call sites are ColMapStamp's tail, i.e. one site after inlining,
// and the concrete ColSolid*/ColColl* globals fold to absolute addressing.
AI static void ColMapMasks(const u8 slot, const u8* col) {
    u8 sLo = 0, sHi = 0, cLo = 0, cHi = 0;
    #pragma clang loop unroll(disable)
    for (u8 r = 0; r < levelHeight; r++) {
        const MetatileCollision cls = GetMetatileCollisions(col[r]);
        if (cls == MetatileCollision::Solid) {
            sLo |= kBitLo[r]; sHi |= kBitHi[r];
        } else if (cls == MetatileCollision::Collect) {
            cLo |= kBitLo[r]; cHi |= kBitHi[r];
        }
    }
    ColSolidLo[slot] = sLo; ColSolidHi[slot] = sHi;
    ColCollLo[slot]  = cLo; ColCollHi[slot]  = cHi;
}

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

    // Capture both planes' row-0 anchors for this column BEFORE the walk consumes
    // the cursor copies. This is what lets the collector reach any row of this
    // column later without a cursor of its own -- see their declarations.
    ColStatBase[slot] = stat.dp;
    ColRunLen[slot]   = dyn.lp;
    ColRunData[slot]  = dyn.dp;
    ColRunProg[slot]  = dyn.progress;

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

    ColMapMasks(slot, col);
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
    // No collector cursors to seed any more: the collector reads whichever column
    // it needs out of the per-slot anchors ColMapStamp records below, so there is
    // no per-player state to park or to drift.
    // dry = false: unverified, so the first call for either player must scan.
    // px/py are not consulted while dry is false, so they need no real seed.
    coinGate1 = { 0, 0, false };
    coinGate2 = coinGate1;
    colMemoValid = false;   // whole window just changed under the memo
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
        AdvancePairForward(colRightStat, colRightDyn);    // entering column
        ColMapSlideRight(colRightStat, colRightDyn);      // base++
        AdvancePairForward(colLeftStat, colLeftDyn);      // re-anchor left edge
    }
    while (ColMapBaseCol > desired) {          // window slides left
        AdvancePairBackward(colLeftStat, colLeftDyn);     // entering column
        ColMapSlideLeft(colLeftStat, colLeftDyn);         // base--
        AdvancePairBackward(colRightStat, colRightDyn);   // re-anchor right edge
    }

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
    colMemoValid = false;   // bases are relative to the window -- see the memo's decl
}

// Mirror: drop the rightmost column, admit (base-1) on the left. Rotate origin
// back onto the freed slot, drop base, stamp the new leftmost column.
// LEVEL_CODE: called only from ColMapTrack.
LEVEL_CODE void ColMapSlideLeft(const Cursor &newLeftStat, DynamicCursor newLeftDyn) {
    ColMapBaseCol--;
    if (ColMapOrigin == 0) ColMapOrigin = ColMapWidth() - 1;
    else                   ColMapOrigin--;
    ColMapStamp(ColMapOrigin, newLeftStat, newLeftDyn);
    colMemoValid = false;   // bases are relative to the window -- see the memo's decl
}

// Resolve world column -> ring slot, or kNoSlot when outside the window. This is
// the one copy of the slot math: a 16-bit subtract against the base, a window
// test, and the non-pow2 ring's single conditional wrap. ColMapColumn (render
// path) and the collision/collector paths all go through it.
// AI: callers want the concrete globals folded to absolute addressing, and each
// one needs a different thing out of the result.
AI static u8 ColMapSlot(const u16 col) {
    const u16 d = col - ColMapBaseCol;
    if (d >= ColMapWidth()) return kNoSlot;       // outside window = air
    u8 slot = ColMapOrigin + static_cast<u8>(d);
    if (slot >= ColMapWidth()) slot -= ColMapWidth();
    return slot;
}

// Resolve the column PAIR an AABB spans into the memo, unless it is already the
// pair held. Keyed on the pair rather than on px, so it stays correct for any
// AABB width and the hit test is two 16-bit compares instead of the slot math.
// Blocked runs up to three times per player per frame on positions a pixel
// apart, and the collector asks about a pair too, so this is nearly always a hit.
AI static void ColMemoFor(const u16 colL, const u16 colR) {
    if (colMemoValid && colL == colMemoL && colR == colMemoR) return;
    colMemoL     = colL;
    colMemoR     = colR;
    colMemoA     = ColMapSlot(colL);
    colMemoB     = colR == colL ? colMemoA : ColMapSlot(colR);
    colMemoValid = true;
}

// Does any row in [mask] of this column hold a Solid / Collect cell? Constant
// time: two ANDs against the per-slot occupancy planes, independent of how many
// rows the AABB spans and with no Metatiles_ATTR decode -- that decode happened
// once per cell on column entry, in ColMapMasks.
// AI so ColSolid*/ColColl* stay concrete globals (absolute,x) rather than being
// passed as pointers, which would force an indirect ZP base in the hot path.
AI static bool SolidHits(const u8 slot, const u8 mLo, const u8 mHi) {
    if (slot == kNoSlot) return false;
    return ((ColSolidLo[slot] & mLo) | (ColSolidHi[slot] & mHi)) != 0;
}
AI static bool CollectHits(const u8 slot, const u8 mLo, const u8 mHi) {
    if (slot == kNoSlot) return false;
    return ((ColCollLo[slot] & mLo) | (ColCollHi[slot] & mHi)) != 0;
}

// Resolve world column `col` to its levelHeight metatiles in the ring, or nullptr
// if outside the window. Shared so the render path reads the SAME decompressed
// column the window already produced.
// FIXED: sole callers are BuildNextColumn/BuildPrevColumn (::ACTORS) -- grouped
// with sibling Blocked (same static state, already FIXED) so both actor-bank and
// level-domain callers reach it by ordinary call.
FIXED const u8* ColMapColumn(const u16 col) {
    const u8 slot = ColMapSlot(col);
    if (slot == kNoSlot) return nullptr;          // outside window = air (caller skips)
    return ViewMap + kSlotOffset[slot];
}

// AABB block test on world-pixel inputs. HUD strip removed so the row index lands
// in level space.
//
// There is NO per-cell loop left. The swept column pair resolves through the memo
// (two 16-bit compares on the common frame, because consecutive probes sit a pixel
// or two apart), the AABB's row extent becomes one 14-bit mask out of the kPref
// prefix tables, and the answer is an AND against each column's occupancy plane.
// Constant time in the AABB's height, and no Metatiles_ATTR decode -- that was
// paid once per cell when the column entered the window.
//
// `collectBlocks` picks the audience: a caller passing true bonks on coins too,
// false only stops on walls. Columns outside the window read as air, no clamp
// needed.
//
// PRECONDITION: w <= 17, so the AABB spans at most TWO metatile columns. Every
// caller passes GetSize(), which is {16,16} for both entries of
// ActorTypeMetadata (actor.hpp). A wider actor needs a general colL..colR loop
// restored, not merely a longer memo -- the two-slot form below would silently
// skip its middle columns.
// NI: called 3x from Player::Update; LTO was duplicating the whole body into
// every call site instead of one JSR target.
// FIXED: always mapped whenever level code runs.
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
    // ViewMap and the per-slot planes are plain RAM and ambient -- no window-2
    // switch needed here.
    ColMemoFor(colL, colR);

    // Rows [rowT, rowB] as one mask. See kPrefLo/kPrefHi.
    const u8 mLo = static_cast<u8>(kPrefLo[rowB + 1] ^ kPrefLo[rowT]);
    const u8 mHi = static_cast<u8>(kPrefHi[rowB + 1] ^ kPrefHi[rowT]);

    if (SolidHits(colMemoA, mLo, mHi)) return true;
    if (collectBlocks && CollectHits(colMemoA, mLo, mHi)) return true;
    if (colMemoB != colMemoA) {
        if (SolidHits(colMemoB, mLo, mHi)) return true;
        if (collectBlocks && CollectHits(colMemoB, mLo, mHi)) return true;
    }
    return false;
}

// Take one dynamic coin out of resolved column `slot`, over rows [rowT, rowB].
//
// Only called once the column's Collect occupancy plane says a Collect cell is in
// range, so the RLE walk below runs on a take-CANDIDATE frame rather than every
// frame -- which is the whole difference from the old persistent-anchor collector.
// The walk starts at the column's row-0 dynamic head (recorded by ColMapStamp) and
// steps down at most levelHeight times, in lockstep with the row scan, so a cell's
// owning run is reached without a cursor of the collector's own.
//
// The composite byte is re-tested per row rather than trusting the mask to name
// the row, because ViewMap is composite: a STATIC Collect-class tile sets the same
// bit, and only a non-air dynamic cell is a coin that can be taken. Finding one of
// those must not stop the scan, hence the loop rather than a single probe.
//
// NI FIXED: two call sites, and the body is large enough that inlining it would
// duplicate the whole walk; FIXED for the same reason as Blocked.
NI FIXED static bool TakeFromColumn(const u8 slot, const u8 rowT, const u8 rowB, const u16 col) {
    u8* const colp = ViewMap + kSlotOffset[slot];
    DynamicCursor pd{ ColRunLen[slot], ColRunData[slot], ColRunProg[slot] };
    if (rowT != 0) pd.Move(static_cast<i8>(rowT));

    for (u8 r = rowT; ; r++) {
        if (GetMetatileCollisions(colp[r]) == MetatileCollision::Collect && pd.Fetch() != 0) {
            // *pd.dp = 0 is DynData[pd.Run()] = 0 without the round trip: pd.dp
            // already points at that DynData byte, so this skips a 16-bit subtract
            // immediately followed by a 16-bit re-add to index back in.
            *pd.dp = 0;                              // permanent removal
            const u8 reveal = ColStatBase[slot][r];  // static plane is flat: direct index
            colp[r] = reveal;                        // window shows it gone immediately

            // The occupancy planes are DERIVED from these bytes, so they have to
            // follow: clear the coin's Collect bit and give the row the revealed
            // tile's class, or the next Blocked would still see a coin here.
            const u8 bLo = kBitLo[r], bHi = kBitHi[r];
            ColCollLo[slot] = static_cast<u8>(ColCollLo[slot] & ~bLo);
            ColCollHi[slot] = static_cast<u8>(ColCollHi[slot] & ~bHi);
            const MetatileCollision rc = GetMetatileCollisions(reveal);
            if (rc == MetatileCollision::Solid) {
                ColSolidLo[slot] |= bLo; ColSolidHi[slot] |= bHi;
            } else if (rc == MetatileCollision::Collect) {
                ColCollLo[slot] |= bLo; ColCollHi[slot] |= bHi;
            }

            coinPick = { col, r, reveal };
            return true;
        }
        if (r == rowB) return false;
        pd.Move(1);                                  // next row, same column
    }
}

// Collect one coin overlapping the AABB. Same column/row projection as Blocked,
// through the same memo.
//
// The common frame -- no coin in range -- is now two ANDs and a return, with no
// cursor touched and no cell visited: the Collect occupancy planes already know
// the answer. What used to stand here was a persistent per-player cursor pair
// nudged by the player's travel every single call, a 16-bit multiply-by-levelHeight
// to compute that nudge, and a per-cell ViewMap pre-scan that re-derived the slot
// math the memo had already done. See the per-slot state's declaration for why
// that cost was a spike rather than a constant.
//
// Collects AT MOST ONE coin per call, even if the AABB straddles several. Bounds
// both the per-frame VRAM-queue burst (PushCoinVram queues 2 paired records, i.e.
// 4 tile writes, per coin) and the CPU burst the same way. Spreading a multi-coin
// cluster's collection over a few frames as the player passes through is not
// perceptible.
//
// Both players share this outright now: there is no per-player state left, so
// CollectCoins/CollectCoins2 differ only in which gate they store.
// NI FIXED: same rationale as Blocked above.
NI FIXED static u8 CollectCoinsImpl(const u16 px, const u16 py, const vec2<u8> dimensions) {
    const u8 w = dimensions.x; const u8 h = dimensions.y;
    i16 sTop = static_cast<i16>(py >> 4) - kHudRows;
    i16 sBot = static_cast<i16>((py + h - 1) >> 4) - kHudRows;
    if (sBot < 0 || sTop >= levelHeight) return kScanOffField;
    if (sTop < 0)            sTop = 0;
    if (sBot >= levelHeight) sBot = levelHeight - 1;
    // Signed only until clamped. Past this point both bounds are in
    // [0, levelHeight), so they are u8.
    const u8 rowTop = static_cast<u8>(sTop);
    const u8 rowBot = static_cast<u8>(sBot);

    const u16 colL = px >> 4;
    const u16 colR = (px + w - 1) >> 4;
    ColMemoFor(colL, colR);

    const u8 mLo = static_cast<u8>(kPrefLo[rowBot + 1] ^ kPrefLo[rowTop]);
    const u8 mHi = static_cast<u8>(kPrefHi[rowBot + 1] ^ kPrefHi[rowTop]);

    if (CollectHits(colMemoA, mLo, mHi)
        && TakeFromColumn(colMemoA, rowTop, rowBot, colL)) return kScanTook;
    if (colMemoB != colMemoA && CollectHits(colMemoB, mLo, mHi)
        && TakeFromColumn(colMemoB, rowTop, rowBot, colR)) return kScanTook;

    // Nothing takeable in range. Reported verified-empty even when a Collect bit
    // was set but turned out to be a static tile: re-scanning it next frame would
    // find the same static tile forever. A coin that LANDS here later arrives by a
    // column entry or a removal, both of which rewrite the planes.
    return kScanEmpty;
}

// Thin wrappers owning each player's gate -- see CollectCoinsImpl for why the body
// lives there. The gate store lives out here rather than in the impl so the impl
// touches no per-player state at all.
//
// NI: these are thin enough that LTO inlines them into Player::Update, and then
// the gate store and outcome decode land in the ACTORS bank TWICE, once per
// player branch -- measured at +588 bytes there before this attribute went on.
// The gate TEST is what the caller needs inline (CoinGateSkips, which is AI);
// the store only runs on frames the call was already made, so it belongs out
// here next to the impl it pairs with.
NI FIXED bool CollectCoins(const u16 px, const u16 py, const vec2<u8> dimensions) {
    const u8 r = CollectCoinsImpl(px, py, dimensions);
    coinGate1 = { px, py, r == kScanEmpty };
    return r == kScanTook;
}

NI FIXED bool CollectCoins2(const u16 px, const u16 py, const vec2<u8> dimensions) {
    const u8 r = CollectCoinsImpl(px, py, dimensions);
    coinGate2 = { px, py, r == kScanEmpty };
    return r == kScanTook;
}

}   // namespace level
