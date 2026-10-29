/**
 * @file video.cpp
 * @brief Native 2D-hardware presentation backend for Nintendo DS + DSi (libnds).
 *
 * The NES PPU emulation is platform-agnostic and lives in src/emu/; this file is
 * only the DS-specific half, and -- unlike every other emulated-PPU backend -- it
 * does NOT rasterise on the CPU. The DS is a 2D tile console whose hardware is a
 * near-cousin of the NES PPU, so it hands the NES's own primitives to the DS 2D
 * core (Model B):
 *
 *   - The 8 KB CHR ROM is expanded from 2bpp to the DS's 4bpp tile format once at
 *     init and uploaded to both BG character VRAM and OBJ (sprite) VRAM. Each NES
 *     tile is one DS tile; tiles 0-255 are pattern table 0, 256-511 table 1.
 *   - paletteRAM becomes DS BGR555 palette RAM (4 BG sub-palettes + 4 sprite
 *     sub-palettes), rebuilt each frame. Pixel value 0 is transparent on both BG
 *     and OBJ: BG colour-0 shows the backdrop (BG_PALETTE[0]); sprite colour-0 is
 *     a hole -- reproducing NES transparency for free.
 *   - The nametable + attribute tables become a 512x512 DS text BG tilemap
 *     (two nametables wide, the BG's maximum height), rebuilt each frame from
 *     VideoRAM and uploaded to BOTH engines.
 *   - The OAM shadow becomes hardware OBJ via oamSet; "behind background" sprites
 *     get a lower OBJ priority than BG0 so opaque BG pixels occlude them.
 *
 * The frame is driven by ::emu::GenerateBands so the scanline IRQ handlers (the
 * game's per-frame logic, incl. the sprite-zero scroll split) still fire in
 * raster order. Each band's scroll is recorded into a per-scanline table that an
 * HBlank interrupt programs into REG_BG0HOFS/VOFS line by line -- this is how the
 * DS reproduces the NES's mid-frame raster scroll and the sprite-0 split on
 * native hardware, with no CPU per-pixel work.
 *
 * BOTH panels are driven. The viewport is their bounding box, 256x384 (see
 * video.hpp's TARGET_NDS branch), and each engine renders the viewport rows its
 * own panel covers -- main 0..191, sub 192..383 -- out of the same plane, same
 * map and same CHR, differing only in the scroll the HBlank ISR programs. The
 * two LCDs scan in lockstep off one REG_VCOUNT, so both engines are programmed
 * at the same line index from their own table (see sh_h/sh_v).
 *
 * A DS text BG is at most 512px tall, so of the grid's full 768px world only
 * the first 512px can be resident at once (band_emit's `& 0x1FF`). With the
 * demo's bottom-anchored title scroll of 48px the visible window is rows 6..53
 * of a 48-row plane, so its last 6 rows fall past the plane into the grid's
 * next nametable row -- blank on this board -- showing as an unused 48px band
 * at the very bottom of the lower panel.
 *
 * Frame pacing is the real hardware VBlank (swiWaitForVBlank), and the VRAM
 * writes (palette/map/OAM) are committed during VBlank to avoid tearing.
 */
#include "internal.hpp"

#include <pnes/video.hpp>
#include <pnes/interrupts.hpp>
#include "../emu/emu.hpp"

#include <nds.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The fixed NES master palette (ARGB8888, 0xAARRGGBB), shared verbatim with the
// software PPU in src/emu/ppu.cpp; here it only seeds the per-frame DS palettes.
// ---------------------------------------------------------------------------
static constexpr u32 nes_rgb[64] = {
    0xFF626262, 0xFF012090, 0xFF1B0CA4, 0xFF3B009E,
    0xFF520080, 0xFF5A004E, 0xFF521610, 0xFF3F2E00,
    0xFF234400, 0xFF0A5200, 0xFF005804, 0xFF004E30,
    0xFF003C62, 0xFF000000, 0xFF000000, 0xFF000000,
    0xFFABABAB, 0xFF1F56D8, 0xFF423CF2, 0xFF6E24EC,
    0xFF9218C4, 0xFF9E1A80, 0xFF933434, 0xFF7A5200,
    0xFF576E00, 0xFF2E8400, 0xFF118E0E, 0xFF008848,
    0xFF007898, 0xFF000000, 0xFF000000, 0xFF000000,
    0xFFFBFBFB, 0xFF6BA4FF, 0xFF8C88FF, 0xFFB87AFF,
    0xFFE072FF, 0xFFF076D0, 0xFFE88C78, 0xFFCCA830,
    0xFFA8C410, 0xFF7EDC24, 0xFF5AE84E, 0xFF48E490,
    0xFF48D4E0, 0xFF4E4E4E, 0xFF000000, 0xFF000000,
    0xFFFBFBFB, 0xFFBED4FF, 0xFFCACAFF, 0xFFDCC4FF,
    0xFFECC0FF, 0xFFF2C0EA, 0xFFF2C8C4, 0xFFE8D4A4,
    0xFFD8E09C, 0xFFC8EC9C, 0xFFBCF0AC, 0xFFB4F0CC,
    0xFFB4E8F0, 0xFFB8B8B8, 0xFF000000, 0xFF000000,
};

// ---------------------------------------------------------------------------
// DS layout constants. The viewport panel is 256x192; the BG covers the full
// 512x256 world (two horizontal NES nametables) and is scrolled over the panel.
// ---------------------------------------------------------------------------
// Scanlines ONE panel has. The viewport is both panels stacked (384px -- see
// video.hpp's TARGET_NDS branch), but a DS's two LCDs are scanned
// SIMULTANEOUSLY off the one REG_VCOUNT, 0..191 for both at once; there is no
// single 384-line raster. So every per-scanline table here is indexed by
// [panel][panel row], and both engines are programmed at the same VCOUNT.
static constexpr int SCREEN_H   = video::panel_rect(0).h;   // 192, visible scanlines per panel
static constexpr int PANELS     = video::panel_count();    // 2 -- both LCDs are driven
static constexpr int MAP_COLS   = 64;           // 512px / 8
// g_map_shadow's row capacity, which is also the DS text BG's maximum height:
// 512px / 8. A 48-row plane needs 48 of these, and the remaining 16 continue
// into the grid's next nametable row (build_map), so the whole capacity is
// live every frame.
static constexpr int MAP_ROWS   = 64;           // 512px / 8, the BG's max height
static constexpr int DS_TILE_SZ = 32;           // bytes per 4bpp 8x8 DS tile

// OBJ priorities: front sprites above BG, BG above behind-sprites (and the
// backdrop below everything). DS draws lower priority numbers in front.
static constexpr int PRIO_SPR_FRONT = 1;
static constexpr int PRIO_BG        = 2;
static constexpr int PRIO_SPR_BEHIND = 3;

namespace {

int  g_bg     = 0;                               // bgInit() handle for main BG0
int  g_bg_sub = 0;                               // bgInitSub() handle for sub BG0
// BG tilemap shadow (built in RAM, DMA'd to VRAM during VBlank). 64x64
// entries (MAP_ROWS's own comment), laid out as four 32x32 screenblocks in
// the standard 2x2 text-BG arrangement (0/1 top row, 2/3 bottom row) -- an
// ordinary single-row board only ever populates/uses the top row (0/1, the
// BgSize_T_512x256 layout bgInit selects at init).
alignas(4) u16 g_map_shadow[MAP_COLS * MAP_ROWS];
// ---------------------------------------------------------------------------
// CHR bank switching: any kind, anywhere, any number of times per frame.
//
// A DS BG does not fetch CHR the way the NES does -- its tiles are a resident
// VRAM resource -- so the obvious model is to mirror each BANK the frame uses.
// That is what this used to do, and it is bounded: a 128 KB bank holds 7
// atlases, so 7 distinct bank states per frame and no more.
//
// So mirror TILES, not banks. A map entry carries a 10-bit tile index, i.e.
// 1024 resident tiles, keyed by CONTENT: the resolved CHR address each of a
// tile's eight pixel rows is fetched from. Every cell of the tilemap is
// resolved individually, under the bank state in effect on the scanlines that
// actually display it. The number of bank switches then stops mattering --
// switches only change the window bases between bands, and bands are free.
//
// Three consequences worth stating, because they are what make this exact
// where the bank-mirroring model was not:
//
//   - A switch at ANY scanline is honoured, not just at a band boundary that
//     happens to fall on a tile-row boundary. When a tile row's eight
//     scanlines span more than one bank state, the cache synthesises a
//     COMPOSITE tile, taking each pixel row from the state that displays it
//     (see g_slot_key / expand_rows). That is the sub-tile case a tilemap is
//     usually said to be incapable of.
//   - Sprites get the same treatment, resolved under the state at their own
//     scanline, so a mid-frame sprite-window re-bank works too.
//   - It is cheaper than mirroring banks in steady state: a cache hit costs a
//     hash probe, and only genuine misses expand a tile (32 bytes) rather
//     than a whole 16 KB atlas.
//
// Tile VRAM holds TILE_SLOTS 4bpp tiles (32 KB) at tile base 0; the tilemap
// moves to the 2 KB block after them. Both engines and both OBJ regions get
// the same pool, so any scanline of either panel can reference any tile.
static constexpr int TILE_SLOTS  = 1024;        // the map entry's 10-bit index
static constexpr int CHR_WINDOWS = 8;           // 8 KiB of CHR space / 1 KiB
static constexpr int TILE_BASE   = 0;           // 16 KB units: tiles at VRAM+0
static constexpr int MAP_BASE    = 16;          // 2 KB units: map after the pool
static constexpr int HASH_SIZE   = 2048;        // power of two, > TILE_SLOTS
static constexpr u16 HASH_EMPTY  = 0;           // stored value is slot + 1
static constexpr u16 HASH_TOMB   = 0xFFFF;

// Bands recorded during the raster walk. One per scanline is the worst case
// (an IRQ on every line), plus the implicit final band.
static constexpr int MAX_BANDS = video::viewport_py() + 2;

struct Band {
    u16 xs, ys;                  // this band's scroll
    u16 y0;                       // first viewport row it covers
    u32 base[CHR_WINDOWS];        // resolved LMA of each 1 KiB CHR window
};
Band g_band[MAX_BANDS];
int  g_bands;

// Which band, and which world row, each viewport scanline came from. Filled by
// band_emit; consumed by build_row_sources to work out what every tilemap row
// must be resolved under.
static constexpr int VPH = video::viewport_py();
u16  s_band_of[VPH];
u16  s_worldy[VPH];

// Per tilemap row: the band behind each of its eight pixel rows, whether the
// row is displayed at all (undisplayed rows are skipped entirely), and whether
// all eight agree (the fast path -- one resolve per cell instead of eight).
u16  g_rowband[MAP_ROWS][8];
bool g_row_used[MAP_ROWS];
bool g_row_uniform[MAP_ROWS];

// The tile pool. g_slot_key[s][f] is the resolved CHR address of the tile whose
// pixel row f this slot holds -- all eight equal for an ordinary tile, differing
// for a composite. Keying on the resolved address (not the tile id) is what
// makes a bank switch invisible to everything downstream; keying on the ROW
// addresses is what makes a mid-tile-row switch expressible.
//
// The palette is NOT part of the key: it lives in separate map-entry bits, so
// one tile drawn under four different attribute palettes is still one slot.
alignas(4) u8 g_tiles[TILE_SLOTS * DS_TILE_SZ];
u32  g_slot_key[TILE_SLOTS][8];
bool g_slot_valid[TILE_SLOTS];
bool g_slot_uniform[TILE_SLOTS];
u32  g_slot_frame[TILE_SLOTS];   // frame it was last referenced on
u16  g_slot_home[TILE_SLOTS];    // its hash index, so eviction can tombstone it
u16  g_hash[HASH_SIZE];
u32  g_frame;                     // frame counter for the eviction guard
int  g_alloc_cursor;              // rolling allocation, keeps misses contiguous
int  g_dirty_lo, g_dirty_hi;      // slot range rebuilt this frame (-1 = none)
u32  g_overflow;                  // frames that wanted > TILE_SLOTS distinct tiles
int  g_tombs;                     // tombstones in g_hash, see rehash()

// Per-scanline scroll. The main thread builds into sh_*; the HBlank ISR reads
// the live g_* copies. They are swapped during VBlank so the ISR never reads a
// half-written table (no tearing of the raster split).
// Per-scanline scroll, ONE TABLE PER PANEL. The DS's two LCDs are scanned
// SIMULTANEOUSLY off the single REG_VCOUNT (0..191 for both at once) -- there
// is no 384-line raster -- so viewport row `v` on the top panel and viewport
// row `192 + v` on the bottom panel are displayed at the SAME instant and both
// have to be programmed at VCOUNT v. Hence [panel][line], not one 384-entry
// table: band_emit routes each viewport row to the panel that shows it, and
// hblank_scroll programs both engines from the same line index.
u16          sh_h[PANELS][SCREEN_H], sh_v[PANELS][SCREEN_H];
u16          s_sprite_voff;   // unwrapped scroll of the band holding the last line
volatile u16 g_h[PANELS][SCREEN_H],  g_v[PANELS][SCREEN_H];

// 0xAARRGGBB -> DS BGR555 (X1BGR5; the unused top bit stays 0).
inline u16 to_bgr555(const u32 argb) {
    const u32 r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
    return static_cast<u16>(RGB15(r >> 3, g >> 3, b >> 3));
}

// Expand one NES tile into one DS 4bpp tile, taking each pixel row from the CHR
// address that row is fetched from. DS 4bpp packs two pixels per byte, low
// nibble = left pixel; NES pixel values 0-3 map straight onto the DS
// sub-palette index (0 = transparent), so no remap.
//
// @p key is ::g_slot_key's eight per-row addresses. For an ordinary tile they
// are all equal and this is the classic "read 16 bytes" expansion. When they
// differ -- a bank switch partway down this tile row -- each row comes from its
// own bank, which is exactly what the NES would have fetched.
void expand_rows(const u32 key[8], u8* out) {
    for (int row = 0; row < 8; row++) {
        const u8* nes = CHR_ROM + key[row];
        const u8  p0   = nes[row];
        const u8  p1   = nes[row + 8];
        for (int col = 0; col < 8; col += 2) {
            const u8 hiA = static_cast<u8>(7 - col), hiB = static_cast<u8>(6 - col);
            const u8 a = static_cast<u8>(((p0 >> hiA) & 1) | (((p1 >> hiA) & 1) << 1));
            const u8 b = static_cast<u8>(((p0 >> hiB) & 1) | (((p1 >> hiB) & 1) << 1));
            out[row * 4 + (col >> 1)] = static_cast<u8>(a | (b << 4));
        }
    }
}

// Resolved LMA of CHR address @p vma under @p b's window bases. Equivalent to
// ::ppu::ResolveTile, but with the per-window resolution (and its modulo)
// hoisted out to once per band: a window is 1 KiB and a tile 16 bytes, so the
// offset never leaves the window, and one conditional subtract is exactly the
// wrap ResolveTile's `% chrRomBytes` performs.
inline u32 lma_of(const Band& b, const unsigned vma) {
    u32 lma = b.base[(vma >> 10) & (CHR_WINDOWS - 1)] + (vma & 0x3FF);
    if (lma >= ppu::chrRomBytes) lma -= ppu::chrRomBytes;
    return lma;
}

inline u32 hash_mix(const u32 a, const u32 b, const u32 c) {
    u32 h = a * 2654435761u;
    h ^= b * 2246822519u;
    h ^= c * 3266489917u;
    return (h ^ (h >> 15)) & (HASH_SIZE - 1);
}

// Rebuild g_hash from the live slots, dropping every tombstone. Eviction has
// to tombstone rather than clear (a cleared entry would cut a probe chain), so
// without this they accumulate across frames until probes get long and, in the
// limit, no HASH_EMPTY is left to terminate on. Called between frames only,
// when they have actually built up: re-inserting is a hit for every slot, so
// nothing is re-expanded.
void rehash() {
    for (u32 i = 0; i < HASH_SIZE; i++) g_hash[i] = HASH_EMPTY;
    g_tombs = 0;
    for (int s = 0; s < TILE_SLOTS; s++) {
        if (!g_slot_valid[s]) continue;
        const u32 h0 = g_slot_uniform[s]
            ? hash_mix(g_slot_key[s][0], g_slot_key[s][0], 0u)
            : hash_mix(g_slot_key[s][0], g_slot_key[s][3], g_slot_key[s][7]);
        for (u32 i = 0; i < HASH_SIZE; i++) {
            const u32 h = (h0 + i) & (HASH_SIZE - 1);
            if (g_hash[h] == HASH_EMPTY) {
                g_hash[h] = static_cast<u16>(s + 1);
                g_slot_home[s] = static_cast<u16>(h);
                break;
            }
        }
    }
}

// A slot not already spoken for by an EARLIER cell of this same frame. Evicting
// one that is would silently change the tiles under whatever referenced it.
int alloc_slot() {
    for (int tries = 0; tries < TILE_SLOTS; tries++) {
        const int s = g_alloc_cursor;
        g_alloc_cursor = (g_alloc_cursor + 1 == TILE_SLOTS) ? 0 : g_alloc_cursor + 1;
        if (!g_slot_valid[s] || g_slot_frame[s] != g_frame) {
            if (g_slot_valid[s]) {
                // Tombstone, not clear: linear probing needs the chain intact.
                if (g_hash[g_slot_home[s]] == static_cast<u16>(s + 1)) {
                    g_hash[g_slot_home[s]] = HASH_TOMB;
                    g_tombs++;
                }
            }
            return s;
        }
    }
    return -1;   // every slot is live this frame -- see g_overflow
}

void commit_slot(const int s, const u32 key[8], const bool uniform, const u16 home) {
    __builtin_memcpy(g_slot_key[s], key, 8 * sizeof(u32));
    g_slot_valid[s]   = true;
    g_slot_uniform[s] = uniform;
    g_slot_frame[s]   = g_frame;
    g_slot_home[s]    = home;
    g_hash[home]      = static_cast<u16>(s + 1);
    expand_rows(key, g_tiles + s * DS_TILE_SZ);
    if (g_dirty_lo < 0 || s < g_dirty_lo) g_dirty_lo = s;
    if (s > g_dirty_hi) g_dirty_hi = s;
}

// Slot holding a tile whose eight rows all come from @p lma -- the ordinary
// case, and the one every cell of an unsplit tile row takes.
int intern_uniform(const u32 lma) {
    const u32 h0 = hash_mix(lma, lma, 0u);
    int tomb = -1;
    for (u32 i = 0; i < HASH_SIZE; i++) {
        const u32 h = (h0 + i) & (HASH_SIZE - 1);
        const u16 e = g_hash[h];
        if (e == HASH_EMPTY) {
            const u32 home = (tomb >= 0) ? static_cast<u32>(tomb) : h;
            const int s = alloc_slot();
            if (s < 0) { g_overflow++; return 0; }
            u32 key[8];
            for (int f = 0; f < 8; f++) key[f] = lma;
            commit_slot(s, key, true, static_cast<u16>(home));
            return s;
        }
        if (e == HASH_TOMB) { if (tomb < 0) tomb = static_cast<int>(h); continue; }
        const int s = e - 1;
        if (g_slot_valid[s] && g_slot_uniform[s] && g_slot_key[s][0] == lma) {
            g_slot_frame[s] = g_frame;
            return s;
        }
    }
    g_overflow++;
    return 0;
}

// Slot holding a tile whose rows come from differing addresses -- a bank switch
// partway down this tile row.
int intern_rows(const u32 key[8]) {
    const u32 h0 = hash_mix(key[0], key[3], key[7]);
    int tomb = -1;
    for (u32 i = 0; i < HASH_SIZE; i++) {
        const u32 h = (h0 + i) & (HASH_SIZE - 1);
        const u16 e = g_hash[h];
        if (e == HASH_EMPTY) {
            const u32 home = (tomb >= 0) ? static_cast<u32>(tomb) : h;
            const int s = alloc_slot();
            if (s < 0) { g_overflow++; return 0; }
            commit_slot(s, key, false, static_cast<u16>(home));
            return s;
        }
        if (e == HASH_TOMB) { if (tomb < 0) tomb = static_cast<int>(h); continue; }
        const int s = e - 1;
        if (g_slot_valid[s] && !g_slot_uniform[s] &&
            __builtin_memcmp(g_slot_key[s], key, 8 * sizeof(u32)) == 0) {
            g_slot_frame[s] = g_frame;
            return s;
        }
    }
    g_overflow++;
    return 0;
}

// Work out, for every tilemap row, which bank state each of its eight pixel
// rows must be resolved under. One pass over the scanlines the frame actually
// displayed; a row no scanline reached is left unused and skipped by build_map.
void build_row_sources() {
    for (int r = 0; r < MAP_ROWS; r++) { g_row_used[r] = false; g_row_uniform[r] = true; }

    for (int sy = 0; sy < VPH; sy++) {
        const u16 b = s_band_of[sy];
        if (b == 0xFFFF) continue;
        const u32 w    = s_worldy[sy];
        const int row  = static_cast<int>((w >> 3) & (MAP_ROWS - 1));
        const int fine = static_cast<int>(w & 7);
        if (!g_row_used[row]) {
            g_row_used[row] = true;
            // Seed every pixel row, so one no scanline covered still resolves
            // to something coherent rather than a stale band index.
            for (int f = 0; f < 8; f++) g_rowband[row][f] = b;
        }
        g_rowband[row][fine] = b;
    }

    for (int r = 0; r < MAP_ROWS; r++) {
        if (!g_row_used[r]) continue;
        const u16 b0 = g_rowband[r][0];
        for (int f = 1; f < 8; f++)
            if (g_rowband[r][f] != b0) { g_row_uniform[r] = false; break; }
    }
}

void build_palettes() {
    for (int p = 0; p < 4; p++) {
        for (int c = 1; c < 4; c++) {
            const u16 bg  = to_bgr555(nes_rgb[paletteRAM[p * 4 + c]        & 0x3F]);
            const u16 spr = to_bgr555(nes_rgb[paletteRAM[0x10 + p * 4 + c] & 0x3F]);
            BG_PALETTE[p * 16 + c]         = bg;
            SPRITE_PALETTE[p * 16 + c]     = spr;
            // The sub engine has its own palette RAM; without these the bottom
            // panel renders the right tiles in an all-black palette.
            BG_PALETTE_SUB[p * 16 + c]     = bg;
            SPRITE_PALETTE_SUB[p * 16 + c] = spr;
        }
    }
    BG_PALETTE[0]     = to_bgr555(nes_rgb[paletteRAM[0] & 0x3F]);
    BG_PALETTE_SUB[0] = BG_PALETTE[0];
}

// Rebuild the BG tilemap shadow from the linked board's nametables in
// VideoRAM/cart VRAM (::ppu::ReadNametable). Mirrors the tile/attribute
// decode the software PPU and GX backend use.
//
// The whole 64-row map capacity is filled every frame: a plane is 48 tile-rows
// on this target (video.hpp's TARGET_NDS branch) and the two panels together
// show 48 of them, so there is no longer a "first half only" case to skip.
// geo.tileW/tileH come from ::emu::ComputeNtGeometry (emu.hpp), the one shared
// place the plane shape and ::ppu::nametableCount's grid are derived, so this
// read side stays byte-for-byte consistent with the write side.
void build_map() {
    // Which pattern table the background fetches from, as a CHR-space address
    // (not a tile-index offset): tile ids are resolved through the bank state
    // now, so what is needed is the VMA the tile's bytes sit at.
    const unsigned chr_space = (ppu::PPUCTRL & ppu::ctrl::BG_ADDR) ? 0x1000u : 0x0000u;
    const emu::NtGeometry geo = emu::ComputeNtGeometry();

    // DS BG row R maps to PLANE row R, straight through. That is the whole
    // change from the old scheme, which derived the nametable row from
    // `row >> 5` -- treating each 32-row screenblock-half as a separate
    // nametable. That was exactly right while a plane was the NES's 30 rows
    // (one plane per screenblock-half, near enough), and is wrong now a plane
    // is 48 rows (video.hpp's TARGET_NDS branch): plane rows 32..47 -- which
    // is where a *_ds.uis UI lives -- had NO representation in the map at all,
    // because local_row never exceeded 31. Physical screenblock placement is
    // still fixed at 32-row boundaries (hardware), but that is now purely an
    // ADDRESSING detail handled by `sb`/`idx` below, not a content decision.
    const int planeRows = geo.tileH;

    for (int row = 0; row < MAP_ROWS; row++) {
        // A row no scanline displayed needs no resolve at all: its map entries
        // are never fetched, and skipping it is what keeps this to roughly the
        // visible cell count rather than the full 64x64 map.
        if (!g_row_used[row]) continue;

        // Which plane row, and which nametable row that plane row belongs to.
        // Rows past the first plane (48..63 for a 48-row plane) continue into
        // the grid's NEXT nametable row, and `% gridH` wraps them back onto
        // the first plane for a board that has only one row of nametables --
        // reproducing the vertical wrap such a board already relies on.
        const int nt_row    = (row / planeRows) % geo.gridH;
        const int local_row = row % planeRows;
        const int row32     = local_row * geo.tileW;
        const int at_roff   = (local_row >> 2) * geo.atW;
        const int at_rbits  = ((local_row >> 1) & 1) * 4;

        for (int col = 0; col < MAP_COLS; col++) {
            const int nt_col    = col >> 5;          // 0 or 1 (which nametable)
            const int local_col = col & 31;
            const int nt_off    = (nt_col + nt_row * geo.gridW) * geo.pageBytes;

            const u8  tile_id = ppu::ReadNametable(static_cast<u16>(nt_off + row32 + local_col));
            const u8  attr    = ppu::ReadNametable(static_cast<u16>(nt_off + geo.ntBytes + at_roff + (local_col >> 2)));
            const int pal     = (attr >> (((local_col >> 1) & 1) * 2 + at_rbits)) & 3;

            // The CHR address this tile's 16 bytes live at, then the slot
            // holding it under the bank state(s) that display THIS row. The
            // fast path is one resolve for the whole tile; the slow path, taken
            // only by a tile row a bank switch lands inside, resolves each
            // pixel row separately and the cache synthesises the composite.
            const unsigned vma = static_cast<unsigned>(chr_space + tile_id * 16);
            int slot;
            if (g_row_uniform[row]) {
                slot = intern_uniform(lma_of(g_band[g_rowband[row][0]], vma));
            } else {
                u32 key[8];
                for (int f = 0; f < 8; f++)
                    key[f] = lma_of(g_band[g_rowband[row][f]], vma);
                slot = intern_rows(key);
            }

            const u16 entry = static_cast<u16>(slot | (pal << 12));
            // Four screenblocks in the standard 2x2 text-BG arrangement:
            // 0/1 the top 32 physical rows, 2/3 the bottom 32. Keyed off the
            // PHYSICAL row (row >> 5), not the nametable row -- see the
            // planeRows comment above.
            const int sb  = (col >> 5) + ((row >> 5) * 2);
            const int idx = sb * 1024 + (row & 31) * 32 + local_col;
            g_map_shadow[idx] = entry;
        }
    }
}

// Push the OAM shadow to the DS hardware OBJ list. y is +1 vs OAM (matches the
// PPU). NES sprite colour-0 is transparent (DS handles it). All 64 NES OAM slots
// map to OBJ 0..63; off-screen sprites are hidden.
void build_sprites() {
    const oam::sprite_t* oam = emu::OamShadow();
    const bool tall   = ppu::PPUCTRL & ppu::ctrl::SPRITE_SIZE;
    const int  height = tall ? 16 : 8;
    // CHR-space address of the sprite pattern table, for the same reason
    // build_map uses one: tile ids are resolved through the bank state now.
    const unsigned spr_space = (ppu::PPUCTRL & ppu::ctrl::SPRITE_ADDR) ? 0x1000u : 0x0000u;

    // Sprites carry raw NES screen-space Y -- physics always runs on the full
    // 240-line frame regardless of the shorter DS panel -- so every OBJ must be
    // shifted up by the gameplay BG's vertical scroll to stay glued to it. That
    // scroll is no longer a fixed bottom-anchor constant: the game runs a vertical
    // follow camera (see SetScroll in demo/src/main.cpp) that eases it between the
    // full bottom-anchor and 0 as the player climbs, so the sprite offset must track
    // it frame by frame. GenerateBands has already filled the per-line scroll table
    // (sh_v) by the time we run, and the bottom row is always in the gameplay band,
    // so it carries the live gameplay scroll -- read it directly. This is the sprite
    // half of the window transform whose BG half is that scroll. HUD-region sprites
    // (here only the software sprite-0 split marker) get pushed off the top and
    // hidden, which is harmless: the DS split is band-driven, not real sprite-0
    // hardware detection.
    const int voff = static_cast<int>(s_sprite_voff);

    for (int s = 0; s < OAM_SPRITES; s++) {
        const oam::sprite_t& o = oam[s];
        const int sx = static_cast<int>(o.x);
        // Viewport-space Y: one coordinate space spanning both panels.
        const int sy = static_cast<int>(o.y) + 1 - voff;

        if (sx <= -8 || sx >= 256) {
            for (int half = 0; half < 2; half++) {
                oamClearSprite(&oamMain, s * 2 + half);
                oamClearSprite(&oamSub,  s * 2 + half);
            }
            continue;
        }

        const int  pal    = o.attributes & 0x03;
        const bool behind = (o.attributes & 0x20) != 0;
        const bool hflip  = (o.attributes & 0x40) != 0;
        const bool vflip  = (o.attributes & 0x80) != 0;
        // A tall sprite becomes TWO 8x8 OBJs rather than one 8x16. The pool is
        // content-addressed, so the two halves are not adjacent slots and a
        // 1D-mapped 8x16 OBJ -- which reads slot N then N+1 -- cannot express
        // it. 64 NES sprites x 2 is exactly the 128 OBJ entries an engine has,
        // and using indices s*2 / s*2+1 keeps NES OAM priority order intact.
        const int halves = tall ? 2 : 1;
        for (int half = 0; half < halves; half++) {
            // The CHR address of this half's 16 bytes. 8x16 takes tile bit 0 as
            // the pattern-table select (overriding SPRITE_ADDR) and pairs
            // (tile & 0xFE) with its successor; a vertical flip swaps which
            // half is on top.
            const int vhalf = vflip ? (halves - 1 - half) : half;
            const unsigned vma = tall
                ? static_cast<unsigned>(((o.tile & 1) * 0x1000) + ((o.tile & 0xFE) + vhalf) * 16)
                : static_cast<unsigned>(spr_space + o.tile * 16);

            // Resolved under the bank state at the scanline this half is drawn
            // on -- so a mid-frame re-bank of a SPRITE window is reproduced,
            // which the old whole-frame atlas could not do.
            const int ssy  = sy + half * 8;
            const int line = (ssy >= 0 && ssy < VPH) ? ssy : (ssy < 0 ? 0 : VPH - 1);
            const u16 bi   = s_band_of[line] == 0xFFFF ? 0 : s_band_of[line];
            const int slot = intern_uniform(lma_of(g_band[bi], vma));

            // Each engine has its OWN OAM, and a sprite is only visible to the
            // engine whose panel it falls on -- so submit to both, each with
            // the Y its panel uses. One straddling the boundary goes to both
            // and is clipped by each engine independently, which is what makes
            // it continuous across the seam (bezel aside).
            OamState* const oams[PANELS] = {&oamMain, &oamSub};
            const int oi = s * 2 + half;
            for (int panel = 0; panel < PANELS; panel++) {
                const int py = ssy - static_cast<int>(video::panel_rect(static_cast<u8>(panel)).y);
                if (py <= -8 || py >= SCREEN_H) {
                    oamClearSprite(oams[panel], oi);
                    continue;
                }
                oamSet(oams[panel], oi, sx, py,
                       behind ? PRIO_SPR_BEHIND : PRIO_SPR_FRONT,
                       pal, SpriteSize_8x8, SpriteColorFormat_16Color,
                       oamGetGfxPtr(oams[panel], slot),
                       -1, false, false, hflip, vflip, false);
            }
        }
    }
}

// ::emu::band_emit_fn: record the scroll for screen rows [y0,y1). Within a band
// the NES PPU auto-increments world Y 1:1 with the screen, so a single DS
// vertical scroll value (ys - y0) covers the whole band; the horizontal scroll
// is constant (xs). The HBlank ISR later programs these per scanline.
//
// World Y is wrapped at the nametable grid's own height (::emu::NtGeometry::
// worldH -- 240 for an ordinary board), exactly as the core renderer does
// (GenerateFrame's `ppu_y % world_h`). The DS's BG is 256 rows tall with rows
// 30/31 only restating 0/1 (see build_map), so raw hardware wrap at 256 is
// NOT the same thing: a band that starts at world Y 240 -- the title screen's
// menu band, which relies on Y wrapping onto nametable row 0 -- would land on
// those two duplicate rows instead of the top of the nametable. A band that
// never reaches worldH (all of gameplay) takes the `phys == w` path, so its
// table entries are byte-for-byte what they were before wrapping existed.
//
// s_sprite_voff keeps the UNWRAPPED band scroll: build_sprites' window offset
// is the frame's scroll, not a per-line wrapped position.
void band_emit(int y0, int y1, u16 xs, u16 ys) {
    const emu::NtGeometry geo = emu::ComputeNtGeometry();
    const int worldH = geo.worldH;
    const int vph    = video::viewport_py();
    const int a = y0 < 0 ? 0 : y0;
    const int b = y1 > vph ? vph : y1;

    if (a <= SCREEN_H - 1 && b > SCREEN_H - 1)
        s_sprite_voff = static_cast<u16>(static_cast<int>(ys) - y0);

    // The bank state is live right now, at this band's own position in the
    // raster timeline (GenerateBands has already run every handler up to y0),
    // so this is where a mid-frame CHR switch is captured -- at whatever
    // scanline the game put it. Record the window bases rather than resolving
    // tiles here: the tilemap is not built until the whole timeline has been
    // walked, so every band's state is available when each row is resolved.
    const int bi = g_bands < MAX_BANDS ? g_bands++ : MAX_BANDS - 1;
    Band& band = g_band[bi];
    band.xs = xs;
    band.ys = ys;
    band.y0 = static_cast<u16>(y0);
    for (int w = 0; w < CHR_WINDOWS; w++)
        band.base[w] = ppu::ResolveTile(static_cast<u16>(w * 0x400));

    for (int sy = a; sy < b; sy++) {
        // Which panel shows this viewport row, and that row's line WITHIN it.
        // Both panels scan line `line` at the same VCOUNT (see sh_h/sh_v's own
        // comment), so this is an index into that panel's own table, not a
        // position in one long raster.
        const int panel = sy / SCREEN_H;
        const int line  = sy % SCREEN_H;
        if (panel >= PANELS) continue;

        const int w = (static_cast<int>(ys) + (sy - y0)) % worldH;
        // BG row == world row: build_map lays plane row R at BG row R, so no
        // screenblock translation is needed here any more. The old
        // `(w / 240) * 256 + (w % 240)` hardcoded a 240px plane and a 256px
        // screenblock stride, which pushed every world row at or past 240 --
        // the whole lower-panel UI band -- to a BG row past the map's end.
        //
        // A DS text BG is at most 512px tall, so of the grid's full worldH
        // (768px for a 2x2 grid of 48-row planes) only the first 512px can be
        // resident at once; `& 0x1FF` is that limit, and it is also exactly
        // the 512px wrap the hardware applies to BG0VOFS.
        const int phys = w & 0x1FF;
        sh_h[panel][line] = xs;
        sh_v[panel][line] = static_cast<u16>((phys - line) & 0x1FF);
        // Provenance for build_row_sources: which band, and which world row.
        s_band_of[sy] = static_cast<u16>(bi);
        s_worldy[sy]  = static_cast<u16>(w);
    }
}

void band_noop(int, int, u16, u16) {}

// HBlank interrupt: program the scroll for the NEXT scanline from the live
// table. Runs in IRQ mode -- touches only globals + memory-mapped registers.
void hblank_scroll() {
    const unsigned next = REG_VCOUNT + 1u;
    if (next < static_cast<unsigned>(SCREEN_H)) {
        // One VCOUNT, both panels -- they are scanned in lockstep, so the sub
        // engine's registers are programmed from the same line index out of
        // its own table (sh_h/sh_v's comment).
        // Scroll only. The tile base is fixed now: a bank switch changes which
        // POOL SLOTS the map entries name, not which VRAM region the BG reads,
        // so there is nothing per-scanline left to program here.
        REG_BG0HOFS     = g_h[0][next];
        REG_BG0VOFS     = g_v[0][next];
        REG_BG0HOFS_SUB = g_h[1][next];
        REG_BG0VOFS_SUB = g_v[1][next];
    }
}

} // namespace

namespace video {

void WaitForPresent() {
    const bool bg  = ppu::PPUMASK & ppu::mask::BG;
    const bool spr = ppu::PPUMASK & ppu::mask::SPRITE;

    // A mapper (MMC3) switched a CHR bank since the atlas was last built --
    // re-expand all 512 tiles through the translator (VRAM upload is deferred
    // to the VBlank window below, alongside every other VRAM commit this
    // backend makes). Gated on the generation counter so an unbanked (NROM)
    // game never pays this cost past the one build already done in init().
    // Per-frame reset. The tile pool itself persists -- that is what makes a
    // stable screen cost nothing -- but the frame counter (the eviction guard),
    // the band list and the dirty range are per-frame.
    g_frame++;
    // Between frames is the only safe point to drop tombstones (see rehash).
    if (g_tombs > HASH_SIZE / 4) rehash();
    g_bands = 0;
    g_dirty_lo = -1;
    g_dirty_hi = -1;
    for (int i = 0; i < VPH; i++) s_band_of[i] = 0xFFFF;

    // Default the scroll tables so disabled / empty scanlines are well-defined.
    for (int p = 0; p < PANELS; p++)
        for (int i = 0; i < SCREEN_H; i++) { sh_h[p][i] = 0; sh_v[p][i] = 0; }
    s_sprite_voff = 0;

    // Walk the IRQ timeline (fires the game's scanline logic, incl. the
    // sprite-0 split) and record each band's scroll AND its CHR window bases.
    // Done even when BG is off so the handlers still run -- exactly like the
    // GX/3DS backends.
    emu::GenerateBands(bg ? band_emit : band_noop, true);

    // The tilemap is built AFTER the walk, not before: resolving a cell needs
    // the bank state of the scanlines that display it, and those are only all
    // known once every handler has run. build_row_sources turns the
    // per-scanline provenance band_emit recorded into a per-row answer.
    if (bg) {
        build_row_sources();
        build_map();
    }

    // Sprites: OBJ has no per-scanline tile base, so one slot has to serve the
    // whole frame's sprites. The frame's first band is used -- the state the
    // game set up before rendering began. A mid-frame re-bank of a SPRITE
    // window is therefore not reproduced (a re-bank of BG windows is, which is
    // the case that matters and the one games actually use); see the OBJ upload
    // in the VBlank block below.
    if (spr) build_sprites();
    else for (int s = 0; s < OAM_SPRITES * 2; s++) {
        oamClearSprite(&oamMain, s);
        oamClearSprite(&oamSub, s);
    }

    // Pace to the hardware frame, then commit all VRAM writes during VBlank.
    swiWaitForVBlank();

    // Only the slot range rebuilt this frame is uploaded. The rolling
    // allocation cursor keeps misses contiguous, so a scrolling screen moves a
    // few hundred bytes rather than a whole atlas; a stable screen moves
    // nothing. Both engines and both OBJ regions get the same pool, so any
    // scanline of either panel can reference any tile.
    if (g_dirty_lo >= 0) {
        const int off = g_dirty_lo * DS_TILE_SZ;
        const int len = (g_dirty_hi - g_dirty_lo + 1) * DS_TILE_SZ;
        DC_FlushRange(g_tiles + off, len);
        dmaCopy(g_tiles + off, (u8*)bgGetGfxPtr(g_bg)         + off, len);
        dmaCopy(g_tiles + off, (u8*)bgGetGfxPtr(g_bg_sub)     + off, len);
        dmaCopy(g_tiles + off, (u8*)oamGetGfxPtr(&oamMain, 0) + off, len);
        dmaCopy(g_tiles + off, (u8*)oamGetGfxPtr(&oamSub, 0)  + off, len);
    }

    build_palettes();

    if (bg) {
        DC_FlushRange(g_map_shadow, sizeof g_map_shadow);
        // Same map to both engines -- both look at the same plane; only the
        // scroll differs, so there is nothing panel-specific to build twice.
        dmaCopy(g_map_shadow, bgGetMapPtr(g_bg),     sizeof g_map_shadow);
        dmaCopy(g_map_shadow, bgGetMapPtr(g_bg_sub), sizeof g_map_shadow);
        bgShow(g_bg);
        bgShow(g_bg_sub);
    } else {
        bgHide(g_bg);
        bgHide(g_bg_sub);
    }

    // Publish the freshly built scroll table to the ISR, then seed line 0 (the
    // HBlank ISR only fills lines 1..191).
    memcpy(const_cast<u16*>(&g_h[0][0]), sh_h, sizeof sh_h);
    memcpy(const_cast<u16*>(&g_v[0][0]), sh_v, sizeof sh_v);
    REG_BG0HOFS     = g_h[0][0];
    REG_BG0VOFS     = g_v[0][0];
    REG_BG0HOFS_SUB = g_h[1][0];
    REG_BG0VOFS_SUB = g_v[1][0];

    oamUpdate(&oamMain);
    oamUpdate(&oamSub);

    // No IRQs permitted post-frame; discard anything still queued from this
    // frame's render before NMI enqueues for the next one (matches OGC/3DS).
    irq::irqPendingValid = false;
    nmi_vector();
}

} // namespace video

// init()/post() are the global library lifecycle hooks the RESET macro expands
// to (declared in interrupts.hpp), so they live at global scope -- not inside
// namespace video.
void irq::init() {
    // Main 2D engine: BG0 (text) + hardware sprites, 1D OBJ mapping.
    videoSetMode(MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D);
    vramSetBankA(VRAM_A_MAIN_BG);       // 128 KB: BG map + tiles
    vramSetBankB(VRAM_B_MAIN_SPRITE);   // 128 KB: OBJ tiles

    // Text BG so the NES nametable(s) fit and X scroll wraps at the 512px
    // world width; map at base 0 (8 KB, four screenblocks), tiles at base 1
    // (16 KB), so the two don't overlap.
    // Always 512x512 (all four screenblocks) now: a plane is 48 tile-rows
    // (384px), which does NOT fit the 256px of a 512x256 layout, so the old
    // nametableCount-dependent choice would have left plane rows 32..47 -- the
    // lower-panel UI band -- with nowhere to live. 512x512 is also the DS text
    // BG's maximum, which is why only the first 512px of the grid's 768px
    // world can be resident (see band_emit).
    g_bg = bgInit(0, BgType_Text4bpp, BgSize_T_512x512, MAP_BASE, TILE_BASE);
    bgSetPriority(g_bg, PRIO_BG);

    oamInit(&oamMain, SpriteMapping_1D_32, false);

    // Sub engine == the bottom panel. The viewport spans both panels
    // (video.hpp's TARGET_NDS branch), so this is no longer optional: without
    // it the lower half of the viewport is simply never displayed. It mirrors
    // the main engine exactly -- same BG layout, same map, same CHR, its own
    // palette RAM and its own OAM -- and differs only in the scroll the HBlank
    // ISR programs into it, which is what makes it show the viewport rows
    // below the top panel's.
    videoSetModeSub(MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D);
    vramSetBankC(VRAM_C_SUB_BG);        // 128 KB: sub BG map + tiles
    vramSetBankD(VRAM_D_SUB_SPRITE);    // 128 KB: sub OBJ tiles
    g_bg_sub = bgInitSub(0, BgType_Text4bpp, BgSize_T_512x512, MAP_BASE, TILE_BASE);
    bgSetPriority(g_bg_sub, PRIO_BG);
    oamInit(&oamSub, SpriteMapping_1D_32, false);

    // Expand the CHR ROM to 4bpp and upload it to both BG and OBJ tile VRAM.
    // With 1D_32 OBJ mapping an 8x8 16-colour tile is exactly one 32-byte
    // slot, so the BG tile layout and the OBJ tile layout are identical and
    // one pool feeds both. Tiles are resolved through the mapper's translator
    // per cell at build time (build_map), so this also picks up NROM's default
    // (unbanked) CHR. The display isn't running yet, so (unlike the per-frame
    // path) the VRAM clear can happen immediately.
    // Nothing to preload: the tile pool is populated on demand, per cell, from
    // the first frame on. Clear it so a slot the game never references reads as
    // transparent rather than as stale VRAM.
    for (u32 i = 0; i < HASH_SIZE; i++) g_hash[i] = HASH_EMPTY;
    __builtin_memset(g_tiles, 0, sizeof g_tiles);
    DC_FlushRange(g_tiles, sizeof g_tiles);
    dmaCopy(g_tiles, bgGetGfxPtr(g_bg),         sizeof g_tiles);
    dmaCopy(g_tiles, bgGetGfxPtr(g_bg_sub),     sizeof g_tiles);
    dmaCopy(g_tiles, oamGetGfxPtr(&oamMain, 0), sizeof g_tiles);
    dmaCopy(g_tiles, oamGetGfxPtr(&oamSub, 0),  sizeof g_tiles);

    // video::vram_bytes() (video.hpp): the DS's viewport is 32x48 -- both panels
    // stacked -- so a plane is 1632 bytes, above the NES's own 0x400, and this
    // resolves to gridW*gridH of those rather than the NES-hardware minimum.
    emu::InitMemory(video::vram_bytes());

    // The per-scanline raster mechanism: an HBlank ISR programs the scroll for
    // each upcoming line from the band table (this is what reproduces mid-frame
    // scroll changes and the sprite-0 split on native DS hardware).
    //
    // An HBlank interrupt needs BOTH the controller bit (REG_IE, via irqEnable)
    // AND the LCD's HBlank-IRQ-enable bit in REG_DISPSTAT. Calico's irqEnable only
    // touches REG_IE, so without lcdSetHBlankIrq(true) the ISR never fires and the
    // BG keeps line 0's scroll for the whole frame -- which silently defeats the
    // sprite-0 split and the bottom-anchor (the BG renders top-anchored, dropping
    // the ground below the panel).
    irqSet(IRQ_HBLANK, hblank_scroll);
    irqEnable(IRQ_HBLANK);
    lcdSetHBlankIrq(true);
}

void irq::post() {
    // The DS has no teardown counterpart a homebrew app needs at exit; the loader
    // resets the machine. Disable the HBlank ISR for tidiness -- clear the LCD
    // DISPSTAT enable as well as the controller bit (mirror of init()).
    lcdSetHBlankIrq(false);
    irqDisable(IRQ_HBLANK);
    irqClear(IRQ_HBLANK);
}
