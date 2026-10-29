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
 * sprite colour-0 is a hole.
 *
 * BOTH PANELS are presented: the viewport is the two screens stacked (50x60
 * tiles / 400x480px, video.hpp's TARGET_CTR branch) and ::video::panel_rect
 * says where each one sits inside it -- the 400x240 top screen at the origin and
 * the 320x240 bottom screen inset 40px (5 tiles) on each side. Each band and
 * each sprite is drawn once per panel it overlaps, clipped and translated into
 * that panel's own render target, so a background crossing the seam is
 * continuous and a sprite can straddle it. A sprite parks by taking a Y past
 * the whole 480-row viewport (::ParkedSpriteY, demo/src/main.hpp) -- 240 is a
 * real, visible row now: the first row of the bottom screen.
 *
 * The frame is driven by ::emu::GenerateBands so the scanline IRQ handlers (the
 * game's per-frame logic, incl. the sprite-zero scroll split) still fire in
 * raster order, with NO per-pixel work on the ARM11. Painter order reproduces
 * NES priority, per panel: behind-sprites, then the background (colour-0
 * transparent), then front-sprites. citro2d clears + writes colour only with a
 * GEQUAL depth test (no depth write), so submission order is the paint order and
 * blended alpha-0 texels never occlude.
 *
 * MID-FRAME CHR BANKING is why the frame is walked in two passes. The game banks
 * CHR from inside a scanline IRQ handler -- the demo hands the top panel the
 * level preview's bank and the bottom panel the UI's, both mapped at the SAME
 * PPU window -- so one atlas set per frame cannot serve a frame, and an atlas
 * upload (a GX display transfer) may not be issued between C3D_FrameBegin and
 * C3D_FrameEnd. So ::video::WaitForPresent first PLANS the frame outside it:
 * GenerateBands runs the game logic once, each band records its scroll plus the
 * CHR atlas set its scanlines resolve to, and every set the frame needs is baked
 * and uploaded there. The second pass then just replays those band records into
 * the two targets. Sets are cached by the resolved CHR image itself (the 512
 * tile base addresses), NOT by ::ppu::chrGeneration -- the generation counter
 * moves every frame in this demo, so keying on it re-baked all eight atlases
 * every frame, which is a megabyte of ARM11 texel writes plus eight synchronous
 * display transfers and exactly what held the game at 30fps. Keyed on content, a
 * frame that alternates between two bank states bakes each one once, ever.
 *
 * The 3DS refreshes at 60Hz, so the GPU's VBlank present (C3D_FrameEnd with
 * C3D_FRAME_SYNCDRAW) paces the loop -- no software timer needed. With all
 * per-pixel work on the PICA200 this reaches native 60fps on an Old 3DS.
 */
#include "internal.hpp"

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

#include <pnes/video.hpp>
#include <pnes/interrupts.hpp>
#include "../emu/emu.hpp"
#include <cstring>

// ---------------------------------------------------------------------------
// 3DS-specific state. Everything the core needs (VideoRAM, paletteRAM, scroll,
// PPUCTRL/PPUMASK, the OAM snapshot, CHR_ROM, ...) is owned by src/emu; only the
// genuine citro2d/citro3d objects live here.
// ---------------------------------------------------------------------------
// The viewport is both panels stacked: 50x60 tiles / 400x480px (video.hpp's
// TARGET_CTR branch), the top screen's 400x240 plus the 320x240 bottom one
// inset 40px on each side (::video::panel_rect). Those extra 18 tile columns
// and 30 tile rows over the NES's native 32x30 are real game world, drawn
// native 1:1 like every other tile, so no stretch and no bars are needed.
// Everything is clipped to the PANEL being drawn (see draw_bg_cell /
// draw_spr_tile) so partial edge tiles cut clean at each screen's edge.

// CHR atlas geometry: 512 tiles, 16 per row -> 128x256 RGBA8 texels, one NES
// tile per 8x8 block. POT in both axes (PICA textures must be power-of-two).
static constexpr int ATLAS_TILES_X = 16;
static constexpr int ATLAS_W       = ATLAS_TILES_X * 8;          // 128
static constexpr int ATLAS_H       = (512 / ATLAS_TILES_X) * 8;  // 256

// One render target per panel, indexed by ::video::panel_rect's own index:
// 0 == GFX_TOP (400x240), 1 == GFX_BOTTOM (320x240).
static C3D_RenderTarget *panel_target[2];

// An atlas set: the eight palette-baked atlases (4 background + 4 sprite) for
// ONE CHR bank state. `bases` is what identifies it -- the physical CHR address
// every one of the 512 PPU tile slots resolved to when the set was claimed (see
// snapshot_bases) -- so a set is reused whenever the banks resolve to the same
// image again, however many times ::ppu::chrGeneration has moved in between.
// Textures are allocated and baked lazily, per palette: a bank state used by
// one palette costs one atlas, not eight.
struct AtlasSet {
    C3D_Tex tex[8];          // [0..3] background palettes, [4..7] sprite palettes
    bool    inited[8];       // texture memory allocated
    bool    baked[8];        // contents match `bases` + the live palette
    u32     bases[512];      // ::ppu::ResolveTile(ti * 16) for every tile slot
    bool    valid;           // `bases` holds a real state
    u32     used;            // frame stamp of last use, for the LRU eviction
};
// Three sets: a frame needs one per distinct bank state it shows (the demo uses
// two -- preview/HUD above the split, UI below it), and the third keeps a mode
// change from evicting a state that is about to come straight back.
static constexpr int CHR_SETS = 3;
static AtlasSet  chr_set[CHR_SETS];
static u32       frame_stamp;            // ++ per frame, the LRU clock
static u32       cur_bases[512];         // scratch for snapshot_bases
static u32      *atlas_scratch;          // linear ARGB->tex staging (ATLAS_W*ATLAS_H)
static u8        palette_cache[32];      // last paletteRAM seen
static bool      palette_valid = false;

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
// Sized from the LARGEST PANEL, not the whole viewport: the viewport is both
// screens stacked (60 rows/480px, see video.hpp's TARGET_CTR branch), but a
// band is gathered and flushed once per PANEL it overlaps (draw_bg_band clips
// it to one panel's rect), so no single pass can exceed the biggest panel --
// the 400x240 top screen, 50x30 tiles. Sizing this at the full 61 rows would
// trade ~90KB of .bss for cells that are never emitted.
static constexpr int MAX_CELLS = (video::panel_tx(0) + 1) * (video::panel_ty(0) + 1);
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
//
// Tile data comes from @p bases -- the OWNING SET's recorded CHR addresses --
// never from a live ::ppu::ResolveTile call: a set is baked when the frame plan
// discovers it needs it, which is not necessarily while that set's banks are
// still the ones switched in.
static void bake_atlas(u32 *buf, const u32 *bases, const int p, const bool spr) {
    const int base = (spr ? 0x10 : 0) + p * 4;
    for (int ti = 0; ti < 512; ti++) {
        const u8 *tile = CHR_ROM + bases[ti];
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

// Resolve the CHR address of all 512 PPU tile slots as the banks stand right
// now. This, not ::ppu::chrGeneration, is a bank state's identity: the counter
// moves on every register write (so it differs every frame once a game banks
// per-frame, even when the resulting image is one it has shown a thousand times)
// while this is exactly the input the bake consumes. Per-tile rather than per
// bank window so no mapper's window granularity is assumed -- it is the same
// question ::emu::GenerateFrame asks on every single pixel fetch.
static void snapshot_bases(u32 *out) {
    for (int ti = 0; ti < 512; ti++)
        out[ti] = ppu::ResolveTile(static_cast<u16>(ti * 16));
}

// The atlas set for the banks as they stand right now, claiming (and if need be
// evicting) one if this state is not cached. Baking is NOT done here -- see
// ensure_atlas, which the frame plan calls for the palettes it actually needs.
static int chr_set_now() {
    snapshot_bases(cur_bases);

    for (int i = 0; i < CHR_SETS; i++)
        if (chr_set[i].valid && std::memcmp(chr_set[i].bases, cur_bases, sizeof cur_bases) == 0) {
            chr_set[i].used = frame_stamp;
            return i;
        }

    // Claim a free slot, else the least recently used one. `used` is a frame
    // stamp, so an eviction takes a state an EARLIER frame used in preference to
    // one this frame has already planned a band against. A frame showing more
    // than CHR_SETS distinct bank states would run out and evict one of its own
    // anyway: those bands then draw from the state that replaced theirs (wrong
    // tiles, never garbage -- the replacement is baked before anything is
    // drawn), and the sets thrash. Two is what the demo needs; the third is the
    // margin, and the way to buy more is another slot, not another policy.
    int victim = 0;
    for (int i = 0; i < CHR_SETS; i++) {
        if (!chr_set[i].valid) { victim = i; break; }
        if (chr_set[i].used < chr_set[victim].used) victim = i;
    }

    AtlasSet &set = chr_set[victim];
    std::memcpy(set.bases, cur_bases, sizeof cur_bases);
    set.valid = true;
    set.used  = frame_stamp;
    for (int k = 0; k < 8; k++) set.baked[k] = false;   // textures kept, contents stale
    return victim;
}

// Make set @p si's atlas for palette @p p (@p spr: the sprite palette block)
// ready to draw from, baking and uploading it if it is not already.
// Must run outside C3D_FrameBegin/End (upload_atlas issues a display transfer).
static C3D_Tex *ensure_atlas(const int si, const int p, const bool spr) {
    AtlasSet &set = chr_set[si];
    const int k = (spr ? 4 : 0) + p;
    C3D_Tex *tex = &set.tex[k];

    if (!set.inited[k]) {
        // Nearest-neighbour so the NES pixels stay crisp, clamped so a
        // partial-tile subtexture never samples its neighbour.
        C3D_TexInit(tex, ATLAS_W, ATLAS_H, GPU_RGBA8);
        C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        set.inited[k] = true;
    }
    if (!set.baked[k]) {
        bake_atlas(atlas_scratch, set.bases, p, spr);
        upload_atlas(atlas_scratch, tex);
        set.baked[k] = true;
    }
    return tex;
}

// A palette write invalidates every baked atlas (the palette is baked into the
// texels) but not the sets themselves: their CHR identity is unchanged, so they
// keep their `bases` and are simply re-baked on demand.
// The palette is read once per frame, here: a mid-frame palette change is not
// modelled on this backend (the demo's fades run in the NMI, and the raster
// splits move scroll and banks, never colours).
static void note_palette() {
    if (palette_valid && std::memcmp(palette_cache, paletteRAM, 32) == 0) return;
    std::memcpy(palette_cache, paletteRAM, 32);
    palette_valid = true;
    for (auto &set : chr_set)
        for (int k = 0; k < 8; k++) set.baked[k] = false;
}

// citro2d UVs are bottom-up: v == 1.0 is texel row 0, so texel row R maps to
// v = 1 - R/ATLAS_H (matches the working full-frame subtex convention).
static inline float row_v(const int texel_row) {
    return 1.0f - (float)texel_row / ATLAS_H;
}

// Draw one background cell (already clipped to its band and to the panel it is
// being drawn on; never flipped). c0/w select the visible texel columns, r0/h
// the visible rows. sx/vy0 are VIEWPORT coordinates, so @p pr -- the panel's
// rect inside the viewport -- is subtracted to reach the target's own space.
// Drawn at native 1:1 scale; both panels have the same pixel pitch, so no
// stretch or centring offset is needed.
static inline void draw_bg_cell(C3D_Tex *tex, const Cell &c, const video::PanelRect &pr) {
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
    C2D_DrawImageAt(img, (float)(c.sx - pr.x), (float)(c.vy0 - pr.y), 0.0f);
}

// Draw one 8x8 sprite tile, clipped to panel @p pr; flipH/flipV swap the UVs.
// The visible screen sub-rectangle is mapped back to UVs by linear interpolation
// so a partial edge sprite shows exactly its on-panel columns/rows (correct
// under flips too). Drawn at native 1:1 scale, like draw_bg_cell.
//
// The clip is the panel's own rect, not the viewport's: a sprite straddling the
// screen seam is drawn once per panel, each time showing only its rows on that
// one, and a sprite whose columns fall in the bezel beside the narrower bottom
// panel is cut there exactly as it is at the viewport's outer edge.
static inline void draw_spr_tile(C3D_Tex *tex, const int tile, const int sx,
                                 const int sy, const bool flipH, const bool flipV,
                                 const video::PanelRect &pr) {
    const int px1 = pr.x + pr.w, py1 = pr.y + pr.h;
    const int vx0 = sx < pr.x ? pr.x : sx;
    const int vx1 = (sx + 8) > px1 ? px1 : (sx + 8);
    const int vy0 = sy < pr.y ? pr.y : sy;
    const int vy1 = (sy + 8) > py1 ? py1 : (sy + 8);
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
    C2D_DrawImageAt(img, (float)(vx0 - pr.x), (float)(vy0 - pr.y), 0.0f);
}

// One band as the plan pass recorded it: the scanline span, the scroll in
// effect across it, and the CHR atlas set its tiles resolve through.
struct BandRec { i16 y0, y1; u16 xs, ys; u8 set; };
// A band per scanline split, so one per IRQ the frame fires. The demo arms at
// most a couple (the HUD/panel split); the cap only bounds the .bss.
static constexpr int MAX_BANDS = 64;
static BandRec band_rec[MAX_BANDS];
static int     band_n;

// ::emu::band_emit_fn for the PLAN pass -- see this file's header comment. Draws
// nothing: it records the band and resolves the CHR state its scanlines are
// sourced from, which is the state the IRQ handler that opened the band left
// behind. Runs outside C3D_FrameBegin/End, so the bake ensure_atlas may do here
// is legal.
static void plan_band(const int y0, const int y1, const u16 xs, const u16 ys) {
    const int si = chr_set_now();
    for (int p = 0; p < 4; p++) ensure_atlas(si, p, false);

    if (band_n == MAX_BANDS) {
        // Out of records: extend the last band over these rows rather than drop
        // them. They then draw with the previous band's scroll and bank, which
        // is wrong but visible -- the alternative is a blank stripe. Unreachable
        // with any sane split count.
        band_rec[band_n - 1].y1 = static_cast<i16>(y1);
        return;
    }
    band_rec[band_n++] = BandRec{ static_cast<i16>(y0), static_cast<i16>(y1), xs, ys,
                                  static_cast<u8>(si) };
}

// Draw one recorded band's share of panel @p pr: the nametable for the rows the
// band and the panel have in common, at the band's scroll, out of the band's own
// atlas set. Background colour-0 is transparent so behind-sprites / backdrop
// show. Mirrors the OGC draw_bg_band tile walk; emits citro2d quads bucketed by
// palette.
static void draw_bg_band(const BandRec &b, const video::PanelRect &pr) {
    // The band's rows clipped to this panel. The tile walk still anchors on the
    // band's OWN first row (b.y0, the row b.ys is the scroll for), so clipping
    // moves which rows are emitted and nothing about the scroll maths.
    const int anchor = b.y0;
    const int y0     = b.y0 < pr.y ? pr.y : b.y0;
    const int y1     = b.y1 > (pr.y + pr.h) ? (pr.y + pr.h) : b.y1;
    if (y1 <= y0) return;

    const u16 xs = b.xs, ys = b.ys;
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
    const int sy0 = anchor - (static_cast<int>(ys) & 7);

    for (int syt = sy0; syt < y1; syt += 8) {
        // Clip this tile row to the band so partial top/bottom tiles cut clean.
        const int vy0 = syt < y0 ? y0 : syt;
        const int vy1 = (syt + 8) > y1 ? y1 : (syt + 8);
        if (vy1 <= vy0) continue;
        const int r0  = vy0 - syt;          // first visible tile row (0..7)
        const int h   = vy1 - vy0;          // visible height (1..8)

        const int world_y   = static_cast<int>(ys) + (syt - anchor);
        const int wym       = ((world_y % world_h) + world_h) % world_h;
        const int trow      = wym / 8;
        const int local_row = trow % geo.tileH;
        const int nt_row    = trow / geo.tileH;
        const int at_roff   = (local_row >> 2) * geo.atW;
        const int at_rbits  = ((local_row >> 1) & 1) * 4;
        const int row32     = local_row * geo.tileW;

        for (int sxt = sx0; sxt < vpw; sxt += 8) {
            // Clip this tile column to the PANEL so a partial left/right tile
            // (non-zero fine-X scroll, or the bottom screen's 40px inset) cuts
            // clean instead of bleeding into a bar or into the bezel.
            const int vx0 = sxt < pr.x ? pr.x : sxt;
            const int vx1 = (sxt + 8) > (pr.x + pr.w) ? (pr.x + pr.w) : (sxt + 8);
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

    for (int p = 0; p < 4; p++) {
        // Unbaked can only mean this band's set was evicted later in the plan
        // (see chr_set_now) -- draw nothing rather than sample a texture whose
        // texels belong to no state at all.
        if (!chr_set[b.set].baked[p]) continue;
        for (int i = 0; i < bg_count[p]; i++)
            draw_bg_cell(&chr_set[b.set].tex[p], bg_bucket[p][i], pr);
    }
}

// Which sprite palettes the OAM shadow actually uses, as a bit per palette.
// The plan pass bakes only these: a sprite atlas for a palette no sprite is in
// is a megabyte of texel writes for nothing.
static u8 sprite_palette_mask() {
    const oam::sprite_t *oam = emu::OamShadow();
    u8 mask = 0;
    for (int s = 0; s < OAM_SPRITES; s++) mask |= static_cast<u8>(1u << (oam[s].attributes & 3));
    return mask;
}

// Draw the visible sprites of the requested priority class on panel @p pr, out
// of atlas set @p si, bucketed by palette. NES sprite colour-0 is transparent
// (baked alpha 0). y is +1 vs OAM (matches the PPU). Mirrors OGC draw_sprites.
static void draw_sprites(const bool behind, const video::PanelRect &pr, const int si) {
    const oam::sprite_t *oam = emu::OamShadow();
    const int  vpy    = video::viewport_py();
    const int  vpw    = video::viewport_px();
    const bool tall   = ppu::PPUCTRL & ppu::ctrl::SPRITE_SIZE;
    // The cheap reject here is the two FAR edges of the WHOLE VIEWPORT; the
    // per-panel clip inside draw_spr_tile does the rest, including the seam and
    // the bottom screen's bezel inset. OAM coordinates are unsigned on every
    // target (::oam::oam_t) and real NES hardware cannot overhang the left or
    // top edge either, so there is no negative case to test -- and unlike the
    // GBA backend this one applies no window transform to sprite Y, so nothing
    // can push an on-screen sprite negative afterwards. Nor does the park band
    // need a test of its own: ::ParkedSpriteY (demo/src/main.hpp) is a row past
    // viewport_py(), which is what this rejects. Note that 240 is NOT such a
    // row here -- it is the first row of the bottom screen.
    const int  atlas0 = (ppu::PPUCTRL & ppu::ctrl::SPRITE_ADDR) ? 256 : 0;

    for (int p = 0; p < 4; p++) {
        C3D_Tex *tex = &chr_set[si].tex[4 + p];
        if (!chr_set[si].baked[4 + p]) continue;   // no sprite uses this palette
        for (int s = 0; s < OAM_SPRITES; s++) {
            const oam::sprite_t &o = oam[s];
            if ((o.attributes & 0x03) != p)            continue;
            if (((o.attributes & 0x20) != 0) != behind) continue;
            const int sx = static_cast<int>(o.x);
            const int sy = static_cast<int>(o.y) + 1;
            if (sx >= vpw || sy >= vpy) continue;
            const bool flipH = (o.attributes & 0x40) != 0;
            const bool flipV = (o.attributes & 0x80) != 0;
            if (!tall) {
                draw_spr_tile(tex, atlas0 + o.tile, sx, sy, flipH, flipV, pr);
                continue;
            }
            // 8x16: tile bit 0 selects the pattern table (overriding SPRITE_ADDR),
            // tile & 0xFE is the top-half tile of the pair. A vertical flip swaps
            // which half draws on top as well as flipping each half's rows.
            const int pair = (o.tile & 1) * 256 + (o.tile & 0xFE);
            draw_spr_tile(tex, pair + (flipV ? 1 : 0), sx, sy,     flipH, flipV, pr);
            draw_spr_tile(tex, pair + (flipV ? 0 : 1), sx, sy + 8, flipH, flipV, pr);
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

    frame_stamp++;

    // ---- PASS 1: plan ----------------------------------------------------
    // Everything that may touch the GPU outside a frame -- the palette check and
    // every atlas bake/upload -- happens here, before C3D_FrameBegin, and this
    // is also the pass that runs the game's own per-frame logic (GenerateBands
    // fires the scanline IRQ handlers). See this file's header comment.
    band_n = 0;
    int spr_set_behind = 0, spr_set_front = 0;

    if (bg || spr) {
        note_palette();
        // Sprites are drawn in two batches that bracket the bands, so they take
        // the bank state at those two moments: whatever the NMI left for the
        // behind batch, whatever the last handler left for the front one.
        spr_set_behind = chr_set_now();
        emu::GenerateBands(bg ? plan_band : band_noop);
        spr_set_front  = chr_set_now();

        if (spr) {
            const u8 mask = sprite_palette_mask();
            for (int p = 0; p < 4; p++) {
                if (!(mask & (1u << p))) continue;
                ensure_atlas(spr_set_behind, p, true);
                ensure_atlas(spr_set_front,  p, true);
            }
        }
    } else {
        // Rendering off: still walk the timeline so the IRQ handlers (game
        // logic) fire; the screens stay at the black clear.
        emu::GenerateBands(band_noop);
    }

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

    // ---- PASS 2: draw ----------------------------------------------------
    // One target per panel, each getting the share of every band and every
    // sprite that lands on it. NES priority order within a panel: behind-sprites
    // under the background, the background (colour-0 transparent) over them,
    // front-sprites on top.
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    for (u8 i = 0; i < video::panel_count(); i++) {
        C2D_TargetClear(panel_target[i], clear);
        C2D_SceneBegin(panel_target[i]);
        if (!(bg || spr)) continue;

        const video::PanelRect pr = video::panel_rect(i);
        if (spr) draw_sprites(true, pr, spr_set_behind);
        if (bg)  for (int b = 0; b < band_n; b++) draw_bg_band(band_rec[b], pr);
        if (spr) draw_sprites(false, pr, spr_set_front);
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
    // Both panels' worth of quads now go through citro2d in one frame: the top
    // screen's 51x31 cells plus the bottom's 41x31, plus up to 128 sprite tiles
    // per panel -- past the 4096-object default, and an object past the budget
    // is dropped silently (a blank stripe wherever the frame ran out).
    C2D_Init(8192);
    C2D_Prepare();

    // One target per panel: the viewport spans both screens (video.hpp's
    // TARGET_CTR branch), and ::video::panel_rect indexes them in this order.
    panel_target[0] = C2D_CreateScreenTarget(GFX_TOP,    GFX_LEFT);
    panel_target[1] = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    // The atlases themselves are allocated and baked on demand, per CHR bank
    // state and per palette (see ensure_atlas) -- nothing to do up front.

    atlas_scratch = (u32 *)linearAlloc(ATLAS_W * ATLAS_H * sizeof(u32));

    // video::vram_bytes() (video.hpp): the 3DS's 50-tile-wide viewport is wider
    // than the NES's native 32, so this resolves to double the NES-hardware
    // minimum (4 pages/0x1000 bytes), not the stock 2.
    emu::InitMemory(video::vram_bytes());
}

void irq::post() {
    linearFree(atlas_scratch);
    for (auto &set : chr_set)
        for (int k = 0; k < 8; k++)
            if (set.inited[k]) C3D_TexDelete(&set.tex[k]);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
}
