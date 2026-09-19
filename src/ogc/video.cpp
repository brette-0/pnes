/**
 * @file video.cpp
 * @brief GX-native (Flipper/Hollywood) presentation backend for GameCube + Wii.
 *
 * The NES PPU emulation is platform-agnostic and lives in src/emu/; this file is
 * only the GameCube/Wii-specific half, and it does NOT rasterise on the CPU. It
 * hands the NES's own primitives to GX:
 *
 *   - The CHR ROM becomes a GX_TF_CI4 (4bpp paletted) atlas: each 8x8 NES tile
 *     is one CI4 block, so all 512 tiles fit a 128x256 atlas (built once).
 *   - paletteRAM becomes GX_TL_RGB5A3 TLUTs (4 background + 4 sprite), rebuilt
 *     each frame. Colour index 0 maps to RGB5A3 0x0000 (alpha 0); an alpha-test
 *     discard reproduces NES transparency -- background colour-0 shows the
 *     backdrop clear, sprite colour-0 is a hole.
 *   - The nametable is drawn as a grid of textured quads and the visible sprites
 *     as up to 64 more; Flipper rasterises and scales them to the TV.
 *
 * The frame is driven by ::emu::GenerateBands so the scanline IRQ handlers (the
 * game's per-frame logic, incl. the sprite-zero scroll split) still fire in
 * raster order. Painter order reproduces NES priority: behind-sprites, then the
 * background (colour-0 transparent), then front-sprites.
 *
 * The GX-native tiles/sprites live in NES pixel space (256x240) and let the
 * viewport scale them to the TV. The development profiler overlay (Debug builds
 * only, gated by OGC_PROFILER) lives in screen-pixel space; its projection and
 * draw path compile out of Release .dols entirely.
 *
 * Frame pacing is the real VBlank (VIDEO_WaitVSync), double-buffered.
 */
#include "internal.hpp"

#include <platform-nes/video.hpp>
#include <platform-nes/interrupts.hpp>
#include "../emu/emu.hpp"
#if defined(OGC_PROFILER)
#include "profiler.hpp"
#endif

#include <ogc/gx.h>
#include <ogc/gu.h>
#include <ogc/pad.h>
#include <ogc/system.h>
#include <malloc.h>
#include <string.h>
#ifdef TARGET_WII
#include <wiiuse/wpad.h>
#endif

// ---------------------------------------------------------------------------
// Wii shutdown handshake.
//
// A Wii title is expected to answer IOS/STM power-down and reset requests
// itself; nothing here did, so the GameCube-oriented "loop forever, the
// loader/BIOS reclaims the machine on exit" model left the Wii side with no
// way to ever satisfy that handshake. On real hardware this mostly goes
// unnoticed (the HOME menu still works). In Dolphin, choosing Stop for a Wii
// title sends that same STM power-button event and then waits for the title
// to call SYS_ResetSystem in response -- which never happened, so Dolphin
// sits on "Shutting Down" forever. The GameCube target has no such handshake
// (Dolphin just halts the CPU on Stop), which is why only Wii is affected.
//
// The callbacks below just set the existing `quit` flag so the ordinary
// `while (!quit)` game loop (demo/src/main.cpp) unwinds normally; post()
// below then completes the handshake with the real SYS_ResetSystem call.
#ifdef TARGET_WII
static void wii_power_callback() { quit = 1; }
static void wii_remote_power_callback(s32 chan) { quit = 1; }
#endif
static void wii_reset_callback(u32 irq, void *ctx) { quit = 1; }

// ---------------------------------------------------------------------------
// Shared GX / video state. All fixed for the window's lifetime.
// ---------------------------------------------------------------------------
// Double-buffered external framebuffers: VI scans out xfb[fbi^1] while GX copies
// the next frame into xfb[fbi].
static void       *xfb[2]  = { nullptr, nullptr };
static u32         fbi     = 0;           // index GX copies into this frame
static GXRModeObj *rmode   = nullptr;     // preferred TV mode (NTSC/PAL/MPAL)
static void       *gp_fifo = nullptr;     // GX command FIFO

// GX command FIFO size. libogc only defines the floor (GX_FIFO_MINSIZE, 64KB);
// 256KB is the conventional default used by the libogc GX examples.
static constexpr u32 FIFO_SIZE = 256 * 1024;
static int         efbH, fbW;             // EFB dimensions (scissor / overlay)

// World columns visible this run (tiles) -- declared in video.hpp, computed
// once in init() below from the real TV pixel width. NOT static: video.hpp's
// video::viewport_tx() reads it from every other translation unit.
u16 ogc_world_tx;

// Projections, built once at init. NES-space (256x240) for the GX-native
// tiles/sprites; screen-space for the profiler overlay.
static Mtx44       proj_nes;
#if defined(OGC_PROFILER)
static Mtx44       proj_screen;   // screen-pixel space, profiler overlay only
#endif

// ===========================================================================
// GX-NATIVE renderer state + helpers.
// ===========================================================================
// CHR atlas: 512 tiles, 16 per row -> 128x256 texels, GX_TF_CI4 (4bpp), 8x8
// blocks of 32 bytes each. One NES tile == one CI4 block.
static constexpr int ATLAS_TILES_X = 16;
static constexpr int ATLAS_W       = ATLAS_TILES_X * 8;          // 128
static constexpr int ATLAS_H       = (512 / ATLAS_TILES_X) * 8;  // 256
static u8       *chr_atlas = nullptr;
static GXTexObj  chrTexObj;
// Generation of ::ppu::chrGeneration the atlas above was last built from.
// Sentinel (not 0, ::chrGeneration's own initial value) forces the first build.
static u32       chr_built_gen = ~0u;

// Per-palette TLUTs, rebuilt each frame from paletteRAM. CI4 needs 16 entries.
// 32-byte aligned: GX_LoadTlut DMAs the source by physical address and requires
// 32-byte alignment, else it reads from the aligned-down address (garbage TLUT).
alignas(32) static u16 tlut_bg[4][16];
alignas(32) static u16 tlut_spr[4][16];
static GXTlutObj tlutBgObj[4];
static GXTlutObj tlutSprObj[4];

// The fixed NES master palette (ARGB8888, 0xAARRGGBB), shared verbatim with the
// software PPU in src/emu/ppu.cpp; here it only seeds the per-frame TLUTs.
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

// Background cells gathered per band, bucketed by palette so each palette is a
// single TLUT load + GX_Begin batch. A single band can cover the WHOLE screen
// (no mid-frame IRQ split, e.g. the title screen) and, worse, can land every
// one of its cells in the SAME palette bucket -- title's
// ppu::Flush(chrEmpty_tile, 0xff) leaves most of the nametable at attribute
// 0xFF (palette 3), so that one bucket alone must hold a full band's worth of
// cells, not band_cells/4. Each bucket therefore needs capacity for
// (viewport width in tiles + 1) x (viewport height in tiles + 1) cells -- the
// +1s cover the partial edge tile a non-zero fine scroll adds on each axis.
// viewport_tx() is runtime here (ogc_world_tx, video.hpp), so this can't be
// sized from it directly; its own formula -- ((fbWidth/ogc_scale)>>3)&~3,
// fbWidth<=640, ogc_scale>=1 -- caps out at 80 tiles wide (a 240p/double-
// strike TV mode forces ogc_scale down to 1), height is always the fixed 30.
// A too-small bound overflows bg_bucket[3][MAX_CELLS] straight into adjacent
// static data (TLUTs, chr_atlas state, ...), which reads as a mostly-blank/
// corrupted screen and can crash much later when that state is next touched
// -- this is exactly what happened on the 3DS backend's equivalent buffer,
// whose narrower fixed 33x32 bound this one used to share, before its
// viewport (also wider than 32 tiles) overflowed it on the very same title
// screen. Sized for the 80x30 extreme rather than the common ~40x30 case.
struct Cell { u16 atlas; i16 sx; i16 sy; };
static constexpr int MAX_CELLS = 81 * 31;
static Cell  bg_bucket[4][MAX_CELLS];
static int   bg_count[4];

// 0xAARRGGBB -> RGB5A3 opaque (bit15 set, 5/5/5).
static inline u16 rgb5a3_opaque(const u32 argb) {
    const u32 r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
    return static_cast<u16>(0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
}

// Convert the CHR ROM into the CI4 atlas, resolving each of the 512 PPU tile
// slots through the mapper's currently-bound translator (see ::ppu::ResolveTile
// and ::ppu::chrGeneration). NROM's translator is the identity function, so
// for a non-bank-switching game this is byte-for-byte the old init-only bake;
// a bank-switching mapper (MMC3) instead needs this re-run whenever it
// switches a CHR bank -- see render_gx's dirty check.
static void build_chr_atlas() {
    constexpr int blocks_per_row = ATLAS_W / 8;   // 16
    for (int ti = 0; ti < 512; ti++) {
        const u8 *tile = CHR_ROM + ppu::ResolveTile(static_cast<u16>(ti * 16));
        u8       *blk  = chr_atlas + (static_cast<size_t>((ti >> 4)) * blocks_per_row
                                       + (ti & 15)) * 32;
        for (int row = 0; row < 8; row++) {
            const u8 p0 = tile[row];
            const u8 p1 = tile[row + 8];
            for (int col = 0; col < 8; col += 2) {
                const int b0 = 7 - col, b1 = 6 - col;
                const u8 c0 = ((p0 >> b0) & 1) | (((p1 >> b0) & 1) << 1);
                const u8 c1 = ((p0 >> b1) & 1) | (((p1 >> b1) & 1) << 1);
                blk[row * 4 + col / 2] = static_cast<u8>((c0 << 4) | c1);  // hi nibble = left
            }
        }
    }
    DCFlushRange(chr_atlas, ATLAS_W * ATLAS_H / 2);
}

// Rebuild the per-palette TLUTs from paletteRAM and set the backdrop clear.
// Index 0 is transparent (alpha-test discarded) for both BG and sprites: BG
// colour-0 then shows the backdrop clear, sprite colour-0 is a hole.
static void build_tluts() {
    for (int p = 0; p < 4; p++) {
        tlut_bg[p][0]  = 0x0000;
        tlut_spr[p][0] = 0x0000;
        for (int c = 1; c < 4; c++) {
            tlut_bg[p][c]  = rgb5a3_opaque(nes_rgb[paletteRAM[p * 4 + c]        & 0x3F]);
            tlut_spr[p][c] = rgb5a3_opaque(nes_rgb[paletteRAM[0x10 + p * 4 + c] & 0x3F]);
        }
        DCFlushRange(tlut_bg[p],  sizeof tlut_bg[p]);
        DCFlushRange(tlut_spr[p], sizeof tlut_spr[p]);
        GX_InitTlutObj(&tlutBgObj[p],  tlut_bg[p],  GX_TL_RGB5A3, 16);
        GX_InitTlutObj(&tlutSprObj[p], tlut_spr[p], GX_TL_RGB5A3, 16);
    }

    // Backdrop = universal background colour (paletteRAM[0]). GX_CopyDisp clears
    // the EFB to this for the *next* frame; the backdrop is effectively static
    // in this game, so the one-frame latency is invisible.
    const u32 bd = nes_rgb[paletteRAM[0] & 0x3F];
    const GXColor c = { static_cast<u8>(bd >> 16), static_cast<u8>(bd >> 8),
                        static_cast<u8>(bd), 0xFF };
    GX_SetCopyClear(c, GX_MAX_Z24);
}

// Emit one 8x8 textured quad (NES pixel coords). flipH/flipV swap the UVs.
static inline void quad(const int ax, const int ay, const int sx, const int sy,
                        const bool flipH, const bool flipV) {
    const f32 u0 = static_cast<f32>(ax)     / ATLAS_W;
    const f32 u1 = static_cast<f32>(ax + 8) / ATLAS_W;
    const f32 v0 = static_cast<f32>(ay)     / ATLAS_H;
    const f32 v1 = static_cast<f32>(ay + 8) / ATLAS_H;
    const f32 ul = flipH ? u1 : u0, ur = flipH ? u0 : u1;
    const f32 vt = flipV ? v1 : v0, vb = flipV ? v0 : v1;
    const f32 x0 = static_cast<f32>(sx), x1 = static_cast<f32>(sx + 8);
    const f32 y0 = static_cast<f32>(sy), y1 = static_cast<f32>(sy + 8);
    GX_Position2f32(x0, y0); GX_TexCoord2f32(ul, vt);
    GX_Position2f32(x1, y0); GX_TexCoord2f32(ur, vt);
    GX_Position2f32(x1, y1); GX_TexCoord2f32(ur, vb);
    GX_Position2f32(x0, y1); GX_TexCoord2f32(ul, vb);
}

// ::emu::band_emit_fn: draw the nametable for screen rows [y0,y1) at the given
// scroll. Background colour-0 is transparent so behind-sprites / backdrop show.
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
        const int world_y = static_cast<int>(ys) + (syt - y0);
        const int wym       = ((world_y % world_h) + world_h) % world_h;
        const int trow      = wym / 8;
        const int local_row = trow % geo.tileH;
        const int nt_row    = trow / geo.tileH;
        const int at_roff   = (local_row >> 2) * geo.atW;
        const int at_rbits  = ((local_row >> 1) & 1) * 4;
        const int row32     = local_row * geo.tileW;

        for (int sxt = sx0; sxt < vpw; sxt += 8) {
            const int world_x = static_cast<int>(xs) + sxt;
            const int wxm       = ((world_x % world_w) + world_w) % world_w;
            const int tcol      = wxm / 8;
            const int local_col = tcol % geo.tileW;
            const int nt_col    = tcol / geo.tileW;
            const int nt_off    = (nt_col + nt_row * geo.gridW) * geo.pageBytes;

            const u8 tile_id = ppu::ReadNametable(static_cast<u16>(nt_off + row32 + local_col));
            const u8 attr    = ppu::ReadNametable(static_cast<u16>(nt_off + geo.ntBytes + at_roff + (local_col >> 2)));
            const int pal    = (attr >> (((local_col >> 1) & 1) * 2 + at_rbits)) & 3;

            const int ti = atlas0 + tile_id;
            Cell &cell = bg_bucket[pal][bg_count[pal]++];
            cell.atlas = static_cast<u16>(ti);
            cell.sx    = static_cast<i16>(sxt);
            cell.sy    = static_cast<i16>(syt);
        }
    }

    // Clip output to the band (EFB coords) so partial top/bottom tiles cut clean.
    const int e0 = y0 * efbH / 240;
    const int e1 = y1 * efbH / 240;
    GX_SetScissor(0, e0, fbW, e1 - e0);

    for (int p = 0; p < 4; p++) {
        if (!bg_count[p]) continue;
        GX_LoadTlut(&tlutBgObj[p], GX_TLUT0);
        GX_InitTexObjTlut(&chrTexObj, GX_TLUT0);
        GX_LoadTexObj(&chrTexObj, GX_TEXMAP0);
        GX_Begin(GX_QUADS, GX_VTXFMT0, bg_count[p] * 4);
        for (int i = 0; i < bg_count[p]; i++) {
            const Cell &c = bg_bucket[p][i];
            quad((c.atlas & 15) * 8, (c.atlas >> 4) * 8, c.sx, c.sy, false, false);
        }
        GX_End();
    }
}

// Draw the visible sprites of the requested priority class. NES sprite colour-0
// is transparent (alpha-test discarded). y is +1 vs OAM (matches the PPU).
static void draw_sprites(const bool behind) {
    const oam::sprite_t *oam = emu::OamShadow();
    const int  vpw    = video::viewport_px();
    const bool tall   = ppu::PPUCTRL & ppu::ctrl::SPRITE_SIZE;
    const int  height = tall ? 16 : 8;
    const int  atlas0 = (ppu::PPUCTRL & ppu::ctrl::SPRITE_ADDR) ? 256 : 0;
    const int  quads_per_sprite = tall ? 2 : 1;

    GX_SetScissor(0, 0, fbW, efbH);

    for (int p = 0; p < 4; p++) {
        int n = 0;
        for (int s = 0; s < OAM_SPRITES; s++) {
            const oam::sprite_t &o = oam[s];
            if ((o.attributes & 0x03) != p)            continue;
            if (((o.attributes & 0x20) != 0) != behind) continue;
            const int sx = static_cast<int>(o.x);
            const int sy = static_cast<int>(o.y) + 1;
            if (sx <= -8 || sx >= vpw || sy <= -height || sy >= 240) continue;
            n++;
        }
        if (!n) continue;

        GX_LoadTlut(&tlutSprObj[p], GX_TLUT0);
        GX_InitTexObjTlut(&chrTexObj, GX_TLUT0);
        GX_LoadTexObj(&chrTexObj, GX_TEXMAP0);
        GX_Begin(GX_QUADS, GX_VTXFMT0, n * 4 * quads_per_sprite);
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
                const int ti = atlas0 + o.tile;
                quad((ti & 15) * 8, (ti >> 4) * 8, sx, sy, flipH, flipV);
                continue;
            }
            // 8x16: tile bit 0 selects the pattern table (overriding SPRITE_ADDR),
            // tile & 0xFE is the top-half tile of the pair. A vertical flip swaps
            // which half draws on top as well as flipping each half's rows.
            const int pair = (o.tile & 1) * 256 + (o.tile & 0xFE);
            const int top_ti = pair + (flipV ? 1 : 0);
            const int bot_ti = pair + (flipV ? 0 : 1);
            quad((top_ti & 15) * 8, (top_ti >> 4) * 8, sx, sy,     flipH, flipV);
            quad((bot_ti & 15) * 8, (bot_ti >> 4) * 8, sx, sy + 8, flipH, flipV);
        }
        GX_End();
    }
}

static void band_noop(int, int, u16, u16) {}

// ===========================================================================
// GX pipeline mode setters. Each frame asserts the mode it needs, so the two
// renderers + overlay can interleave on one binary without state bleed.
// ===========================================================================

// REPLACE with a CI4 texture (GX-native tiles/sprites). Alpha test discards
// colour index 0 to reproduce NES transparency.
static inline void gx_mode_tiles() {
    GX_SetNumTexGens(1);
    GX_SetNumChans(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
    GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    // Alpha-test discards CI index 0 (TLUT entry 0x0000, alpha 0) to reproduce
    // NES transparency: background colour-0 then shows the backdrop clear, sprite
    // colour-0 is a hole. (This requires the TLUTs to be 32-byte aligned so
    // GX_LoadTlut reads the real alpha bits -- see tlut_bg/tlut_spr.)
    GX_SetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_GREATER, 0);
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS,  GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
}

#if defined(OGC_PROFILER)
// Pass the per-vertex colour straight through (the profiler overlay quads).
static inline void gx_mode_flat() {
    GX_SetNumTexGens(0);
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX,
                   0, GX_DF_NONE, GX_AF_NONE);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS,  GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
}

// ---------------------------------------------------------------------------
// Profiler overlay. The sampler (profiler.cpp) gives us the hottest main-thread
// PCs; here we draw each as an 8-digit hex address plus a bar (length relative
// to the hottest entry), in screen-pixel space using flat per-vertex-colour
// quads. Read the hex off-screen and resolve it with tools/profsym.sh.
// ---------------------------------------------------------------------------
static constexpr int   PROF_ROWS = 12;
static constexpr float PX        = 2.0f;    // overlay "pixel" size, ortho units

// 3x5 hex glyphs: 5 rows top->bottom; low 3 bits are columns left->right.
static const u8 kFont[16][5] = {
    {0b111,0b101,0b101,0b101,0b111}, // 0
    {0b010,0b110,0b010,0b010,0b111}, // 1
    {0b111,0b001,0b111,0b100,0b111}, // 2
    {0b111,0b001,0b111,0b001,0b111}, // 3
    {0b101,0b101,0b111,0b001,0b001}, // 4
    {0b111,0b100,0b111,0b001,0b111}, // 5
    {0b111,0b100,0b111,0b101,0b111}, // 6
    {0b111,0b001,0b010,0b010,0b010}, // 7
    {0b111,0b101,0b111,0b101,0b111}, // 8
    {0b111,0b101,0b111,0b001,0b111}, // 9
    {0b111,0b101,0b111,0b101,0b101}, // A
    {0b110,0b101,0b110,0b101,0b110}, // B
    {0b111,0b100,0b100,0b100,0b111}, // C
    {0b110,0b101,0b101,0b101,0b110}, // D
    {0b111,0b100,0b111,0b100,0b111}, // E
    {0b111,0b100,0b111,0b100,0b100}, // F
};

static inline void flat_rect(float x0, float y0, float x1, float y1,
                             u8 r, u8 g, u8 b) {
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
        GX_Position2f32(x0, y0); GX_Color4u8(r, g, b, 0xFF);
        GX_Position2f32(x1, y0); GX_Color4u8(r, g, b, 0xFF);
        GX_Position2f32(x1, y1); GX_Color4u8(r, g, b, 0xFF);
        GX_Position2f32(x0, y1); GX_Color4u8(r, g, b, 0xFF);
    GX_End();
}

// Draw one hex glyph at (gx,gy); returns the x advance (3px glyph + 1px gap).
static float draw_glyph(int nibble, float gx, float gy, u8 r, u8 g, u8 b) {
    const u8* rows = kFont[nibble & 0xF];
    for (int ry = 0; ry < 5; ry++)
        for (int cx = 0; cx < 3; cx++)
            if (rows[ry] & (1 << (2 - cx))) {
                const float x0 = gx + cx * PX, y0 = gy + ry * PX;
                flat_rect(x0, y0, x0 + PX, y0 + PX, r, g, b);
            }
    return 4 * PX;
}

// Draw the hottest-PC table. Call only after gx_mode_flat() + screen-space proj.
static void draw_overlay() {
    ProfEntry top[PROF_ROWS];
    ProfStats st;
    const int n = prof_top(top, PROF_ROWS, &st);

    const float ox   = 8.0f, oy = 8.0f;
    const float rowh = 7 * PX;               // 5px glyph + 2px gap
    const float hexw = 8 * (4 * PX);         // 8 hex chars
    const float barx = ox + hexw + 4 * PX;
    const float barw = 160.0f;
    const float h    = rowh * (n > 0 ? n : 1);

    flat_rect(ox - 4, oy - 4, barx + barw + 4, oy + h + 4, 16, 16, 16);

    const u32 hot = (n > 0) ? top[0].count : 1;
    for (int i = 0; i < n; i++) {
        const float ry = oy + i * rowh;
        float gx = ox;
        for (int d = 0; d < 8; d++)
            gx += draw_glyph((top[i].addr >> (28 - 4 * d)) & 0xF, gx, ry, 0, 255, 0);
        const float bw = (float)top[i].count * barw / (float)hot;
        flat_rect(barx, ry, barx + bw, ry + 5 * PX, 255, 128, 0);
    }
}
#endif // OGC_PROFILER

// ===========================================================================
// Per-frame renderer.
// ===========================================================================

// GX-NATIVE path: no CPU raster. GenerateBands walks the IRQ timeline (running
// game logic) and calls draw_bg_band per scroll-constant band; sprites bracket
// the background for NES priority.
static void render_gx() {
    GX_LoadProjectionMtx(proj_nes, GX_ORTHOGRAPHIC);

    const bool bg  = ppu::PPUMASK & ppu::mask::BG;
    const bool spr = ppu::PPUMASK & ppu::mask::SPRITE;

    gx_mode_tiles();

    if (bg || spr) {
        // A mapper (MMC3) switched a CHR bank since the atlas was last built --
        // re-expand all 512 tiles through the translator. Gated on the
        // generation counter so an unbanked (NROM) game never pays this cost
        // past the one build already done in init(). GX_InvalidateTexAll below
        // already runs unconditionally every frame (for the TLUT rebuild), so
        // it covers invalidating the GPU's cached copy of chr_atlas too.
        if (chr_built_gen != ppu::chrGeneration) {
            build_chr_atlas();
            chr_built_gen = ppu::chrGeneration;
        }

        build_tluts();
        GX_InvalidateTexAll();

        // NES priority order: behind-sprites under the background, the
        // background (colour-0 transparent) over them, front-sprites on top.
        if (spr) draw_sprites(true);
        emu::GenerateBands(bg ? draw_bg_band : band_noop);  // also runs game logic
        if (spr) draw_sprites(false);
    } else {
        // Rendering off: still walk the timeline so the IRQ handlers (game
        // logic) fire; the EFB stays at the backdrop clear.
        emu::GenerateBands(band_noop);
    }
}

namespace video {

void WaitForPresent() {
#if defined(OGC_PROFILER)
    // Controller polling: L (unused by the game) edge-triggers a profiler reset.
    // Edge detection via prevHeld so a held button fires once.
    static u32 prevHeld = 0;
    PAD_ScanPads();
    const u32 held = PAD_ButtonsHeld(0);
    if ((held & PAD_TRIGGER_L) && !(prevHeld & PAD_TRIGGER_L)) prof_reset();
    prevHeld = held;
#endif

    render_gx();

#if defined(OGC_PROFILER)
    // Profiler overlay on top, in screen-space.
    GX_LoadProjectionMtx(proj_screen, GX_ORTHOGRAPHIC);
    GX_SetScissor(0, 0, fbW, efbH);
    gx_mode_flat();
    draw_overlay();
#endif

    // Copy this frame into the back buffer, show it, then flip for next frame.
    GX_DrawDone();
    GX_CopyDisp(xfb[fbi], GX_TRUE);
    GX_Flush();

    VIDEO_SetNextFramebuffer(xfb[fbi]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    fbi ^= 1;

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
    VIDEO_Init();
    rmode = VIDEO_GetPreferredMode(nullptr);

    // The NES core ticks exactly one game frame per VBlank (see WaitForPresent:
    // VIDEO_WaitVSync() then nmi()), so the game's logic rate IS the display's
    // refresh rate. On a 50Hz PAL preferred mode the ~60Hz NES game therefore
    // runs ~17% slow -- which reads as input/scroll "lag" even though the
    // console is nowhere near CPU-bound. Force a 60Hz mode so the tick rate
    // matches the game's intended ~60Hz. Dolphin renders any mode regardless of
    // region; a real PAL TV would instead need an EURGB60/component-cable guard.
    const u32 vfmt = rmode->viTVMode >> 2;   // (fmt << 2) | mode
    if (vfmt == VI_PAL || vfmt == VI_DEBUG_PAL) {
        rmode = &TVNtsc480IntDf;             // 60Hz, 480i de-flicker
    }

    xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb[fbi]);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    // --- GX init -----------------------------------------------------------
    gp_fifo = memalign(32, FIFO_SIZE);
    memset(gp_fifo, 0, FIFO_SIZE);
    GX_Init(gp_fifo, FIFO_SIZE);

    GXColor black = {0, 0, 0, 0xff};
    GX_SetCopyClear(black, GX_MAX_Z24);

    GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
    GX_SetDispCopyYScale(static_cast<f32>(rmode->xfbHeight) / static_cast<f32>(rmode->efbHeight));
    GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopyDst(rmode->fbWidth, rmode->xfbHeight);
    GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
    GX_SetFieldMode(rmode->field_rendering,
                    ((rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));

    if (rmode->aa) GX_SetPixelFmt(GX_PF_RGB565_Z16, GX_ZC_LINEAR);
    else           GX_SetPixelFmt(GX_PF_RGB8_Z24,   GX_ZC_LINEAR);

    GX_SetCullMode(GX_CULL_NONE);
    GX_CopyDisp(xfb[fbi], GX_TRUE);
    GX_SetDispCopyGamma(GX_GM_1_0);

    // 2D: no depth test. Tiles/sprites REPLACE the raster colour with the CI4
    // texture and alpha-test colour-0 for NES transparency (asserted per-frame
    // by gx_mode_tiles). No blend.
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetZCompLoc(GX_TRUE);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS,  GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,  GX_POS_XY,   GX_F32,   0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST,   GX_F32,   0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);  // overlay

    GX_SetNumChans(1);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
    GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);

    // World columns needed to fill the real TV width when the fixed 30 rows
    // map 1:1 to the real TV height -- i.e. render more world instead of
    // stretching a 256px image over a wider screen (see video::viewport_tx()
    // in video.hpp). Aligned to a multiple of 4 tiles (32px attribute grid),
    // matching the SDL LANDSCAPE desktop formula. Derived from the same
    // rmode->fbWidth/efbHeight pair GX_SetViewport above already uses, so the
    // projection below stays self-consistent regardless of the actual TV mode.
    const u32 ogc_scale = rmode->efbHeight >= 240 ? rmode->efbHeight / 240 : 1;
    ogc_world_tx = static_cast<u16>(((rmode->fbWidth / ogc_scale) >> 3) & ~3u);

    // Both orthographic projections, built once; identity modelview.
    // NES-space (256 x variable-width x240): GX tiles/sprites. Screen-space:
    // profiler overlay.
    guOrtho(proj_nes,    0, video::viewport_py(), 0, video::viewport_px(), 0, 300);
#if defined(OGC_PROFILER)
    guOrtho(proj_screen, 0, rmode->efbHeight - 1, 0, rmode->fbWidth - 1, 0, 300);
#endif
    GX_LoadProjectionMtx(proj_nes, GX_ORTHOGRAPHIC);

    Mtx mv;
    guMtxIdentity(mv);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);

    efbH = rmode->efbHeight;
    fbW  = rmode->fbWidth;

    // video::vram_bytes() (video.hpp) sizes this for ::ppu::nametableCount
    // physical, viewport-sized nametables -- the NES-hardware-equivalent 2
    // pages/0x800 bytes when ogc_world_tx resolves to 32 or less (e.g. a
    // tiny/unusual TV mode), scaling up with a wider TV's viewport (see
    // ::emu::ComputeNtGeometry in draw_bg_band below).
    emu::InitMemory(video::vram_bytes());

    // --- GX-NATIVE resources ----------------------------------------------
    chr_atlas = static_cast<u8 *>(memalign(32, ATLAS_W * ATLAS_H / 2));
    build_chr_atlas();
    chr_built_gen = ppu::chrGeneration;
    GX_InitTexObjCI(&chrTexObj, chr_atlas, ATLAS_W, ATLAS_H, GX_TF_CI4,
                    GX_CLAMP, GX_CLAMP, GX_FALSE, GX_TLUT0);
    GX_InitTexObjFilterMode(&chrTexObj, GX_NEAR, GX_NEAR);

    ogc_input_init();

    // See the wii_*_callback comment above: without these, a Wii title never
    // answers IOS/STM's power-down or reset request, which is what leaves
    // Dolphin stuck on "Shutting Down" for the Wii build. SYS_SetResetCallback
    // exists on GameCube too (it's the physical Reset button), so it's
    // registered unconditionally; it's a no-op for this bug since GC has no
    // such handshake to hang on.
    SYS_SetResetCallback(wii_reset_callback);
#ifdef TARGET_WII
    SYS_SetPowerCallback(wii_power_callback);
    WPAD_SetPowerButtonCallback(wii_remote_power_callback);
#endif

#if defined(OGC_PROFILER)
    // Start the statistical PC sampler. MUST run here -- init() executes on the
    // main thread, whose KThread* the sampler reads PCs from.
    prof_init();
#endif
}

void irq::post() {
    // libogc has no teardown counterpart to VIDEO_Init/GX_Init that a homebrew
    // app needs at exit; the loader resets the machine. Free the scratch buffers
    // so a clean shutdown doesn't leak (mostly documentary on a console).
    free(chr_atlas);
    free(gp_fifo);

#ifdef TARGET_WII
    // This is the other half of the wii_*_callback registration in init():
    // the callbacks only set `quit` so the game loop unwinds; the actual
    // SYS_ResetSystem call is the acknowledgement IOS/STM (and, in Dolphin,
    // the Stop button) is waiting on. Without it the request is never
    // answered and Dolphin hangs on "Shutting Down" instead of closing.
    SYS_ResetSystem(SYS_POWEROFF, 0, 0);
#endif
}
