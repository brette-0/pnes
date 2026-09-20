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
 *   - The nametable + attribute tables become a 512x256 DS text BG tilemap
 *     (two horizontal NES nametables wide), rebuilt each frame from VideoRAM.
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
 * The 256x192 panel is 6 tiles shorter than the NES's 240px frame; the backend
 * shows a window onto the unaltered 256x240 world (the NES game is never changed
 * for the DS). Because the DS BG wraps vertically at 256 while the NES wraps at
 * 240, this first pass assumes vertical scroll stays within one nametable (the
 * demo is a status-bar + horizontal-scroll title); pure-X raster splits -- the
 * common case -- are exact.
 *
 * Frame pacing is the real hardware VBlank (swiWaitForVBlank), and the VRAM
 * writes (palette/map/OAM) are committed during VBlank to avoid tearing.
 */
#include "internal.hpp"

#include <platform-nes/video.hpp>
#include <platform-nes/interrupts.hpp>
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
static constexpr int SCREEN_H   = 192;          // visible scanlines
static constexpr int MAP_COLS   = 64;           // 512px / 8
// g_map_shadow's own row capacity: sized for the WORST case (a four-screen
// board's two stacked 240px nametable rows, ::ppu::nametableCount == 4) --
// 512px / 8 x2. An ordinary (::ppu::nametableCount == 2) board only ever
// fills/DMAs the first half (build_map's own activeRows, below); the
// unused second half costs a fixed amount of RAM but keeps this file free
// of any board-specific #if (see mmc3.hpp's own comment on
// ALTERNATIVE_NAMETABLE for why this TU must not presume that).
static constexpr int MAP_ROWS   = 64;           // 2 x (256px / 8)
static constexpr int CHR_TILES  = 512;          // NES tiles in 8 KB CHR
static constexpr int DS_TILE_SZ = 32;           // bytes per 4bpp 8x8 DS tile

// OBJ priorities: front sprites above BG, BG above behind-sprites (and the
// backdrop below everything). DS draws lower priority numbers in front.
static constexpr int PRIO_SPR_FRONT = 1;
static constexpr int PRIO_BG        = 2;
static constexpr int PRIO_SPR_BEHIND = 3;

namespace {

int  g_bg = 0;                                   // bgInit() handle for BG0
// BG tilemap shadow (built in RAM, DMA'd to VRAM during VBlank). 64x64
// entries (MAP_ROWS's own comment), laid out as four 32x32 screenblocks in
// the standard 2x2 text-BG arrangement (0/1 top row, 2/3 bottom row) -- an
// ordinary single-row board only ever populates/uses the top row (0/1, the
// BgSize_T_512x256 layout bgInit selects at init).
alignas(4) u16 g_map_shadow[MAP_COLS * MAP_ROWS];
// 4bpp CHR staged in RAM at init and whenever a mapper (e.g. MMC3) switches a
// CHR bank, then uploaded to BG + OBJ tile VRAM.
alignas(4) u8  g_chr4[CHR_TILES * DS_TILE_SZ];
// Generation of ::ppu::chrGeneration the atlas above was last built from.
// Sentinel (not 0, ::chrGeneration's own initial value) forces the first build.
u32 g_chr_built_gen = ~0u;

// Per-scanline scroll. The main thread builds into sh_*; the HBlank ISR reads
// the live g_* copies. They are swapped during VBlank so the ISR never reads a
// half-written table (no tearing of the raster split).
u16          sh_h[SCREEN_H], sh_v[SCREEN_H];
u16          s_sprite_voff;   // unwrapped scroll of the band holding the last line
volatile u16 g_h[SCREEN_H],  g_v[SCREEN_H];

// 0xAARRGGBB -> DS BGR555 (X1BGR5; the unused top bit stays 0).
inline u16 to_bgr555(const u32 argb) {
    const u32 r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
    return static_cast<u16>(RGB15(r >> 3, g >> 3, b >> 3));
}

// Expand one NES 2bpp tile (16 bytes) into one DS 4bpp tile (32 bytes). DS 4bpp
// packs two pixels per byte, low nibble = left pixel. NES pixel values 0-3 map
// straight onto the DS sub-palette index (0 = transparent), so no remap.
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
// touches g_chr4 (RAM) -- safe to run any time; the VRAM upload is a separate
// step (see WaitForPresent) deferred to VBlank like this backend's other
// VRAM commits. Called only when ::ppu::chrGeneration has moved since the
// last build.
void build_chr_atlas() {
    for (int t = 0; t < CHR_TILES; t++) {
        const u32 lma = ppu::ResolveTile(static_cast<u16>(t * 16));
        expand_tile(CHR_ROM + lma, g_chr4 + t * DS_TILE_SZ);
    }
}

// Rebuild the DS BG + sprite sub-palettes from paletteRAM. Sub-palette 0..3 hold
// the 4 BG / 4 sprite palettes; entry 0 of each is transparent. The universal
// backdrop is BG_PALETTE[0] (shown where every BG pixel is colour 0).
void build_palettes() {
    for (int p = 0; p < 4; p++) {
        for (int c = 1; c < 4; c++) {
            BG_PALETTE[p * 16 + c]     = to_bgr555(nes_rgb[paletteRAM[p * 4 + c]        & 0x3F]);
            SPRITE_PALETTE[p * 16 + c] = to_bgr555(nes_rgb[paletteRAM[0x10 + p * 4 + c] & 0x3F]);
        }
    }
    BG_PALETTE[0] = to_bgr555(nes_rgb[paletteRAM[0] & 0x3F]);
}

// Rebuild the BG tilemap shadow from the linked board's nametables in
// VideoRAM/cart VRAM (::ppu::ReadNametable). Mirrors the tile/attribute
// decode the software PPU and GX backend use; NES vertical wrap within one
// 240px row (30 tile-rows) is reproduced with `local_row % 30`, applied
// independently within each 32-row screenblock-half -- see build_map's own
// comment for why it can't be one combined period across both halves.
//
// activeRows -- not the fixed MAP_ROWS capacity -- bounds the loop: an
// ordinary 2-nametable board (::emu::NtGeometry::gridH == 1) only ever fills
// the first 32 DS tile-rows (one screenblock-row), the same footprint and
// cost this loop always had before four-screen support existed, and never
// calls ::ppu::ReadNametable with an offset past what that board's VideoRAM
// actually covers. geo.tileW/tileH are always exactly the NES-native 32/30 --
// never the DS's own 32x24 viewport (see TARGET_NDS's own viewport_tx()/
// viewport_ty() doc comments, video.hpp) -- so this stays exactly the
// screenblock-shaped walk it always was; ::emu::ComputeNtGeometry (emu.hpp)
// is just the one shared place that shape (and ::ppu::nametableCount's grid)
// now comes from, instead of this file's own copy of the formula.
void build_map() {
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

// Push the OAM shadow to the DS hardware OBJ list. y is +1 vs OAM (matches the
// PPU). NES sprite colour-0 is transparent (DS handles it). All 64 NES OAM slots
// map to OBJ 0..63; off-screen sprites are hidden.
void build_sprites() {
    const oam::sprite_t* oam = emu::OamShadow();
    const bool tall   = ppu::PPUCTRL & ppu::ctrl::SPRITE_SIZE;
    const int  height = tall ? 16 : 8;
    const int  atlas0 = (ppu::PPUCTRL & ppu::ctrl::SPRITE_ADDR) ? 256 : 0;

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
        const int sy = static_cast<int>(o.y) + 1 - voff;

        if (sx <= -8 || sx >= 256 || sy <= -height || sy >= SCREEN_H) {
            oamClearSprite(&oamMain, s);
            continue;
        }

        const int  pal    = o.attributes & 0x03;
        const bool behind = (o.attributes & 0x20) != 0;
        const bool hflip  = (o.attributes & 0x40) != 0;
        const bool vflip  = (o.attributes & 0x80) != 0;
        // 8x16: tile bit 0 selects the pattern table (overriding SPRITE_ADDR/atlas0);
        // libnds reads tile N then N+1 for a 1D-mapped 8x16 OBJ, matching the NES's
        // (tile & 0xFE)/(tile & 0xFE)+1 pair, so no extra per-row work is needed.
        const int  ti     = tall ? ((o.tile & 1) * 256 + (o.tile & 0xFE)) : (atlas0 + o.tile);

        oamSet(&oamMain, s, sx, sy,
               behind ? PRIO_SPR_BEHIND : PRIO_SPR_FRONT,
               pal, tall ? SpriteSize_8x16 : SpriteSize_8x8, SpriteColorFormat_16Color,
               oamGetGfxPtr(&oamMain, ti),
               -1, false, false, hflip, vflip, false);
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
    const int worldH = emu::ComputeNtGeometry().worldH;
    const int a = y0 < 0 ? 0 : y0;
    const int b = y1 > SCREEN_H ? SCREEN_H : y1;

    if (a <= SCREEN_H - 1 && b > SCREEN_H - 1)
        s_sprite_voff = static_cast<u16>(static_cast<int>(ys) - y0);

    for (int sy = a; sy < b; sy++) {
        const int w    = (static_cast<int>(ys) + (sy - y0)) % worldH;
        // Each 240-row nametable row starts on a 256-row screenblock boundary.
        const int phys = (w / 240) * 256 + (w % 240);
        sh_h[sy] = xs;
        sh_v[sy] = static_cast<u16>((phys - sy) & 0x1FF);
    }
}

void band_noop(int, int, u16, u16) {}

// HBlank interrupt: program the scroll for the NEXT scanline from the live
// table. Runs in IRQ mode -- touches only globals + memory-mapped registers.
void hblank_scroll() {
    const unsigned next = REG_VCOUNT + 1u;
    if (next < static_cast<unsigned>(SCREEN_H)) {
        REG_BG0HOFS = g_h[next];
        REG_BG0VOFS = g_v[next];
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
    const bool chr_dirty = g_chr_built_gen != ppu::chrGeneration;
    if (chr_dirty) build_chr_atlas();

    // Default the scroll table so disabled / empty scanlines are well-defined.
    for (int i = 0; i < SCREEN_H; i++) { sh_h[i] = 0; sh_v[i] = 0; }
    s_sprite_voff = 0;

    if (bg) build_map();

    // Walk the IRQ timeline (fires the game's scanline logic, incl. the sprite-0
    // split) and record each band's scroll. Done even when BG is off so the
    // handlers still run -- exactly like the GX/3DS backends.
    emu::GenerateBands(bg ? band_emit : band_noop, true);

    if (spr) build_sprites();
    else for (int s = 0; s < OAM_SPRITES; s++) oamClearSprite(&oamMain, s);

    // Pace to the hardware frame, then commit all VRAM writes during VBlank.
    swiWaitForVBlank();

    if (chr_dirty) {
        DC_FlushRange(g_chr4, sizeof g_chr4);
        dmaCopy(g_chr4, bgGetGfxPtr(g_bg),         sizeof g_chr4);
        dmaCopy(g_chr4, oamGetGfxPtr(&oamMain, 0), sizeof g_chr4);
        g_chr_built_gen = ppu::chrGeneration;
    }

    build_palettes();

    if (bg) {
        DC_FlushRange(g_map_shadow, sizeof g_map_shadow);
        dmaCopy(g_map_shadow, bgGetMapPtr(g_bg), sizeof g_map_shadow);
        bgShow(g_bg);
    } else {
        bgHide(g_bg);
    }

    // Publish the freshly built scroll table to the ISR, then seed line 0 (the
    // HBlank ISR only fills lines 1..191).
    memcpy(const_cast<u16*>(g_h), sh_h, sizeof sh_h);
    memcpy(const_cast<u16*>(g_v), sh_v, sizeof sh_v);
    REG_BG0HOFS = g_h[0];
    REG_BG0VOFS = g_v[0];

    oamUpdate(&oamMain);

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
    // world width; map at base 0, tiles at base 1 (16 KB). Size follows the
    // linked board's own ::ppu::nametableCount (video.hpp): 512x256 (two
    // horizontal NES nametables, one row) for an ordinary (2-nametable)
    // board, 512x512 (four screenblocks, a four-screen board's second
    // stacked row) otherwise -- see build_map's own comment for how
    // activeRows keeps an ordinary board's per-frame cost identical to
    // before this was possible at all.
    g_bg = bgInit(0, BgType_Text4bpp,
        ppu::nametableCount > 2 ? BgSize_T_512x512 : BgSize_T_512x256, 0, 1);
    bgSetPriority(g_bg, PRIO_BG);

    oamInit(&oamMain, SpriteMapping_1D_32, false);

    // Expand the CHR ROM to 4bpp and upload it to both BG and OBJ tile VRAM.
    // With 1D_32 OBJ mapping an 8x8 16-colour tile is exactly one 32-byte
    // slot, so the BG tile layout and the OBJ tile layout are identical and
    // one staged buffer feeds both. build_chr_atlas resolves through the
    // mapper's tile translator, so this also picks up NROM's default
    // (unbanked) CHR. The display isn't running yet, so (unlike the
    // per-frame path) the VRAM upload can happen immediately.
    build_chr_atlas();
    DC_FlushRange(g_chr4, sizeof g_chr4);
    dmaCopy(g_chr4, bgGetGfxPtr(g_bg),            sizeof g_chr4);
    dmaCopy(g_chr4, oamGetGfxPtr(&oamMain, 0),    sizeof g_chr4);
    g_chr_built_gen = ppu::chrGeneration;

    // video::vram_bytes() (video.hpp): the DS's 32x24 viewport is at or under
    // the NES's native 32x30, so this resolves to the NES-hardware minimum
    // (2 pages/0x800 bytes), like the NES hardware itself.
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
