/**
 * @file video.cpp
 * @brief Native 2D-hardware presentation backend for the Game Boy Advance (libgba).
 *
 * The NES PPU emulation is platform-agnostic and lives in src/emu/; this file is
 * only the GBA-specific half, and -- like the DS backend (src/nds/) and unlike
 * every CPU-rasterising backend -- it does NOT rasterise on the CPU. The GBA is a
 * 2D tile console whose hardware is a near-cousin of the NES PPU, so it hands the
 * NES's own primitives to the GBA 2D core (Model B):
 *
 *   - The 8 KB CHR ROM is expanded from 2bpp to the GBA's 4bpp tile format once at
 *     init and uploaded to both BG character VRAM and OBJ (sprite) VRAM. Each NES
 *     tile is one GBA tile; tiles 0-255 are pattern table 0, 256-511 table 1.
 *   - paletteRAM becomes GBA BGR555 palette RAM (4 BG sub-palettes + 4 sprite
 *     sub-palettes), rebuilt each frame. Pixel value 0 is transparent on both BG
 *     and OBJ: BG colour-0 shows the backdrop (palette entry 0); sprite colour-0
 *     is a hole -- reproducing NES transparency for free.
 *   - The nametable + attribute tables become a 512x256 GBA text BG tilemap
 *     (two horizontal NES nametables wide, two side-by-side 32x32 screenblocks),
 *     rebuilt each frame from VideoRAM.
 *   - The OAM shadow becomes hardware OBJ written straight into OAM; "behind
 *     background" sprites get a lower OBJ priority than BG0 so opaque BG pixels
 *     occlude them.
 *
 * The frame is driven by ::emu::GenerateBands so the scanline IRQ handlers (the
 * game's per-frame logic, incl. the sprite-zero scroll split) still fire in
 * raster order. Each band's scroll is recorded into a per-scanline table that an
 * HBlank interrupt programs into REG_BG0HOFS/VOFS line by line -- this is how the
 * GBA reproduces the NES's mid-frame raster scroll and the sprite-0 split on
 * native hardware, with no CPU per-pixel work.
 *
 * The 240x160 panel is the first target NARROWER than the NES (2 tiles / 16px)
 * as well as shorter (10 tiles / 80px). The backend shows a window onto the
 * unaltered 256x240 world (the NES game is never changed for the GBA); the
 * emulated PPU VRAM stays generous (a full 32-wide nametable) against the
 * narrower viewport, which is where the sub-tile-scroll + lookahead margin lives.
 *
 * Frame pacing is the real hardware VBlank (VBlankIntrWait), and the VRAM writes
 * (palette/map/OAM) are committed during VBlank to avoid tearing. Unlike the DS
 * the ARM7 has no data cache, so there are no cache flushes before the DMAs.
 *
 * Performance: the GBA's ARM7TDMI is ~4x slower than the DS's cached ARM9 and, by
 * default, runs code from the wait-stated GamePak ROM. The per-frame hot path
 * (build_map's 2048-entry tilemap rebuild above all, plus build_sprites/
 * build_palettes/band_emit and the HBlank ISR) is therefore marked IWRAM_CODE so
 * it executes from the 32 KB of zero-waitstate IWRAM; the per-frame shadows it
 * touches (g_map_shadow, the scroll tables, VideoRAM) are .bss, which is IWRAM on
 * the GBA. The only large buffer that is NOT hot -- the 16 KB CHR staging buffer,
 * used once at init -- is pushed to EWRAM (EWRAM_BSS) so it does not crowd the hot
 * code/data out of IWRAM. When the ARM7 still misses a VBlank, the HBlank ISR's
 * line-0 wrap-seed (see hblank_scroll) keeps the top of the screen stable rather
 * than stranding it on the previous scanline's gameplay scroll.
 */
#include "internal.hpp"

#include <platform-nes/video.hpp>
#include <platform-nes/interrupts.hpp>
#include "../emu/emu.hpp"

#include <gba.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The fixed NES master palette (ARGB8888, 0xAARRGGBB), shared verbatim with the
// software PPU in src/emu/ppu.cpp; here it only seeds the per-frame GBA palettes.
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
// GBA layout constants. The viewport panel is 240x160; the BG covers the full
// 512x256 world (two horizontal NES nametables) and is scrolled over the panel.
// ---------------------------------------------------------------------------
static constexpr int SCREEN_H    = 160;          // visible scanlines
static constexpr unsigned LINES_TOTAL = 228;     // total scanlines (160 visible + 68 vblank)
static constexpr int MAP_COLS    = 64;           // 512px / 8
// g_map_shadow's own row capacity: sized for the WORST case (a four-screen
// board's two stacked 240px nametable rows, ::ppu::nametableCount == 4) --
// 512px / 8 x2. An ordinary (::ppu::nametableCount == 2) board only ever
// fills/DMAs the first half (build_map's own activeRows, below); the
// unused second half costs a fixed ~4 KB of IWRAM but keeps this file free
// of any board-specific #if (see mmc3.hpp's own comment on
// ALTERNATIVE_NAMETABLE for why this TU must not presume that).
static constexpr int MAP_ROWS    = 64;           // 2 x (256px / 8)
static constexpr int CHR_TILES   = 512;          // NES tiles in 8 KB CHR
static constexpr int GBA_TILE_SZ = 32;           // bytes per 4bpp 8x8 GBA tile

// OBJ priorities: front sprites above BG, BG above behind-sprites (and the
// backdrop below everything). The GBA draws lower priority numbers in front,
// and an OBJ wins ties against a BG of equal priority.
static constexpr int PRIO_BG         = 2;
static constexpr int PRIO_SPR_FRONT  = 1;
static constexpr int PRIO_SPR_BEHIND = 3;

namespace {

// --- Register / VRAM bit constants -----------------------------------------
// libgba's REG_* macros and irq/dma/input/systemcall functions are stable, but
// the bit-value macro names drift between versions, so the handful of bits this
// backend pokes are spelled out here against the (architectural, fixed) GBA
// hardware layout.
constexpr u32 DCNT_MODE0    = 0x0000;   // tiled mode 0
constexpr u32 DCNT_BG0      = 0x0100;   // BG0 enable (bit 8)
constexpr u32 DCNT_OBJ      = 0x1000;   // OBJ enable (bit 12)
constexpr u32 DCNT_OBJ_1D   = 0x0040;   // 1D OBJ tile mapping (bit 6)
constexpr u16 DSTAT_HBL_IRQ = 0x0010;   // DISPSTAT HBlank-IRQ enable (bit 4)

constexpr u16 BGCNT_512x256 = 0x4000;   // size field = 1 (bits 14-15)
constexpr u16 BGCNT_512x512 = 0xC000;   // size field = 3 (bits 14-15) -- four screenblocks, 2x2
inline u16 BGCNT_CHARBASE(int n) { return static_cast<u16>(n << 2); }  // bits 2-3
inline u16 BGCNT_MAPBASE(int n)  { return static_cast<u16>(n << 8); }  // bits 8-12

constexpr u16 ATTR0_DISABLE = 0x0200;   // affine flag 0 + disable bit -> hidden
// libgba's gba_sprites.h already defines ATTR0_TALL (OBJ_SHAPE(TALL), bits 14-15
// = 10b); size 00b under that shape is 8x16, so no extra size OR term is needed.
constexpr u16 ATTR1_HFLIP   = 0x1000;
constexpr u16 ATTR1_VFLIP   = 0x2000;
// 16-colour (attr0 bit13=0), square shape (attr0 bits14-15=0) and 8x8 size
// (attr1 bits14-15=0) are all zero, so no OR terms are needed for them.

// --- VRAM / palette / OAM pointers (fixed GBA addresses) -------------------
// BG tiles fill charblock 0 (16 KB = exactly 512 4bpp tiles); the BG map sits in
// screenblock 8 (0x06004000), the next region after the tile atlas. OBJ tiles
// live in the separate object-VRAM region at 0x06010000.
inline u8*  bg_tiles()  { return reinterpret_cast<u8*>(0x06000000); }
inline u16* bg_map()    { return reinterpret_cast<u16*>(0x06004000); }
inline u8*  obj_tiles() { return reinterpret_cast<u8*>(0x06010000); }
inline u16* bg_pal()    { return reinterpret_cast<u16*>(0x05000000); }
inline u16* obj_pal()   { return reinterpret_cast<u16*>(0x05000200); }
inline void* oam_mem()  { return reinterpret_cast<void*>(0x07000000); }

// One hardware OBJ entry (8 bytes: attr0/1/2 + an interleaved affine slot we
// leave at 0 since no sprite is affine).
struct ObjAttr { u16 a0, a1, a2, fill; };

// BG tilemap shadow (built in RAM, DMA'd to VRAM during VBlank). 64x64 entries
// (MAP_ROWS's own comment), laid out as four 32x32 screenblocks in the
// standard 2x2 text-BG arrangement (0/1 top row, 2/3 bottom row) -- an
// ordinary single-row board only ever populates/uses the top row (0/1, the
// 512x256 layout REG_BG0CNT selects at init). Kept in IWRAM (the default
// .bss section on the GBA) so build_map's per-frame writes are zero-waitstate.
alignas(4) u16 g_map_shadow[MAP_COLS * MAP_ROWS];
// 4bpp CHR staged at init and whenever a mapper (e.g. MMC3) switches a CHR
// bank, then uploaded to BG + OBJ tile VRAM. This buffer is 16 KB and rebuilt
// only on a CHR bank change (see build_chr_atlas), so it is parked in EWRAM
// rather than eating half of the scarce 32 KB IWRAM -- that space is reserved
// for the per-frame .bss shadows above and the hot IWRAM_CODE functions below.
EWRAM_BSS alignas(4) u8 g_chr4[CHR_TILES * GBA_TILE_SZ];
// Generation of ::ppu::chrGeneration the atlas above was last built from.
// Sentinel (not 0, ::chrGeneration's own initial value) forces the first build.
u32 g_chr_built_gen = ~0u;
// OAM shadow (all 128 OBJ); built each frame, DMA'd to OAM during VBlank.
alignas(4) ObjAttr g_oam_shadow[128];

// Per-scanline scroll. The main thread builds into sh_*; the HBlank ISR reads
// the live g_* copies. They are swapped during VBlank so the ISR never reads a
// half-written table (no tearing of the raster split).
u16          sh_h[SCREEN_H], sh_v[SCREEN_H];
volatile u16 g_h[SCREEN_H],  g_v[SCREEN_H];

// 0xAARRGGBB -> GBA BGR555 (X1BGR5; the unused top bit stays 0).
inline u16 to_bgr555(const u32 argb) {
    const u32 r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
    return static_cast<u16>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

// Expand one NES 2bpp tile (16 bytes) into one GBA 4bpp tile (32 bytes). 4bpp
// packs two pixels per byte, low nibble = left pixel. NES pixel values 0-3 map
// straight onto the GBA sub-palette index (0 = transparent), so no remap.
void expand_tile(const u8* nes, u8* out) {
    for (int row = 0; row < 8; row++) {
        const u8 p0 = nes[row];
        const u8 p1 = nes[row + 8];
        for (int c2 = 0; c2 < 4; c2++) {
            const int cl = c2 * 2, cr = c2 * 2 + 1;
            const u8 vl = ((p0 >> (7 - cl)) & 1) | (((p1 >> (7 - cl)) & 1) << 1);
            const u8 vr = ((p0 >> (7 - cr)) & 1) | (((p1 >> (7 - cr)) & 1) << 1);
            out[row * 4 + c2] = static_cast<u8>(vl | (vr << 4));
        }
    }
}

// Rebuild the 4bpp CHR atlas from CHR_ROM, resolving each of the 512 PPU tile
// slots through ::ppu::ResolveTile. An unbanked board's default is the
// identity function, so this is byte-for-byte what the old one-shot init
// loop did; a bank-switching mapper (MMC3) instead pulls each tile from
// whatever physical CHR bank is presently switched into that PPU address, the
// same resolution the per-pixel software rasterizer does on every fetch. Only
// called when ::ppu::chrGeneration has moved since the last build (see
// WaitForPresent), since re-expanding all 512 tiles every frame regardless
// would cost real time on this backend's wait-stated ARM7.
// IWRAM_CODE: runs whenever a CHR bank switch lands mid-gameplay, not just at
// init, so it needs to be off the slow GamePak ROM bus like the other hot
// per-frame builders.
// Only touches g_chr4 (EWRAM) -- safe to run any time, including outside
// VBlank. The VRAM upload is a separate step (see WaitForPresent) so it can
// be deferred to the VBlank window like every other VRAM commit this backend
// makes, keeping this one's extra CPU cost off the tear-sensitive path.
IWRAM_CODE void build_chr_atlas() {
    for (int t = 0; t < CHR_TILES; t++) {
        const u32 lma = ppu::ResolveTile(static_cast<u16>(t * 16));
        expand_tile(CHR_ROM + lma, g_chr4 + t * GBA_TILE_SZ);
    }
}

// Rebuild the GBA BG + sprite sub-palettes from paletteRAM. Sub-palette 0..3 hold
// the 4 BG / 4 sprite palettes; entry 0 of each is transparent. The universal
// backdrop is palette entry 0 (shown where every BG pixel is colour 0).
// IWRAM_CODE: runs every frame; zero-waitstate execution keeps it off the slow
// GamePak ROM bus (see build_map for why per-frame ARM7 cost is the bottleneck).
IWRAM_CODE void build_palettes() {
    for (int p = 0; p < 4; p++) {
        for (int c = 1; c < 4; c++) {
            bg_pal()[p * 16 + c]  = to_bgr555(nes_rgb[paletteRAM[p * 4 + c]        & 0x3F]);
            obj_pal()[p * 16 + c] = to_bgr555(nes_rgb[paletteRAM[0x10 + p * 4 + c] & 0x3F]);
        }
    }
    bg_pal()[0] = to_bgr555(nes_rgb[paletteRAM[0] & 0x3F]);
}

// Rebuild the BG tilemap shadow from the linked board's nametables in
// VideoRAM/cart VRAM (::ppu::ReadNametable). Mirrors the tile/attribute
// decode the software PPU and GX/DS backends use; NES vertical wrap within
// one 240px row (30 tile-rows) is reproduced with `local_row % 30`, applied
// independently within each 32-row screenblock-half -- see build_map's own
// comment for why it can't be one combined period across both halves.
//
// activeRows -- not the fixed MAP_ROWS capacity -- bounds the loop: an
// ordinary 2-nametable board (::emu::NtGeometry::gridH == 1) only ever fills
// the first 32 GBA tile-rows (one screenblock-row), the same footprint and
// cost this loop always had before four-screen support existed, and never
// calls ::ppu::ReadNametable with an offset past what that board's VideoRAM
// actually covers. geo.tileW/tileH are always exactly the NES-native 32/30 --
// never GBA's own 30x20 viewport (see TARGET_GBA's own viewport_tx()/
// viewport_ty() doc comments, video.hpp) -- so this stays exactly the
// screenblock-shaped walk it always was; ::emu::ComputeNtGeometry (emu.hpp)
// is just the one shared place that shape (and ::ppu::nametableCount's grid)
// now comes from, instead of this file's own copy of the formula.
//
// IWRAM_CODE: this is the backend's per-frame hot spot -- up to 64x64 = 4096
// entries on a four-screen board (2048 on an ordinary one), each with two
// nametable reads + an attribute decode. On the 16.78 MHz ARM7 running from
// GamePak ROM (wait states) that alone is multiple milliseconds and is the
// main reason a frame can overrun its 16.7 ms budget; executing it from
// zero-waitstate IWRAM (with VideoRAM/g_map_shadow also in IWRAM) is the
// single biggest lever against dropped frames on this backend.
IWRAM_CODE void build_map() {
    const int atlas0 = (ppu::PPUCTRL & ppu::ctrl::BG_ADDR) ? 256 : 0;
    const emu::NtGeometry geo = emu::ComputeNtGeometry();
    const int activeRows = 32 * geo.gridH;

    for (int row = 0; row < activeRows; row++) {
        // Each 32-row screenblock-half is an independent tileH-periodic
        // content stream -- NOT one continuous period spanning both halves.
        // Physical screenblock placement is fixed at 32-row boundaries
        // (hardware fact); NES nametable content repeats every tileH (30)
        // rows (hardware fact too); those two periods don't divide evenly
        // into each other, so deriving nt_row from a combined modulus across
        // the full 64-row span (e.g. `row % 60`) desyncs partway through the
        // second half -- confirmed as a real, reproduced bug (screenblock 2/3
        // content landing 2 rows short of where it belongs, "mirroring" the
        // tail of screenblock 0/1's own row-30/31 wraparound into it
        // instead). row >> 5 alone gives which screenblock-half (and
        // therefore which nt_row) a physical row belongs to, always exactly
        // at the 32-row boundary; local_row's own `% tileH` wrap then
        // reproduces, within THAT half alone, the same 2-row scroll-
        // lookahead duplication an ordinary single-row board already relies
        // on (rows 30/31 restating rows 0/1) -- this is also, by
        // construction, byte-for-byte the original single-row formula
        // whenever geo.gridH == 1 (activeRows == 32, so nt_row is always 0).
        const int nt_row    = row >> 5;
        const int row_in_sb = row & 31;
        const int local_row = row_in_sb % geo.tileH;
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

            const u16 entry = static_cast<u16>((atlas0 + tile_id) | (pal << 12));
            // Four screenblocks in the standard 2x2 text-BG arrangement: 0/1
            // top row (nt_row 0), 2/3 bottom row (nt_row 1).
            const int sb  = (col >> 5) + nt_row * 2;
            const int idx = sb * 1024 + row_in_sb * 32 + local_col;
            g_map_shadow[idx] = entry;
        }
    }
}

// Build the OAM shadow from the NES OAM. y is +1 vs OAM (matches the PPU). NES
// sprite colour-0 is transparent (the GBA handles it). The 64 NES OAM slots map
// to OBJ 0..63; off-screen sprites are hidden (OBJ 64..127 stay hidden from init).
// IWRAM_CODE: runs every frame (64 OBJ); kept off the ROM bus like build_map.
IWRAM_CODE void build_sprites() {
    const oam::sprite_t* oam = emu::OamShadow();
    const bool tall   = ppu::PPUCTRL & ppu::ctrl::SPRITE_SIZE;
    const int  height = tall ? 16 : 8;
    const int  atlas0 = (ppu::PPUCTRL & ppu::ctrl::SPRITE_ADDR) ? 256 : 0;

    // Sprites carry raw NES screen-space Y -- physics always runs on the full
    // 240-line frame regardless of the shorter GBA panel -- so every OBJ must be
    // shifted up by the gameplay BG's vertical scroll to stay glued to it. That
    // scroll is not a fixed bottom-anchor constant: the game runs a vertical
    // follow camera (see SetScroll in demo/src/main.cpp) that eases it between the
    // full bottom-anchor and 0 as the player climbs, so the sprite offset must
    // track it frame by frame. GenerateBands has already filled the per-line
    // scroll table (sh_v) by the time we run, and the bottom row is always in the
    // gameplay band, so it carries the live gameplay scroll -- read it directly.
    // This is the sprite half of the window transform whose BG half is that scroll.
    const int voff = static_cast<int>(sh_v[SCREEN_H - 1]);

    for (int s = 0; s < OAM_SPRITES; s++) {
        const oam::sprite_t& o = oam[s];
        const int sx = static_cast<int>(o.x);
        const int sy = static_cast<int>(o.y) + 1 - voff;

        // Clip against the 240x160 panel (narrower AND shorter than the NES).
        if (sx <= -8 || sx >= 240 || sy <= -height || sy >= SCREEN_H) {
            g_oam_shadow[s].a0 = ATTR0_DISABLE;
            continue;
        }

        const int  pal    = o.attributes & 0x03;
        const bool behind = (o.attributes & 0x20) != 0;
        const bool hflip  = (o.attributes & 0x40) != 0;
        const bool vflip  = (o.attributes & 0x80) != 0;
        // 8x16: tile bit 0 selects the pattern table (overriding SPRITE_ADDR/atlas0)
        // and the low tile of the pair (tile & 0xFE) is the top half; the CHR atlas
        // already lays tiles 0-255/256-511 out as the two pattern tables, and 1D OBJ
        // mapping reads tile N then N+1 for a 1-tile-wide, 2-tile-tall OBJ -- which is
        // exactly the NES's (tile & 0xFE)/(tile & 0xFE)+1 pair, so hardware does the
        // rest with no extra per-row work.
        const int  ti     = tall ? ((o.tile & 1) * 256 + (o.tile & 0xFE)) : (atlas0 + o.tile);
        const int  prio   = behind ? PRIO_SPR_BEHIND : PRIO_SPR_FRONT;

        // OAM x is 9-bit and y 8-bit; masking lets a sprite straddle the left/top
        // edge via hardware wrap (the clip above already drops fully-off ones).
        g_oam_shadow[s].a0 = static_cast<u16>((sy & 0xFF) | (tall ? ATTR0_TALL : 0));
        g_oam_shadow[s].a1 = static_cast<u16>((sx & 0x1FF)
                                            | (hflip ? ATTR1_HFLIP : 0)
                                            | (vflip ? ATTR1_VFLIP : 0));
        g_oam_shadow[s].a2 = static_cast<u16>(ti | (prio << 10) | (pal << 12));
    }
}

// ::emu::band_emit_fn: record the scroll for screen rows [y0,y1). Within a band
// the NES PPU auto-increments world Y 1:1 with the screen, so a single GBA
// vertical scroll value (ys - y0) covers the whole band; the horizontal scroll
// is constant (xs). The HBlank ISR later programs these per scanline.
// IWRAM_CODE: invoked once per band from emu::GenerateBands every frame.
IWRAM_CODE void band_emit(int y0, int y1, u16 xs, u16 ys) {
    const u16 v = static_cast<u16>(static_cast<int>(ys) - y0);
    int a = y0 < 0 ? 0 : y0;
    int b = y1 > SCREEN_H ? SCREEN_H : y1;
    for (int sy = a; sy < b; sy++) {
        sh_h[sy] = xs;
        sh_v[sy] = v;
    }
}

void band_noop(int, int, u16, u16) {}

// HBlank interrupt: program the scroll for the NEXT scanline from the live
// table. Runs in IRQ mode -- touches only globals + memory-mapped registers.
// Placed in IWRAM (zero-waitstate) so 228 fires/frame stay cheap on the ARM7.
//
// The wrap case re-seeds line 0 during the LAST scanline of the frame (VCOUNT
// 227 -> next 0). This is what makes the raster path robust to DROPPED frames:
// line 0's scroll is otherwise only written by WaitForPresent, which does not
// run on a dropped frame -- so without this the top of the screen would hold
// line 159's gameplay scroll for the whole dropped frame, i.e. the HUD bar at
// the top jumps for one frame whenever the ARM7 misses VBlank. Reading the live
// g_* table here keeps line 0 correct every frame regardless of drops (on a
// dropped frame g_h[0]/g_v[0] is the prior frame's HUD scroll, which is right).
IWRAM_CODE void hblank_scroll() {
    unsigned next = REG_VCOUNT + 1u;
    if (next >= LINES_TOTAL) next = 0u;     // wrap: last vblank line seeds line 0
    if (next < static_cast<unsigned>(SCREEN_H)) {
        REG_BG0HOFS = g_h[next];
        REG_BG0VOFS = g_v[next];
    }
}

} // namespace

namespace video {

// IWRAM_CODE: this is the per-frame driver, and with LTO it inlines build_map/
// build_palettes/build_sprites (anonymous-namespace, single call site) directly
// into its body -- so to actually get that hot code off the GamePak ROM bus the
// driver itself must live in IWRAM, not just the (inlined-away) callees. The
// noinline is load-bearing: cross-module LTO would otherwise inline WaitForPresent
// itself into the demo's main loop (in ROM .text), discarding the IWRAM section
// placement entirely. Keeping it a standalone IWRAM function (called once/frame
// from ROM via a veneer) anchors the whole hot path in fast RAM. The libgba calls
// it makes (VBlankIntrWait, dmaCopy) and nmi() stay in ROM, reached via veneers.
IWRAM_CODE NI void WaitForPresent() {
    const bool bg  = ppu::PPUMASK & ppu::mask::BG;
    const bool spr = ppu::PPUMASK & ppu::mask::SPRITE;

    // A mapper (MMC3) switched a CHR bank since the atlas was last built --
    // re-expand all 512 tiles through the translator (VRAM upload is deferred
    // to the VBlank window below, alongside every other VRAM commit this
    // backend makes). Gated on the generation counter so an unbanked (NROM)
    // game never pays this cost past the one build already done in init().
    const bool chr_dirty = g_chr_built_gen != ppu::chrGeneration;
    if (chr_dirty) build_chr_atlas();

    // Default the scroll table so disabled / empty scanlines are well-defined.
    for (int i = 0; i < SCREEN_H; i++) { sh_h[i] = 0; sh_v[i] = 0; }

    if (bg) build_map();

    // Walk the IRQ timeline (fires the game's scanline logic, incl. the sprite-0
    // split) and record each band's scroll. Done even when BG is off so the
    // handlers still run -- exactly like the GX/3DS/DS backends.
    emu::GenerateBands(bg ? band_emit : band_noop);

    if (spr) build_sprites();
    else for (int s = 0; s < OAM_SPRITES; s++) g_oam_shadow[s].a0 = ATTR0_DISABLE;

    // Pace to the hardware frame, then commit all VRAM writes during VBlank.
    VBlankIntrWait();

    if (chr_dirty) {
        dmaCopy(g_chr4, bg_tiles(),  sizeof g_chr4);
        dmaCopy(g_chr4, obj_tiles(), sizeof g_chr4);
        g_chr_built_gen = ppu::chrGeneration;
    }

    build_palettes();

    if (bg) {
        dmaCopy(g_map_shadow, bg_map(), sizeof g_map_shadow);
        REG_DISPCNT |= DCNT_BG0;
    } else {
        REG_DISPCNT &= ~DCNT_BG0;
    }

    // Publish the freshly built scroll table to the ISR, then seed line 0 for the
    // immediate frame. (The HBlank ISR also wrap-seeds line 0 every frame from the
    // live table, which is what survives a dropped frame; this explicit seed just
    // makes the just-published table take effect without waiting for VCOUNT 227.)
    memcpy(const_cast<u16*>(g_h), sh_h, sizeof sh_h);
    memcpy(const_cast<u16*>(g_v), sh_v, sizeof sh_v);
    REG_BG0HOFS = g_h[0];
    REG_BG0VOFS = g_v[0];

    dmaCopy(g_oam_shadow, oam_mem(), sizeof g_oam_shadow);

    // No IRQs permitted post-frame; discard anything still queued from this
    // frame's render before NMI enqueues for the next one (matches OGC/3DS/DS).
    irq::irqPendingValid = false;
    nmi_vector();
}

} // namespace video

// init()/post() are the global library lifecycle hooks the RESET macro expands
// to (declared in interrupts.hpp), so they live at global scope -- not inside
// namespace video.
void irq::init() {
    irqInit();
    irqSet(IRQ_HBLANK, hblank_scroll);
    irqEnable(IRQ_VBLANK);   // required for VBlankIntrWait frame pacing

    // The per-scanline raster mechanism: an HBlank ISR programs the scroll for
    // each upcoming line from the band table (this is what reproduces mid-frame
    // scroll changes and the sprite-0 split on native GBA hardware).
    //
    // An HBlank interrupt needs BOTH the controller bit (REG_IE, via irqEnable)
    // AND the LCD's HBlank-IRQ-enable bit in REG_DISPSTAT. libgba's irqEnable
    // does set the DISPSTAT bit too (the inverse of calico on the DS, where we
    // had to call lcdSetHBlankIrq separately) -- but we set it explicitly anyway
    // so the raster path never depends on that library detail.
    REG_DISPSTAT |= DSTAT_HBL_IRQ;
    irqEnable(IRQ_HBLANK);

    // BG0: 16-colour tiles in charblock 0, map in screenblock 8 (right after
    // the 16 KB tile atlas). Map size follows the linked board's own
    // ::ppu::nametableCount (video.hpp): 512x256 (two horizontal NES
    // nametables, one row) for an ordinary (2-nametable) board, 512x512 (four
    // screenblocks, a four-screen board's second stacked row) otherwise --
    // see build_map's own comment for how activeRows keeps an ordinary
    // board's per-frame cost identical to before this was possible at all.
    REG_BG0CNT = static_cast<u16>(PRIO_BG | BGCNT_CHARBASE(0) | BGCNT_MAPBASE(8)
        | (ppu::nametableCount > 2 ? BGCNT_512x512 : BGCNT_512x256));

    // Expand the CHR ROM to 4bpp and upload it to both BG and OBJ tile VRAM.
    // With 1D OBJ mapping an 8x8 16-colour tile is exactly one 32-byte slot, so
    // the BG tile layout and the OBJ tile layout are identical and one staged
    // buffer feeds both. build_chr_atlas resolves through the mapper's tile
    // translator, so this also picks up an unbanked (NROM-like) board's default
    // (identity) CHR. The display isn't running yet, so (unlike the per-frame
    // path) the VRAM upload can happen immediately rather than waiting for VBlank.
    build_chr_atlas();
    dmaCopy(g_chr4, bg_tiles(),  sizeof g_chr4);
    dmaCopy(g_chr4, obj_tiles(), sizeof g_chr4);
    g_chr_built_gen = ppu::chrGeneration;

    // Hide every OBJ up front (the 64 NES sprites use OBJ 0..63; 64..127 stay off).
    for (int s = 0; s < 128; s++) g_oam_shadow[s].a0 = ATTR0_DISABLE;
    dmaCopy(g_oam_shadow, oam_mem(), sizeof g_oam_shadow);

    // video::vram_bytes() (video.hpp): the GBA's 30x20 viewport is at or under
    // the NES's native 32x30, so this resolves to the NES-hardware minimum
    // (2 pages/0x800 bytes), like the NES hardware itself.
    emu::InitMemory(video::vram_bytes());

    // Display on: mode 0, BG0 + OBJ enabled, 1D OBJ tile mapping.
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;
}

void irq::post() {
    // The GBA has no teardown a homebrew app needs at exit; the loader/BIOS
    // resets the machine. Disable the HBlank ISR for tidiness -- clear the LCD
    // DISPSTAT enable as well as the controller bit (mirror of init()).
    REG_DISPSTAT &= ~DSTAT_HBL_IRQ;
    irqDisable(IRQ_HBLANK);
}
