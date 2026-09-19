/**
 * @file video.cpp
 * @brief GPU-native (PICA200) presentation backend for the Nintendo 3DS.
 *
 * The PPU emulation -- ::emu::GenerateBands and every nametable/attribute/
 * palette/OAM write function plus the scroll and sprite-zero machinery -- lives
 * in src/emu/ppu.cpp and is shared verbatim with the SDL and libogc backends.
 * This file is only the 3DS-specific half, and -- like the GameCube/Wii GX
 * backend -- it does NOT rasterise on the CPU: it hands the NES's own primitives
 * to the PICA200 via citro2d/citro3d.
 *
 * The PICA200 has no paletted-texture format, so the GX/CI4 tilemap+TLUT path
 * the GameCube/Wii backend uses cannot port directly. Instead the palette is
 * BAKED into the CHR atlas: each 8x8 NES tile becomes RGBA8 texels under a
 * concrete palette, giving four background atlases (palettes 0..3) and four
 * sprite atlases. Colour index 0 bakes to alpha 0, so standard alpha blending
 * reproduces NES transparency -- background colour-0 shows the backdrop clear,
 * sprite colour-0 is a hole. The atlases are rebuilt only when paletteRAM
 * actually changes; in this demo the palette is effectively static (the sprite-0
 * split moves *scroll*, not palette), so the per-pixel bake almost never runs.
 *
 * The frame is driven by ::emu::GenerateBands so the scanline IRQ handlers (the
 * game's per-frame logic, incl. the sprite-zero scroll split) still fire in
 * raster order, with NO per-pixel work on the ARM11. Painter order reproduces
 * NES priority: behind-sprites, then the background (colour-0 transparent), then
 * front-sprites. citro2d clears + writes colour only with a GEQUAL depth test
 * (no depth write), so submission order is the paint order and blended alpha-0
 * texels never occlude.
 *
 * The 3DS refreshes at 60Hz, so the GPU's VBlank present (C3D_FrameEnd with
 * C3D_FRAME_SYNCDRAW) paces the loop -- no software timer needed. With all
 * per-pixel work on the PICA200 this reaches native 60fps on an Old 3DS.
 */
#include "internal.hpp"

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

#include <platform-nes/video.hpp>
#include <platform-nes/interrupts.hpp>
#include "../emu/emu.hpp"
#include <cstring>

// ---------------------------------------------------------------------------
// 3DS-specific state. Everything the core needs (VideoRAM, paletteRAM, scroll,
// PPUCTRL/PPUMASK, the OAM snapshot, CHR_ROM, ...) is owned by src/emu; only the
// genuine citro2d/citro3d objects live here.
// ---------------------------------------------------------------------------
static constexpr int NES_H = 240;

// Top screen is a fixed 400x240 panel: exactly the NES's 240px height (scale
// 1), so video::viewport_tx() (video.hpp) resolves to 50 tiles/400px instead of
// the NES's native 32 -- the extra 18 tile columns are real game world, drawn
// native 1:1 like every other tile, so no stretch and no side bars are needed.
// Everything is still clipped to the viewport window first (see draw_bg_cell /
// draw_spr_tile) so partial edge tiles cut clean at the panel edge.

// CHR atlas geometry: 512 tiles, 16 per row -> 128x256 RGBA8 texels, one NES
// tile per 8x8 block. POT in both axes (PICA textures must be power-of-two).
static constexpr int ATLAS_TILES_X = 16;
static constexpr int ATLAS_W       = ATLAS_TILES_X * 8;          // 128
static constexpr int ATLAS_H       = (512 / ATLAS_TILES_X) * 8;  // 256

static C3D_RenderTarget *top;

// Four background + four sprite atlases, one per NES palette, palette baked in.
// Rebuilt (and re-uploaded) only when paletteRAM changes -- see maybe_rebuild().
static C3D_Tex   atlas_bg[4];
static C3D_Tex   atlas_spr[4];
static u32      *atlas_scratch;          // linear ARGB->tex staging (ATLAS_W*ATLAS_H)
static u8        palette_cache[32];      // last paletteRAM seen
static bool      palette_valid = false;
// Generation of ::ppu::chrGeneration the atlases were last baked from.
// Sentinel (not 0, ::chrGeneration's own initial value) forces the first bake.
static u32       chr_built_gen = ~0u;

// The fixed NES master palette (ARGB8888, 0xAARRGGBB), shared verbatim with the
// software PPU in src/emu/ppu.cpp; here it only seeds the baked atlases.
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

// Background cells gathered per band, bucketed by palette so each palette is one
// texture bind + a run of same-atlas draws. A single band can cover the WHOLE
// screen (no mid-frame IRQ split, e.g. the title screen) and, worse, can land
// every one of its cells in the SAME palette bucket -- title's
// ppu::Flush(chrEmpty_tile, 0xff) leaves most of the nametable at attribute
// 0xFF (palette 3), so that one bucket alone must hold a full band's worth of
// cells, not band_cells/4. Each bucket therefore needs capacity for
// (viewport width in tiles + 1) x (viewport height in tiles + 1) cells -- the
// +1s cover the partial edge tile a non-zero fine scroll adds on each axis.
// A too-small bound here overflows bg_bucket[3][MAX_CELLS] straight into
// adjacent static data (the atlas handles, palette_cache, ...), which reads
// as a mostly-blank screen with corrupted content and can crash much later,
// e.g. on shutdown when the corrupted state is finally touched again.
//
// Each cell carries its clip against both the band (vertical: r0/h/vy0) and the
// NES 256-wide window (horizontal: c0/w/sx) so partial edge or seam tiles cut
// clean -- no rotated-framebuffer scissor needed. sx/vy0 are the clipped NES
// top-left, c0/r0 the first visible tile column/row, w/h the visible size.
struct Cell { u16 tile; i16 sx; i16 vy0; u8 c0; u8 w; u8 r0; u8 h; };
static constexpr int MAX_CELLS = (video::viewport_tx() + 1) * (video::viewport_ty() + 1);
static Cell  bg_bucket[4][MAX_CELLS];
static int   bg_count[4];

// 0xAARRGGBB -> the byte order the PICA RGBA8 texture wants in linear memory,
// i.e. bytes [A,B,G,R] == a u32 of 0xRRGGBBAA (a left-rotate of the ARGB word by
// 8). Index-0/transparent passes 0 in and 0 out (alpha 0).
static inline u32 to_tex(const u32 argb) {
    return (argb << 8) | (argb >> 24);
}

// Bake all 512 tiles into the linear scratch under palette @p p. @p spr selects
// the sprite palette block (paletteRAM[0x10+]) vs the background block. Colour
// index 0 -> alpha 0 (transparent).
static void bake_atlas(u32 *buf, const int p, const bool spr) {
    const int base = (spr ? 0x10 : 0) + p * 4;
    for (int ti = 0; ti < 512; ti++) {
        const u8 *tile = CHR_ROM + ppu::ResolveTile(static_cast<u16>(ti * 16));
        const int ax = (ti & 15) * 8;
        const int ay = (ti >> 4) * 8;
        for (int ry = 0; ry < 8; ry++) {
            const u8 p0 = tile[ry];
            const u8 p1 = tile[ry + 8];
            u32 *dst = buf + (ay + ry) * ATLAS_W + ax;
            for (int rx = 0; rx < 8; rx++) {
                const int bit = 7 - rx;
                const u8  c   = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1);
                dst[rx] = c ? to_tex(nes_rgb[paletteRAM[base + c] & 0x3F]) : 0u;
            }
        }
    }
}

// Push the linear scratch into a tiled PICA texture: flush the CPU cache so the
// GPU sees the writes, then convert linear -> tiled (OUT_TILED) with no scaling.
// Must run outside C3D_FrameBegin/End.
static void upload_atlas(u32 *buf, C3D_Tex *tex) {
    GSPGPU_FlushDataCache(buf, ATLAS_W * ATLAS_H * sizeof(u32));
    C3D_SyncDisplayTransfer(
        buf,             GX_BUFFER_DIM(ATLAS_W, ATLAS_H),
        (u32 *)tex->data, GX_BUFFER_DIM(ATLAS_W, ATLAS_H),
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) |
        GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
        GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
}

// Rebuild every atlas iff paletteRAM or the mapper's CHR banks (MMC3 etc. --
// see ::ppu::chrGeneration) changed since the last frame. The bake + eight
// display transfers are the only per-pixel work in the backend, and they run
// only on a genuine palette or CHR-bank change (near-never in this demo).
// Must run outside C3D_FrameBegin/End.
static void maybe_rebuild_atlases() {
    const bool chr_dirty = chr_built_gen != ppu::chrGeneration;
    if (!chr_dirty && palette_valid && std::memcmp(palette_cache, paletteRAM, 32) == 0) return;
    for (int p = 0; p < 4; p++) {
        bake_atlas(atlas_scratch, p, false);
        upload_atlas(atlas_scratch, &atlas_bg[p]);
        bake_atlas(atlas_scratch, p, true);
        upload_atlas(atlas_scratch, &atlas_spr[p]);
    }
    std::memcpy(palette_cache, paletteRAM, 32);
    palette_valid  = true;
    chr_built_gen  = ppu::chrGeneration;
}

// citro2d UVs are bottom-up: v == 1.0 is texel row 0, so texel row R maps to
// v = 1 - R/ATLAS_H (matches the working full-frame subtex convention).
static inline float row_v(const int texel_row) {
    return 1.0f - (float)texel_row / ATLAS_H;
}

// Draw one background cell (already clipped to its band and the viewport
// window; never flipped). c0/w select the visible texel columns, r0/h the
// visible rows. Drawn at native 1:1 scale -- the viewport already spans the
// full 400px panel width (see video::viewport_tx() in video.hpp), so no
// stretch or centring offset is needed.
static inline void draw_bg_cell(C3D_Tex *tex, const Cell &c) {
    const int ax = (c.tile & 15) * 8 + c.c0;
    const int ay = (c.tile >> 4) * 8;
    const int top_row = ay + c.r0;
    Tex3DS_SubTexture sub;
    sub.width  = c.w;
    sub.height = c.h;
    sub.left   = (float)ax / ATLAS_W;
    sub.right  = (float)(ax + c.w) / ATLAS_W;
    sub.top    = row_v(top_row);
    sub.bottom = row_v(top_row + c.h);
    C2D_Image img{ tex, &sub };
    C2D_DrawImageAt(img, (float)c.sx, (float)c.vy0, 0.0f);
}

// Draw one 8x8 sprite tile, clipped to the viewport window; flipH/flipV swap
// the UVs. The visible screen sub-rectangle is mapped back to UVs by linear
// interpolation so a partial edge sprite shows exactly its on-window columns/rows
// (correct under flips too). Drawn at native 1:1 scale, like draw_bg_cell.
static inline void draw_spr_tile(C3D_Tex *tex, const int tile, const int sx,
                                 const int sy, const bool flipH, const bool flipV) {
    const int vx0 = sx < 0 ? 0 : sx;
    const int vx1 = (sx + 8) > video::viewport_px() ? video::viewport_px() : (sx + 8);
    const int vy0 = sy < 0 ? 0 : sy;
    const int vy1 = (sy + 8) > NES_H ? NES_H : (sy + 8);
    if (vx1 <= vx0 || vy1 <= vy0) return;

    const int ax = (tile & 15) * 8;
    const int ay = (tile >> 4) * 8;
    float ul = (float)ax / ATLAS_W, ur = (float)(ax + 8) / ATLAS_W;
    float vt = row_v(ay),           vb = row_v(ay + 8);
    if (flipH) { const float t = ul; ul = ur; ur = t; }
    if (flipV) { const float t = vt; vt = vb; vb = t; }

    // screen x in [sx, sx+8) maps linearly to u in [ul, ur); clamp to [vx0, vx1).
    const float fxl = (float)(vx0 - sx) / 8.0f, fxr = (float)(vx1 - sx) / 8.0f;
    const float fyl = (float)(vy0 - sy) / 8.0f, fyr = (float)(vy1 - sy) / 8.0f;

    Tex3DS_SubTexture sub;
    sub.width  = static_cast<u16>(vx1 - vx0);
    sub.height = static_cast<u16>(vy1 - vy0);
    sub.left   = ul + (ur - ul) * fxl;
    sub.right  = ul + (ur - ul) * fxr;
    sub.top    = vt + (vb - vt) * fyl;
    sub.bottom = vt + (vb - vt) * fyr;
    C2D_Image img{ tex, &sub };
    C2D_DrawImageAt(img, (float)vx0, (float)vy0, 0.0f);
}

// ::emu::band_emit_fn: draw the nametable for screen rows [y0,y1) at the given
// scroll. Background colour-0 is transparent so behind-sprites / backdrop show.
// Mirrors the OGC draw_bg_band tile walk; emits citro2d quads bucketed by palette.
static void draw_bg_band(const int y0, const int y1, const u16 xs, const u16 ys) {
    const int vpw      = video::viewport_px();
    // Shared with the SDL/desktop core (::emu::ComputeNtGeometry,
    // src/emu/emu.hpp) so this backend's read side stays byte-for-byte
    // consistent with the one write side (::ppu::CartesianToAddress's
    // internal xy_to_nt_addr/xy_to_at_addr, src/emu/ppu.cpp).
    const emu::NtGeometry geo = emu::ComputeNtGeometry();
    const int world_w  = geo.worldW;
    const int world_h  = geo.worldH;
    const int atlas0   = (ppu::PPUCTRL & ppu::ctrl::BG_ADDR) ? 256 : 0;

    bg_count[0] = bg_count[1] = bg_count[2] = bg_count[3] = 0;

    const int sx0 = -(static_cast<int>(xs) & 7);
    const int sy0 = y0 - (static_cast<int>(ys) & 7);

    for (int syt = sy0; syt < y1; syt += 8) {
        // Clip this tile row to the band so partial top/bottom tiles cut clean.
        const int vy0 = syt < y0 ? y0 : syt;
        const int vy1 = (syt + 8) > y1 ? y1 : (syt + 8);
        if (vy1 <= vy0) continue;
        const int r0  = vy0 - syt;          // first visible tile row (0..7)
        const int h   = vy1 - vy0;          // visible height (1..8)

        const int world_y   = static_cast<int>(ys) + (syt - y0);
        const int wym       = ((world_y % world_h) + world_h) % world_h;
        const int trow      = wym / 8;
        const int local_row = trow % geo.tileH;
        const int nt_row    = trow / geo.tileH;
        const int at_roff   = (local_row >> 2) * geo.atW;
        const int at_rbits  = ((local_row >> 1) & 1) * 4;
        const int row32     = local_row * geo.tileW;

        for (int sxt = sx0; sxt < vpw; sxt += 8) {
            // Clip this tile column to the NES window so a partial left/right tile
            // (non-zero fine-X scroll) cuts clean instead of bleeding into a bar.
            const int vx0 = sxt < 0 ? 0 : sxt;
            const int vx1 = (sxt + 8) > vpw ? vpw : (sxt + 8);
            if (vx1 <= vx0) continue;
            const int c0  = vx0 - sxt;          // first visible tile column (0..7)
            const int w   = vx1 - vx0;          // visible width (1..8)

            const int world_x   = static_cast<int>(xs) + sxt;
            const int wxm       = ((world_x % world_w) + world_w) % world_w;
            const int tcol      = wxm / 8;
            const int local_col = tcol % geo.tileW;
            const int nt_col    = tcol / geo.tileW;
            const int nt_off    = (nt_col + nt_row * geo.gridW) * geo.pageBytes;

            const u8 tile_id = ppu::ReadNametable(static_cast<u16>(nt_off + row32 + local_col));
            const u8 attr    = ppu::ReadNametable(static_cast<u16>(nt_off + geo.ntBytes + at_roff + (local_col >> 2)));
            const int pal    = (attr >> (((local_col >> 1) & 1) * 2 + at_rbits)) & 3;

            Cell &cell = bg_bucket[pal][bg_count[pal]++];
            cell.tile  = static_cast<u16>(atlas0 + tile_id);
            cell.sx    = static_cast<i16>(vx0);
            cell.vy0   = static_cast<i16>(vy0);
            cell.c0    = static_cast<u8>(c0);
            cell.w     = static_cast<u8>(w);
            cell.r0    = static_cast<u8>(r0);
            cell.h     = static_cast<u8>(h);
        }
    }

    for (int p = 0; p < 4; p++)
        for (int i = 0; i < bg_count[p]; i++)
            draw_bg_cell(&atlas_bg[p], bg_bucket[p][i]);
}

// Draw the visible sprites of the requested priority class, bucketed by palette.
// NES sprite colour-0 is transparent (baked alpha 0). y is +1 vs OAM (matches
// the PPU). Mirrors OGC draw_sprites.
static void draw_sprites(const bool behind) {
    const oam::sprite_t *oam = emu::OamShadow();
    const int  vpw    = video::viewport_px();
    const bool tall   = ppu::PPUCTRL & ppu::ctrl::SPRITE_SIZE;
    const int  height = tall ? 16 : 8;
    const int  atlas0 = (ppu::PPUCTRL & ppu::ctrl::SPRITE_ADDR) ? 256 : 0;

    for (int p = 0; p < 4; p++) {
        for (int s = 0; s < OAM_SPRITES; s++) {
            const oam::sprite_t &o = oam[s];
            if ((o.attributes & 0x03) != p)            continue;
            if (((o.attributes & 0x20) != 0) != behind) continue;
            const int sx = static_cast<int>(o.x);
            const int sy = static_cast<int>(o.y) + 1;
            if (sx <= -8 || sx >= vpw || sy <= -height || sy >= 240) continue;
            const bool flipH = (o.attributes & 0x40) != 0;
            const bool flipV = (o.attributes & 0x80) != 0;
            if (!tall) {
                draw_spr_tile(&atlas_spr[p], atlas0 + o.tile, sx, sy, flipH, flipV);
                continue;
            }
            // 8x16: tile bit 0 selects the pattern table (overriding SPRITE_ADDR),
            // tile & 0xFE is the top-half tile of the pair. A vertical flip swaps
            // which half draws on top as well as flipping each half's rows.
            const int pair = (o.tile & 1) * 256 + (o.tile & 0xFE);
            draw_spr_tile(&atlas_spr[p], pair + (flipV ? 1 : 0), sx, sy,          flipH, flipV);
            draw_spr_tile(&atlas_spr[p], pair + (flipV ? 0 : 1), sx, sy + 8,      flipH, flipV);
        }
    }
}

static void band_noop(int, int, u16, u16) {}

namespace video {

void WaitForPresent() {
    // Honour the HOME menu / power button: aptMainLoop() goes false when the
    // app should exit, which drives the demo's `while (!quit)` loop the same way
    // SDL_EVENT_QUIT does on desktop.
    if (!aptMainLoop()) quit = 1;

    const bool bg  = ppu::PPUMASK & ppu::mask::BG;
    const bool spr = ppu::PPUMASK & ppu::mask::SPRITE;

    // Rebuild the baked atlases if the palette moved (outside FrameBegin/End).
    if (bg || spr) maybe_rebuild_atlases();

    // Backdrop = universal background colour (paletteRAM[0]) when rendering, else
    // black. citro2d also clears depth here; the GEQUAL/no-depth-write state then
    // makes submission order the paint order.
    u32 clear;
    if (bg || spr) {
        const u32 bd = nes_rgb[paletteRAM[0] & 0x3F];
        clear = C2D_Color32((bd >> 16) & 0xFF, (bd >> 8) & 0xFF, bd & 0xFF, 0xFF);
    } else {
        clear = C2D_Color32(0, 0, 0, 0xFF);
    }

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(top, clear);
    C2D_SceneBegin(top);

    if (bg || spr) {
        // NES priority order: behind-sprites under the background, the
        // background (colour-0 transparent) over them, front-sprites on top.
        // GenerateBands also walks the IRQ timeline (running game logic).
        if (spr) draw_sprites(true);
        emu::GenerateBands(bg ? draw_bg_band : band_noop);
        if (spr) draw_sprites(false);
    } else {
        // Rendering off: still walk the timeline so the IRQ handlers (game
        // logic) fire; the screen stays at the black clear.
        emu::GenerateBands(band_noop);
    }

    C3D_FrameEnd(0);

    /* No IRQs permitted post-frame; discard anything still queued from this
     * frame's render before NMI enqueues for the next one. */
    irq::irqPendingValid = false;
    nmi_vector();
}

}   // namespace video

// init()/post() are the global library lifecycle hooks the RESET macro expands
// to (declared in interrupts.hpp), so they live at global scope -- not inside
// namespace video.
void irq::init() {
    gfxInitDefault();

    // The PICA200 does all per-pixel work now (tiles/sprites are textured quads,
    // not a CPU raster), so the ARM11 is no longer the frame-budget hot spot.
    // The New 3DS/2DS speedup (804MHz + extra L2) is still requested as cheap
    // insurance -- it is a harmless no-op on an Old 3DS, where the GPU-native
    // path already reaches native 60fps.
    osSetSpeedupEnable(true);

    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);

    // Eight palette-baked CHR atlases (4 bg + 4 spr), 128x256 RGBA8, nearest-
    // neighbour so the NES pixels stay crisp when citro2d scales the quads.
    for (int p = 0; p < 4; p++) {
        C3D_TexInit(&atlas_bg[p],  ATLAS_W, ATLAS_H, GPU_RGBA8);
        C3D_TexInit(&atlas_spr[p], ATLAS_W, ATLAS_H, GPU_RGBA8);
        C3D_TexSetFilter(&atlas_bg[p],  GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetFilter(&atlas_spr[p], GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&atlas_bg[p],  GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        C3D_TexSetWrap(&atlas_spr[p], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    }

    atlas_scratch = (u32 *)linearAlloc(ATLAS_W * ATLAS_H * sizeof(u32));

    // video::vram_bytes() (video.hpp): the 3DS's 50-tile-wide viewport is wider
    // than the NES's native 32, so this resolves to double the NES-hardware
    // minimum (4 pages/0x1000 bytes), not the stock 2.
    emu::InitMemory(video::vram_bytes());
}

void irq::post() {
    linearFree(atlas_scratch);
    for (int p = 0; p < 4; p++) {
        C3D_TexDelete(&atlas_bg[p]);
        C3D_TexDelete(&atlas_spr[p]);
    }
    C2D_Fini();
    C3D_Fini();
    gfxExit();
}
