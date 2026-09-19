/**
 * @file ppu.cpp
 * @brief Portable software NES PPU shared by every emulated-PPU backend.
 *
 * This is the backend-agnostic half of the desktop/console renderer: the
 * per-pixel PPU emulator (::emu::GenerateFrame) plus all the nametable,
 * attribute, palette and OAM write functions, the scroll model and the
 * sprite-zero machinery. It depends only on the shared PPU state and the
 * interrupt queue -- never on SDL, GX or any display API. Each backend
 * (src/SDL3, src/ogc) links this and supplies presentation/input/audio.
 */
#include "emu.hpp"

#include <platform-nes/video.hpp>
#include <platform-nes/interrupts.hpp>

#include <cstdlib>
#include <cstring>

#include "platform-nes/technology.hpp"

// ---------------------------------------------------------------------------
// Shared PPU state. These were previously defined in src/SDL3/video.cpp; they
// are platform-independent (plain RAM shadows of the PPU), so they live here
// and are referenced by every backend.
// ---------------------------------------------------------------------------
u16 xScroll;
u16 yScroll;
u8 *paletteRAM;
u8 *VideoRAM;
int quit;
bool mirroring;

namespace ppu {
u8 PPUCTRL;
u8 PPUMASK;
u32 chrGeneration;
}   // namespace ppu

const u8 *patternTable = CHR_ROM;

static video::spriteZeroHandler_t sprite0_zero;

void video::SetSpriteZeroHandler(const vec2<u16> pos, void (*fn)()) {
    sprite0_zero = (video::spriteZeroHandler_t){ .method = fn, .pos = pos };
}

/* Weak default ::ppu::ResolveTile: identity, matching a board with no CHR
 * bank switching at all -- a bank-switching mapper's own emu-side TU (e.g.
 * src/emu/mappers/mmc3.cpp) supplies a strong definition of this exact
 * symbol instead. See this function's own doc comment (video.hpp) for why
 * that always wins over this one with no runtime dispatch. */
__attribute__((weak)) u32 ppu::ResolveTile(const u16 tileVMA) {
    return tileVMA;
}

/* Weak default ::ppu::nametableCount: every ordinary board (switchable H/V
 * mirroring, e.g. MMC3) provides exactly 2 physical nametables -- see this
 * symbol's own doc comment (video.hpp). Same idiom as audio.hpp's
 * ::sfxs/::nSfx (src/SDL3/audio.cpp). */
__attribute__((weak)) extern const u8 ppu::nametableCount = 2;

/* Weak default ::ppu::ReadNametable/::WriteNametable: every nametable/
 * attribute VRAM access funnels through these two -- correct for every board,
 * since ::video::vram_bytes() already sizes ::VideoRAM for every page
 * ::ppu::nametableCount claims (see that symbol's own doc comment, video.hpp).
 * A board could still supply a strong override if it ever needed to answer
 * from somewhere else. */
__attribute__((weak)) u8 ppu::ReadNametable(const u16 logical) {
    return VideoRAM[logical];
}

__attribute__((weak)) void ppu::WriteNametable(const u16 logical, const u8 value) {
    VideoRAM[logical] = value;
}

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

#ifdef _WIN32
// Anchor symbol for the merged CHR ROM section on the MSVC/COFF target (the
// other targets get __start_chr_rom from the linker). See video.hpp's CHR_ROM.
//
// Inline asm labels bypass the compiler's extern "C" name decoration, so the
// raw symbol emitted here must already match whatever decorated name video.hpp's
// `extern "C" const u8 _chr_rom[]` compiles down to for the active ABI: on
// x86_64 that's the undecorated `_chr_rom`, but on i686 cdecl prepends an
// extra leading underscore to every extern "C" symbol, so the reference is
// actually to `__chr_rom`.
#if defined(_M_IX86) || defined(__i386__)
  #define PNES_CHR_ROM_ANCHOR "__chr_rom"
#else
  #define PNES_CHR_ROM_ANCHOR "_chr_rom"
#endif
__asm__(
    ".pushsection chr_rom$a,\"dr\"\n"
    ".global " PNES_CHR_ROM_ANCHOR "\n"
    PNES_CHR_ROM_ANCHOR ":\n"
    ".popsection\n"
);
#endif

/* PPU-side OAM: a per-frame snapshot of the application's OAM buffer,
 * mirroring the NES OAMDMA. GenerateFrame renders from this, never from the
 * live buffer, so mid-frame writes (e.g. from the sprite-zero IRQ handler)
 * only appear on the next frame -- exactly as on hardware. */
static struct oam::sprite_t oamShadow[OAM_SPRITES];

#pragma region PPU_EMU

/* Per-pixel NES PPU emulator.
 *
 * Scroll model (matches real PPU v/t-register behaviour -- deliberately
 * ASYMMETRIC between X and Y, not a simplification):
 *
 *   On real hardware, the PPU's internal v register only re-latches its
 *   HORIZONTAL bits from t once per scanline (at dot 257); the VERTICAL bits
 *   are only re-latched from t once per FRAME, during the pre-render
 *   scanline (dots 280-304). A mid-frame $2005/$2006 write updates t
 *   immediately, but a change to the Y portion never reaches v -- and so
 *   never affects what's on screen -- until the following frame's
 *   pre-render line. This is exactly why real NES status-bar Y-splits use
 *   the $2006-double-write trick instead of a plain $2005 Y write: an
 *   ordinary mid-frame Y write is supposed to be a no-op for the rest of
 *   that frame. X has no such restriction, which is what makes mid-frame
 *   horizontal splits (this engine's own MMC3 HUD/menu splits) work at all.
 *
 *   xScroll is read fresh every segment (see wx0 below) -- any write takes
 *   effect on the very next pixel, matching the real per-scanline reload.
 *   yScroll instead only seeds ppu_y once, here at the top of the frame;
 *   ppu_y then free-runs (auto-incrementing once per scanline) for the rest
 *   of the frame regardless of any further yScroll writes an IRQ handler
 *   makes -- those are only picked up the NEXT time GenerateFrame runs,
 *   same as real hardware only picking up a new Y at the next pre-render
 *   line. A handler that wants an on-screen Y split within one frame still
 *   needs the same trick a real NES program would.
 *
 * IRQ dispatch: each scanline is split at the px of the next queued IRQ.
 * The handler fires before the pixel at its (px, py) renders, so it can
 * mutate xScroll, ppu::PPUCTRL, palette -- anything -- and the very next
 * pixel sees the new X/palette/etc state; a yScroll write there is stored
 * but, per above, not visible until next frame.
 *
 * The finished frame is written into the caller's ARGB8888 surface (@p fb,
 * @p stride pixels per row); the backend presents it. */
namespace emu {

void GenerateFrame(u32 *fb, const int stride) {
    const int vpw = video::viewport_px();
    const int vph = video::viewport_py();

    /* Nametable/attribute geometry for the linked board: ::ppu::nametableCount
     * physical, viewport-sized nametables, gridded by mirroring -- see
     * ::emu::ComputeNtGeometry's own doc comment (emu.hpp). Folding ppu_y/
     * xScroll through geo.worldH/geo.worldW instead of a hardcoded 240/256 is
     * what lets the background walk reach every nametable the linked board
     * actually provides, exactly the way real mirroring wiring would alias
     * an out-of-range logical address back onto real storage, instead of
     * silently wrapping into (or worse, past) a smaller world than the board
     * has. */
    const emu::NtGeometry geo = emu::ComputeNtGeometry();
    const int world_w  = geo.worldW;
    const int world_h  = geo.worldH;
    const int spr_base = (ppu::PPUCTRL & ppu::ctrl::SPRITE_ADDR) ? 0x1000 : 0x0000;
    const int spr_h    = (ppu::PPUCTRL & ppu::ctrl::SPRITE_SIZE) ? 16 : 8;

    /* PPU Y counter: the absolute VRAM row currently being sourced. Seeded
     * from yScroll once, here -- NOT re-read from yScroll again for the rest
     * of this frame, even if an IRQ handler below writes it (see this
     * function's own doc comment for why that matches real hardware). */
    int ppu_y = (int)yScroll;

    for (int py = 0; py < vph; py++) {
        /* Sprites use screen-space Y -- they don't scroll with the background. */
        int line_spr[64];
        int n_line = 0;
        if (ppu::PPUMASK & ppu::mask::SPRITE) {
            for (size_t s = 0; s < OAM_SPRITES && n_line < 64; s++) {
                if (const int sy = static_cast<int>(oamShadow[s].y) + 1; py >= sy && py < sy + spr_h)
                    line_spr[n_line++] = static_cast<int>(s);
            }
        }

        int seg_start = 0;
        while (seg_start < vpw) {
            /* Check whether the pending IRQ falls on this scanline. Consuming
             * it (clearing irqPendingValid) rather than latching a local
             * "already fired this frame" flag lets the handler re-arm a new
             * position for the next hunk -- we keep seeking forward and can
             * fire again later in the same frame, as many times as needed. */
            int seg_end = vpw;
            int fire    = 0;
            if (irq::irqPendingValid) {
                const vec2<u16>& pos = irq::irqPosition;
                if (static_cast<int>(pos.y) < py
                    || (static_cast<int>(pos.y) == py && static_cast<int>(pos.x) < seg_start)) {
                    /* Already past — consume without calling the handler. */
                    irq::irqPendingValid = false;
                } else if (static_cast<int>(pos.y) == py) {
                    seg_end = static_cast<int>(pos.x);
                    fire    = 1;
                }
            }

            /* Derive Y source from ppu_y (the PPU's current absolute row).
             * This is computed once per segment: ppu_y only changes at IRQ
             * boundaries, so it is constant within a segment. xScroll is
             * added to px inside the loop for the horizontal scan offset. */
            const int wy        = ppu_y % world_h;
            const int tile_row  = wy / 8;
            const int local_row = tile_row % geo.tileH;
            const int nt_row    = tile_row / geo.tileH;
            const int fine_y    = wy & 7;

            /* Background tile-walk state, advanced incrementally across the
             * scanline so the per-pixel inner loop carries NO integer divides.
             * (The Gekko's divw is a ~19-cycle, non-pipelined stall; at
             * 256x240 the old four-divides-per-pixel BG fetch dominated the
             * frame.) Every Y-derived term and the pattern-table base is
             * segment-invariant and hoisted here; the X walk then carries
             * fine_x / local_col / nt_col and reloads the tile bytes only at
             * each 8-pixel boundary via load_tile(). */
            const bool bg_on    = ppu::PPUMASK & ppu::mask::BG;
            const bool bg_left  = ppu::PPUMASK & 0x02;
            const int  row32    = local_row * geo.tileW;
            const int  at_roff  = (local_row >> 2) * geo.atW;    // (local_row / 4) * atW
            const int  at_rbits = ((local_row >> 1) & 1) * 4;
            const int  ntrow_b  = nt_row * geo.gridW;
            const int  chr_tbl  = (ppu::PPUCTRL & ppu::ctrl::BG_ADDR) ? 0x1000 : 0;

            const int  wx0      = (static_cast<int>(xScroll) + seg_start) % world_w;
            const int  tcol0    = wx0 >> 3;
            int        fine_x   = wx0 & 7;
            int        local_col = tcol0 % geo.tileW;
            int        nt_col    = tcol0 / geo.tileW;

            u8 plane0 = 0, plane1 = 0, tile_pal = 0;
            auto load_tile = [&]() {
                const int nt_off = (nt_col + ntrow_b) * geo.pageBytes;
                const u8 tile_id = ppu::ReadNametable(static_cast<u16>(nt_off + row32 + local_col));
                const u8 attr    = ppu::ReadNametable(static_cast<u16>(nt_off + geo.ntBytes + at_roff + (local_col >> 2)));
                tile_pal = (attr >> (((local_col >> 1) & 1) * 2 + at_rbits)) & 3;
                const int chr_base = chr_tbl + tile_id * 16 + fine_y;
                const u32 chr_lma  = ppu::ResolveTile(static_cast<u16>(chr_base));
                plane0 = patternTable[chr_lma];
                plane1 = patternTable[chr_lma + 8];
            };
            if (bg_on) load_tile();

            for (int px = seg_start; px < seg_end; px++) {

                /* --- Background ---------------------------------------- */
                int bg_cidx = 0;
                u8  bg_pal  = 0;
                if (bg_on && (bg_left || px >= 8)) {
                    const int bit = 7 - fine_x;
                    bg_cidx = ((plane0 >> bit) & 1) | (((plane1 >> bit) & 1) << 1);
                    bg_pal  = tile_pal;
                }
                const int bg_opaque = bg_cidx != 0;

                /* --- Sprites ------------------------------------------- */
                int     spr_hit    = 0;
                int     spr_behind = 0;
                u8 spr_nes    = 0;
                if ((ppu::PPUMASK & ppu::mask::SPRITE) && ((ppu::PPUMASK & 0x04) || px >= 8)) {
                    for (int k = 0; k < n_line; k++) {
                        const auto [y, tile, attributes, x] = oamShadow[line_spr[k]];
                        const int sx  = (int)x;
                        if (px < sx || px >= sx + 8) continue;
                        const int sy      = static_cast<int>(y + 1);
                        const u8 att = attributes;
                        const int row     = (att & 0x80) ? (spr_h - 1 - (py - sy)) : (py - sy);
                        const int col_bit = (att & 0x40) ? (px - sx) : (7 - (px - sx));
                        /* 8x16 mode: tile bit 0 selects the pattern table (overriding
                         * SPRITE_ADDR) and row>>3 picks the top/bottom tile of the pair;
                         * row already accounts for vertical flip above, so this falls out
                         * of the same formula real hardware uses. */
                        const int addr    = (spr_h == 16)
                            ? ((tile & 1) ? 0x1000 : 0x0000) + (tile & 0xFE) * 16 + (row >> 3) * 16 + (row & 7)
                            : spr_base + tile * 16 + row;
                        /* Route through the mapper's tile translator, same as the BG
                         * fetch above -- CHR-bank-switching mappers (MMC3) need this to
                         * pick the right physical bank for sprites too. addr already
                         * sits in $0000-$1FFF PPU space, so in 8x16 mode it naturally
                         * lands in whichever half (R0/R1 vs R2-R5 windows) the tile's
                         * pattern-table bit selected -- no extra bank logic needed here. */
                        const u32 spr_lma = ppu::ResolveTile(static_cast<u16>(addr));
                        const int cidx    = ((patternTable[spr_lma]      >> col_bit) & 1)
                                          | (((patternTable[spr_lma + 8]  >> col_bit) & 1) << 1);
                        if (cidx == 0) continue;
                        spr_nes    = paletteRAM[0x10 + (att & 0x03) * 4 + cidx];
                        spr_behind = att & 0x20;
                        spr_hit    = 1;
                        break;
                    }
                }

                /* --- Compose ------------------------------------------- */
                u8 final_nes;
                if      (spr_hit && (!spr_behind || !bg_opaque)) final_nes = spr_nes;
                else if (bg_opaque)                               final_nes = paletteRAM[bg_pal * 4 + bg_cidx];
                else                                              final_nes = paletteRAM[0];

                if (ppu::PPUMASK & 0x01) final_nes &= 0x30;

                u32 col = nes_rgb[final_nes & 0x3F];

                if (ppu::PPUMASK & 0xE0) {
                    u32 r = (col >> 16) & 0xFF;
                    u32 g = (col >>  8) & 0xFF;
                    u32 b =  col        & 0xFF;
                    if (ppu::PPUMASK & 0x20) { g = g * 3 / 4; b = b * 3 / 4; }
                    if (ppu::PPUMASK & 0x40) { r = r * 3 / 4; b = b * 3 / 4; }
                    if (ppu::PPUMASK & 0x80) { r = r * 3 / 4; g = g * 3 / 4; }
                    col = 0xFF000000u | (r << 16) | (g << 8) | b;
                }

                fb[py * stride + px] = col;

                /* Advance the background X walk one pixel; reload the tile
                 * bytes at each 8-pixel boundary, wrapping local_col at the
                 * nametable edge and nt_col at the world edge. */
                if (++fine_x == 8) {
                    fine_x = 0;
                    if (++local_col == geo.tileW) {
                        local_col = 0;
                        if (++nt_col == geo.gridW) nt_col = 0;
                    }
                    if (bg_on) load_tile();
                }
            }

            /* Fire the IRQ. A yScroll write inside the handler is NOT
             * applied to ppu_y here -- real hardware doesn't reload v's
             * vertical bits until the next frame's pre-render line, so
             * neither does this. ppu_y keeps free-running (see the ppu_y++
             * below); xScroll, unlike yScroll, is simply re-read fresh by
             * the next segment's wx0, so an X change the handler made is
             * already picked up with no bookkeeping needed here. */
            if (fire) {
                /* Clear before calling so a re-arm from inside the handler
                 * (a new irqPosition for the next hunk) survives the call. */
                irq::irqPendingValid = false;
                if (irq::irqHandler) irq::irqHandler();
            }

            seg_start = seg_end;
        }

        /* Advance the PPU Y counter by one scanline, exactly as the real
         * PPU increments its V register at the end of each active line. */
        ppu_y++;
    }
}

void InitMemory(const unsigned vram_bytes) {
    paletteRAM = static_cast<u8 *>(malloc(32));
    VideoRAM   = static_cast<u8 *>(malloc(vram_bytes));
}

/* Raster-timeline walk for the GX backend. Same IRQ-dispatch logic as
 * GenerateFrame's outer loop -- including the same real-hardware asymmetry
 * between X and Y (see that function's own doc comment) -- but with no inner
 * pixel loop: it splits the frame into scanline bands at IRQ boundaries,
 * fires each handler (running game logic, which may move the scroll), and
 * hands each band's scroll to the backend to render as a tilemap. See
 * emu.hpp for the band semantics. */
void GenerateBands(const band_emit_fn emit) {
    const int vph = video::viewport_py();

    int  band_start = 0;
    u16  cur_xs = xScroll;
    // Latched once, here, for the whole frame -- a handler's yScroll write
    // below is not picked up until the NEXT GenerateBands call, matching
    // real hardware only reloading v's vertical bits at the pre-render line.
    const u16 fixed_ys = yScroll;

    for (int py = 0; py < vph; py++) {
        /* Fire the pending IRQ when its scanline is reached. We band at
         * scanline granularity (the px within a line is irrelevant to a tile
         * renderer), but still run the handler so game logic and the scroll
         * write happen at the right raster position. Consuming irqPendingValid
         * here (rather than latching a local "already fired" flag) lets the
         * handler re-arm a new position for the next hunk -- we keep seeking
         * forward and can fire again later in the same frame, as many times
         * as needed. */
        if (irq::irqPendingValid) {
            const vec2<u16>& pos = irq::irqPosition;
            if (static_cast<int>(pos.y) < py) {
                irq::irqPendingValid = false;   // stale — past without firing
            } else if (static_cast<int>(pos.y) == py) {
                if (py > band_start) {
                    emit(band_start, py, cur_xs, fixed_ys);
                    band_start = py;
                }
                /* Clear before calling so a re-arm from inside the handler
                 * survives the call. */
                irq::irqPendingValid = false;
                if (irq::irqHandler) irq::irqHandler();
                // X, unlike Y, reloads every scanline on real hardware --
                // simply re-read fresh, no "did it change" bookkeeping needed.
                cur_xs = xScroll;
            }
        }
    }

    if (band_start < vph) emit(band_start, vph, cur_xs, fixed_ys);
}

const oam::sprite_t* OamShadow() { return oamShadow; }

}   // namespace emu

#pragma endregion

// Every one of these four functions addresses the exact same nametable/
// attribute layout ::emu::GenerateFrame reads from -- ::emu::ComputeNtGeometry
// (emu.hpp) is the single shared source of that layout, so the write side
// (these) and the read side can never drift apart the way the old
// viewport-width-derived nt_cols/hardcoded-0x400 scheme used to (see
// ::ppu::nametableCount's own doc comment, video.hpp, for why a logical
// address past one nametable's own tiles is expected -- e.g. a vertical-
// mirroring reveal trick -- and must land on a real, readable page instead of
// silently addressing storage no board actually provides).
inline static u16 xy_to_nt_addr(u16 x, u16 y) {
    const emu::NtGeometry geo = emu::ComputeNtGeometry();
    // Both axes wrap modulo the grid's own size here -- exactly the alias a
    // real board's mirroring wiring gives for free: a caller placing content
    // at, say, y == one nametable's height (deliberately past what's on
    // screen, to be revealed by a later scroll -- see ::ppu::nametableCount's
    // own doc comment, video.hpp) lands back on a REAL page the grid actually
    // has, exactly the row/column it would alias to on real hardware, instead
    // of indexing a page number past ::ppu::nametableCount that no board
    // provides.
    const u16 nt_h = static_cast<u16>((x / geo.tileW) % geo.gridW);
    const u16 nt_v = static_cast<u16>((y / geo.tileH) % geo.gridH);
    const u16 col  = static_cast<u16>(x % geo.tileW);
    const u16 row  = static_cast<u16>(y % geo.tileH);

    return static_cast<u16>((nt_h + nt_v * geo.gridW) * geo.pageBytes + row * geo.tileW + col);
}

inline static u16 xy_to_at_addr(u16 x, u16 y) {
    const emu::NtGeometry geo = emu::ComputeNtGeometry();
    // See xy_to_nt_addr above: both axes wrap modulo the grid's own size.
    const u16 nt_h = static_cast<u16>((x / geo.tileW) % geo.gridW);
    const u16 nt_v = static_cast<u16>((y / geo.tileH) % geo.gridH);
    const u16 col  = static_cast<u16>(x % geo.tileW);
    const u16 row  = static_cast<u16>(y % geo.tileH);

    return static_cast<u16>((nt_h + nt_v * geo.gridW) * geo.pageBytes
         + geo.ntBytes + (row / 4) * geo.atW + (col / 4));
}

// Inverse of xy_to_nt_addr: recovers an (x,y) the multi-byte writers' own
// per-byte page-wrap logic can walk from, given only the flattened address a
// caller precomputed via CartesianToAddress. Native division is cheap here
// (unlike the 6502 NES backend, where CartesianToAddress's cost is the whole
// reason an address overload exists) -- so rather than re-deriving a
// division-free page-wrap scheme (fragile: it would have to assume the grid
// stays a fixed shape, which isn't guaranteed on a resizable LANDSCAPE
// window), this just reconstructs the coordinate the existing, already-
// wraparound-safe pos-based implementation needs, and defers to it unchanged.
inline static void nt_addr_to_xy(const u16 address, u16& x, u16& y) {
    const emu::NtGeometry geo = emu::ComputeNtGeometry();
    const u16 pageIndex = static_cast<u16>(address / geo.pageBytes);
    const u16 nt_h       = static_cast<u16>(pageIndex % geo.gridW);
    const u16 nt_v       = static_cast<u16>(pageIndex / geo.gridW);
    const u16 local      = static_cast<u16>(address % geo.pageBytes);
    x = static_cast<u16>(nt_h * geo.tileW + local % geo.tileW);
    y = static_cast<u16>(nt_v * geo.tileH + local / geo.tileW);
}

// Attribute counterpart of ::nt_addr_to_xy. An attribute address only encodes
// row/4 and col/4 (4 metatile rows/cols share one attribute byte), so this
// can't recover the exact (x,y) that produced it -- only picks the top-left
// corner of that 4x4 block, which is fine: xy_to_at_addr(x,y) depends only on
// x/4 and y/4, so any (x,y) inside the block round-trips to the same address.
inline static void at_addr_to_xy(const u16 address, u16& x, u16& y) {
    const emu::NtGeometry geo = emu::ComputeNtGeometry();
    const u16 pageIndex = static_cast<u16>(address / geo.pageBytes);
    const u16 nt_h       = static_cast<u16>(pageIndex % geo.gridW);
    const u16 nt_v       = static_cast<u16>(pageIndex / geo.gridW);
    const u16 local      = static_cast<u16>(address % geo.pageBytes - geo.ntBytes);
    x = static_cast<u16>(nt_h * geo.tileW + (local % geo.atW) * 4);
    y = static_cast<u16>(nt_v * geo.tileH + (local / geo.atW) * 4);
}

namespace ppu {

void EnableRendering(u8 ppuCtrl_, u8 ppuMask_) {
    ppu::PPUMASK = ppuMask_;
    ppu::PPUCTRL = ppuCtrl_;
}

/* Weak default ::ppu::Flush: walks every physical nametable
 * (::ppu::nametableCount of them, see that symbol's own doc comment) --
 * correct for every board, since they all now live in the one flat
 * ::VideoRAM allocation (::video::vram_bytes() already sizes it for all of
 * them). */
__attribute__((weak))
void Flush(const u8 nt, const u8 at) {
    const emu::NtGeometry geo = emu::ComputeNtGeometry();
    const int pageCount = static_cast<int>(ppu::nametableCount);
    for (int page = 0; page < pageCount; page++) {
        const int base = page * geo.pageBytes;
        for (int i = 0; i < geo.ntBytes; i++) {
            ppu::WriteNametable(static_cast<u16>(base + i), nt);
        }
        for (int i = 0; i < geo.atBytes; i++) {
            ppu::WriteNametable(static_cast<u16>(base + geo.ntBytes + i), at);
        }
    }
}

void WriteFromBufferToNameTable(
    const vec2<u16> pos, const u8* source, const u8 sBuffer, u8 polarity
) {
    const u16 x = pos.x, y = pos.y;
    ppu::PPUCTRL &= ~ppu::ctrl::POLARITY;
    if (polarity) ppu::PPUCTRL |= ppu::ctrl::POLARITY;
    const bool vertical = ppu::PPUCTRL & ppu::ctrl::POLARITY;
    // Re-derive the full page-aware address per tile rather than walking a
    // single precomputed offset by a flat stride: a run that crosses a
    // 32-tile nametable page boundary (row for horizontal writes, or the
    // 30-tile column boundary for vertical ones) needs to land in the next
    // page at the same row/col, not fall through into the next row/col of
    // the SAME page the way raw offset+i arithmetic would. Only visible on
    // viewports wider/taller than one page (e.g. OGC's runtime ~40-tile
    // width), which is why this stayed latent on the fixed-32-wide NES.
    for (u8 i = 0; i < sBuffer; i++) {
        const u16 addr = vertical ? xy_to_nt_addr(x, static_cast<u16>(y + i))
                                   : xy_to_nt_addr(static_cast<u16>(x + i), y);
        ppu::WriteNametable(addr, source[i]);
    }
}

// Address overload -- see ::nt_addr_to_xy's own comment for why this
// reconstructs (x,y) and defers to the pos-based version above instead of
// re-deriving the page-wrap logic against a flat address.
void WriteFromBufferToNameTable(
    const u16 address, const u8* source, const u8 sBuffer, const u8 polarity
) {
    u16 x, y;
    nt_addr_to_xy(address, x, y);
    WriteFromBufferToNameTable({x, y}, source, sBuffer, polarity);
}

void WriteRepeatedToNameTable(
    const vec2<u16> pos, const u8 value, const u8 amt, u8 polarity
) {
    const u16 x = pos.x, y = pos.y;
    ppu::PPUCTRL &= ~ppu::ctrl::POLARITY;
    if (polarity) ppu::PPUCTRL |= ppu::ctrl::POLARITY;
    const bool vertical = ppu::PPUCTRL & ppu::ctrl::POLARITY;

    // See WriteFromBufferToNameTable above: page-aware address per tile, not
    // a flat stride off one precomputed offset.
    for (u8 i = 0; i < amt; i++) {
        const u16 addr = vertical ? xy_to_nt_addr(x, static_cast<u16>(y + i))
                                   : xy_to_nt_addr(static_cast<u16>(x + i), y);
        ppu::WriteNametable(addr, value);
    }
}

// Address overload -- see ::nt_addr_to_xy's own comment.
void WriteRepeatedToNameTable(
    const u16 address, const u8 value, const u8 amt, const u8 polarity
) {
    u16 x, y;
    nt_addr_to_xy(address, x, y);
    WriteRepeatedToNameTable({x, y}, value, amt, polarity);
}

void WriteSingleToNameTable(const vec2<u16> pos, u8 value) {
    const u16 offset = xy_to_nt_addr(pos.x, pos.y);
    ppu::WriteNametable(offset, value);
}

// Address overload: @p address is the 0-based VRAM offset CartesianToAddress returns
// on this backend (xy_to_nt_addr is already 0-based here), so it routes through the
// same nametable accessor -- the desktop mirror of the NES poke-by-address path.
void WriteSingleToNameTable(const u16 address, u8 value) {
    ppu::WriteNametable(address, value);
}

void SetScroll(const vec2<u16> pos) {
    xScroll = pos.x; yScroll = pos.y;
}

void DeltaScroll(const vec2<i8> delta) {
    xScroll = static_cast<u16>(xScroll + delta.x);
    yScroll = static_cast<u16>(yScroll + delta.y);
}

template <typename Idx>
void WriteFromProviderToNameTable(
    const vec2<u16> pos, u8 (*fn)(Idx), const u8 amt,
    const u8 polarity
) {
    const u16 x = pos.x, y = pos.y;
    ppu::PPUCTRL &= ~ppu::ctrl::POLARITY;
    if (polarity) ppu::PPUCTRL |= ppu::ctrl::POLARITY;
    const bool vertical = ppu::PPUCTRL & ppu::ctrl::POLARITY;

    // See WriteFromBufferToNameTable above: page-aware address per tile, not
    // a flat stride off one precomputed offset.
    for (Idx i = 0; i < amt; ++i) {
        const u16 addr = vertical ? xy_to_nt_addr(x, static_cast<u16>(y + i))
                                   : xy_to_nt_addr(static_cast<u16>(x + i), y);
        ppu::WriteNametable(addr, fn(i));
    }
}

// Explicit instantiations for the provider index types in use. The body writes
// host video RAM, so it must stay in this backend rather than the header.
template void WriteFromProviderToNameTable<u8>(vec2<u16>, u8 (*)(u8), u8, u8);
template void WriteFromProviderToNameTable<u16>(vec2<u16>, u8 (*)(u16), u8, u8);

// Address overload -- see ::nt_addr_to_xy's own comment.
template <typename Idx>
void WriteFromProviderToNameTable(
    const u16 address, u8 (*fn)(Idx), const u8 amt, const u8 polarity
) {
    u16 x, y;
    nt_addr_to_xy(address, x, y);
    WriteFromProviderToNameTable({x, y}, fn, amt, polarity);
}

template void WriteFromProviderToNameTable<u8>(u16, u8 (*)(u8), u8, u8);
template void WriteFromProviderToNameTable<u16>(u16, u8 (*)(u16), u8, u8);

void WriteFromBufferToAttributeTable(
    const vec2<u16> pos, const u8* source,
    const u8 sBuffer, const u8 polarity
) {
    const u16 x = pos.x, y = pos.y;
    // Horizontal (polarity 0) runs can cross an nt_h page on any viewport
    // wider than 32 tiles (OGC/Switch/WiiU/PSP), so those need the full
    // page-aware address recomputed per cell -- see WriteFromBufferToNameTable
    // above. Vertical (polarity 1) runs never cross a page: a nametable's
    // height is always exactly the viewport's own (::emu::ComputeNtGeometry),
    // so nt_v is always 0 -- walking the flat row-bucket stride off one base
    // address is correct and avoids reintroducing xy_to_at_addr's y%tileH
    // wraparound, which trips incorrectly for a run whose start row isn't
    // itself a multiple of 4 (e.g. level.cpp's ground column starts at y=2).
    if (polarity) {
        const u16 offset = xy_to_at_addr(x, y);
        const u16 atStride = emu::ComputeNtGeometry().atW;
        for (u8 i = 0; i < sBuffer; i++) {
            ppu::WriteNametable(static_cast<u16>(offset + i * atStride), source[i]);
        }
    } else {
        for (u8 i = 0; i < sBuffer; i++) {
            ppu::WriteNametable(xy_to_at_addr(static_cast<u16>(x + i * 4), y), source[i]);
        }
    }
}

// Address overload -- see ::at_addr_to_xy's own comment.
void WriteFromBufferToAttributeTable(
    const u16 address, const u8* source, const u8 sBuffer, const u8 polarity
) {
    u16 x, y;
    at_addr_to_xy(address, x, y);
    WriteFromBufferToAttributeTable({x, y}, source, sBuffer, polarity);
}

void WriteRepeatedToAttributeTable(
    const vec2<u16> pos, const u8 value, const u8 amt, const u8 polarity
) {
    const u16 x = pos.x, y = pos.y;
    // See WriteFromBufferToAttributeTable above: vertical stays a flat stride
    // off one base address; horizontal recomputes the page-aware address per cell.
    if (polarity) {
        const u16 offset = xy_to_at_addr(x, y);
        const u16 atStride = emu::ComputeNtGeometry().atW;
        for (u8 i = 0; i < amt; i++) {
            ppu::WriteNametable(static_cast<u16>(offset + i * atStride), value);
        }
        return;
    }
    for (u8 i = 0; i < amt; i++) {
        ppu::WriteNametable(xy_to_at_addr(static_cast<u16>(x + i * 4), y), value);
    }
}

// Address overload -- see ::at_addr_to_xy's own comment.
void WriteRepeatedToAttributeTable(
    const u16 address, const u8 value, const u8 amt, const u8 polarity
) {
    u16 x, y;
    at_addr_to_xy(address, x, y);
    WriteRepeatedToAttributeTable({x, y}, value, amt, polarity);
}

void WriteSingleToAttributeTable(const vec2<u16> pos, const u8 value) {
    const u16 offset = xy_to_at_addr(pos.x, pos.y);
    ppu::WriteNametable(offset, value);
}

// Address overload -- see ::at_addr_to_xy's own comment.
void WriteSingleToAttributeTable(const u16 address, const u8 value) {
    ppu::WriteNametable(address, value);
}

template <typename Idx>
void WriteFromProviderToAttributeTable(
    const vec2<u16> pos, u8 (*fn)(Idx), const u8 amt,
    const u8 polarity
) {
    const u16 x = pos.x, y = pos.y;
    // See WriteFromBufferToAttributeTable above: vertical stays a flat stride
    // off one base address (nt_v is always 0, never crosses a page);
    // horizontal recomputes the page-aware address per cell.
    if (polarity) {
        const u16 offset = xy_to_at_addr(x, y);
        const u16 atStride = emu::ComputeNtGeometry().atW;
        for (Idx i = 0; i < amt; ++i) {
            ppu::WriteNametable(static_cast<u16>(offset + i * atStride), fn(i));
        }
        return;
    }
    for (Idx i = 0; i < amt; ++i) {
        const u16 addr = xy_to_at_addr(static_cast<u16>(x + i * 4), y);
        ppu::WriteNametable(addr, fn(i));
    }
}

// Explicit instantiations for the provider index types in use. The body writes
// host video RAM, so it must stay in this backend rather than the header.
template void WriteFromProviderToAttributeTable<u8>(vec2<u16>, u8 (*)(u8), u8, u8);
template void WriteFromProviderToAttributeTable<u16>(vec2<u16>, u8 (*)(u16), u8, u8);

// Address overload -- see ::at_addr_to_xy's own comment.
template <typename Idx>
void WriteFromProviderToAttributeTable(
    const u16 address, u8 (*fn)(Idx), const u8 amt, const u8 polarity
) {
    u16 x, y;
    at_addr_to_xy(address, x, y);
    WriteFromProviderToAttributeTable({x, y}, fn, amt, polarity);
}

template void WriteFromProviderToAttributeTable<u8>(u16, u8 (*)(u8), u8, u8);
template void WriteFromProviderToAttributeTable<u16>(u16, u8 (*)(u16), u8, u8);

u16 CartesianToAddress(const vec2<u16> pos) {
    return xy_to_nt_addr(pos.x, pos.y);
}

scroll_t CartesianToScroll(const vec2<u16> pos) {
    return (scroll_t){ .x = pos.x, .y = pos.y };
}

void SetColorPriority(const u8 priority) {
    ppu::PPUMASK = (ppu::PPUMASK & ~(ppu::mask::RED | ppu::mask::GREEN | ppu::mask::BLUE)) |
        (priority & (ppu::mask::RED | ppu::mask::GREEN | ppu::mask::BLUE)
    );
}

namespace pal {

void WriteFromBuffer(const u8 offset, const u8* source, const u8 sBuffer) {
    memcpy(paletteRAM + offset, source, sBuffer);
}

void WriteSingle(const u8 offset, const u8 value) {
    paletteRAM[offset] = value;
}

}   // namespace pal

}   // namespace ppu

namespace oam {

void OAMFromBuffer(sprite_t* oam, const u8 slot, const u16 off,
                   const u8 width, const u8* src, const u16 count) {
    u8* dst = reinterpret_cast<u8 *>(oam) + static_cast<size_t>(slot) * spriteStride + off;
    const u8* s = src + off;
    for (u16 i = 0; i < count; i++)
        memcpy(dst + static_cast<size_t>(i) * spriteStride, s + static_cast<size_t>(i) * spriteStride, width);
}

void OAMFromProvider(sprite_t* oam, const u8 slot, const u16 off,
                     const u8 width, oam_t (*fn)(u16), const u16 count) {
    u8* base = reinterpret_cast<u8 *>(oam) + static_cast<size_t>(slot) * spriteStride + off;
    for (u16 i = 0; i < count; i++) {
        oam_t v = fn(i);
        memcpy(base + static_cast<size_t>(i) * spriteStride, &v, width);  /* low `width` bytes (LE) */
    }
}

/* Backend-agnostic analogue of OAMDMA: freeze the passed OAM buffer into the
 * PPU-side snapshot that GenerateFrame renders from. Called from the app's NMI. */
void RefreshSprites(const sprite_t* oam) {
    memcpy(oamShadow, oam, OAM_SPRITES * sizeof(struct sprite_t));
}

}   // namespace oam

namespace ppu {

void StreamFromVideoMemory(const u16 offset, atomic u8* target, const u8 size) {
    for (u8 i = 0; i < size; i++) {
        target[i] = ppu::ReadNametable(static_cast<u16>(offset + i));
    }
}

}   // namespace ppu

void video::WaitThenReactToSpriteZero(const vec2<u16> pos, void (*fn)(), atomic u8* latch) {
    *latch = true;
    video::SetSpriteZeroHandler(pos, fn);
    irq::irqHandler      = fn;
    irq::irqPosition     = pos;
    irq::irqPendingValid = true;
}
