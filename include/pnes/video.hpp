/**
 * @file video.hpp
 * @brief PPU abstraction: CHR ROM embedding, OAM, scrolling,
 *        nametable / attribute / palette writes, sprite-zero events,
 *        and viewport geometry.
 *
 * The NES build targets the real PPU registers at \$2000-\$2007 and
 * OAM DMA at \$4014. The SDL3 build presents an equivalent software
 * framebuffer with the same register conventions, so application code
 * can compile unchanged against either backend.
 *
 * Two areas need care:
 *
 * - **CHR ROM linkage.** The CHR image is placed in the
 *   platform-specific ::_CHR_SECTION so the linker lands it on the PPU's
 *   8 KB boundary (the blob itself is packed by tools/blobman).
 * - **Video-memory writes.** Poking the PPU outside VBlank corrupts the
 *   display; the ::VRAM block toggles ::PPUMASK around its body.
 */
#pragma once

#include <intsh>
using namespace br0::intsh;
#include <cstddef>
#include <array>

#include "types.hpp"
#include "technology.hpp"

// The SDL desktop backend pulls in SDL3's display-mode types here. The libogc
// (GameCube/Wii), 3DS (citro2d), Switch (libnx framebuffer), Wii U, PSP and
// Nintendo DS backends are emulated-PPU builds like SDL but present through
// console graphics, so they must NOT see any SDL3 headers. (The Wii U backend
// DOES use SDL -- but SDL2, from the devkitPro Wii U portlib -- which it
// includes itself in src/wiiu; it must not pull in SDL3. The PSP backend writes
// straight into VRAM with no GU/SDL at all -- see src/psp/video.cpp. The DS and
// GBA backends drive the libnds/libgba 2D hardware directly, with no SDL at
// all. The PS2 backend (src/ps2) presents through the GS via gsKit -- a real
// GPU, like GX/libnx, not raw VRAM like PSP -- so it must not see SDL3
// either. The PS3 backend (src/ps3) is back to the PSP's shape rather than
// the PS2's: it blits into an RSX-mapped buffer and flips it, with no
// shaders/3D pipeline involved, so it must not see SDL3 either.) All are
// "non-NES", so the SDL3-only includes/globals are gated on
// (!TARGET_NES && !OGC && !CTR && !NX && !WIIU && !PSP && !NDS && !GBA &&
// !PS2 && !PS3).
#if !defined(TARGET_NES) && !defined(TARGET_OGC) && !defined(TARGET_CTR) && !defined(TARGET_NX) && !defined(TARGET_WIIU) && !defined(TARGET_PSP) && !defined(TARGET_NDS) && !defined(TARGET_GBA) && !defined(TARGET_PS2) && !defined(TARGET_PS3)
#include <SDL3/SDL_video.h>
#endif

namespace video {
/** @brief Base PPU address of the pattern tables (\$0000 / \$1000). */
extern const u16 PatternTables;
} // namespace video

/**
 * @brief Number of sprites in an OAM region.
 *
 * An OAM buffer is simply a pointer to `OAM_SPRITES` consecutive
 * ::sprite_t records — `OAM_SPRITES * sizeof(struct sprite_t)` bytes —
 * on both NES and desktop. Application code owns the storage and passes
 * the pointer to every OAM call, so several independent buffers may
 * coexist.
 */
#define OAM_SPRITES 64

/* ------------------------------------------------------------------------ *
 *  CHR ROM placement                                                       *
 *                                                                          *
 *  The cartridge's CHR ROM is a single image object placed in the CHR      *
 *  section below. Its layout is declared in a blobman config (see          *
 *  tools/blobman and demo/chr/config.toml): blobman packs the whole blob,  *
 *  already padded to the full image size, and emits a header of symbolic   *
 *  tile ids beside it, so tile ids are never hand-assigned and are usable  *
 *  in `constexpr` tables. One TU `#embed`s that blob into                  *
 *  ::_CHR_SECTION and defines ::ppu::chrRomBytes -- see                    *
 *  demo/src/graphics/chr_rom.cpp.                                          *
 * ------------------------------------------------------------------------ */

/** @brief Platform CHR section name, for `[[gnu::section]]` placement. */
#ifdef TARGET_NES
  #define _CHR_SECTION ".chr_rom"
#elif defined(_WIN32)
  #define _CHR_SECTION "chr_rom$m"
#elif defined(__APPLE__)
  #define _CHR_SECTION "__DATA,chr_rom"
#else
  #define _CHR_SECTION "chr_rom"
#endif

#ifndef TARGET_NES
  #if defined(_WIN32)
    // C linkage so the symbol is the bare `_chr_rom` defined by the inline-asm
    // block in video.cpp. As a C++ variable it would take the MSVC-mangled
    // name and fail to resolve against that undecorated asm label at link time.
    extern "C" const u8 _chr_rom[];
  #elif defined(__APPLE__)
    extern const u8 _chr_rom[] __asm("section$start$__DATA$chr_rom");
  #else
    extern const u8 _chr_rom[] __asm("__start_chr_rom");
  #endif
  /** @brief Base pointer to the merged CHR ROM section (desktop). */
  #define CHR_ROM ((const u8 *)_chr_rom)
#endif

namespace oam {
#ifndef TARGET_NES
    /** @brief OAM coordinate type — 16-bit on desktop to allow off-screen sprites. */
    typedef u16 oam_t;
#else
    /** @brief OAM coordinate type — 8-bit on NES to match hardware OAM layout. */
    typedef u8 oam_t;
#endif

    /**
     * @brief A single sprite in the object attribute memory layout.
     */
    struct sprite_t {
        oam_t y;            /**< Y coordinate of the top-left corner. */
        u8 tile;         /**< Pattern table tile index. */
        u8 attributes;   /**< Palette select, priority, flip flags. */
        oam_t x;            /**< X coordinate of the top-left corner. */
    };

    constexpr auto spriteStride =  sizeof(struct sprite_t);
    constexpr auto spriteSlot(const u16 slot) {return slot * spriteStride;}

    /**
     * @brief Compile-time selector for a ::sprite_t field.
     *
     * Replaces the old token-pasting macros: a tag carries the field's
     * byte @c offset and @c width so the OAM populate functions can
     * forward them to the backend with zero runtime cost. Pass one of the
     * predefined tags ::oam::y, ::oam::tile, ::oam::attributes, ::oam::x.
     */
    template<auto Off, auto Width>
    struct field_t {
        static constexpr u16 offset = Off;   /**< Byte offset within sprite_t. */
        static constexpr u8  width  = Width; /**< Field width in bytes.        */
    };

    /** @brief Field tag for ::sprite_t::y. */
    inline constexpr field_t<offsetof(sprite_t, y),          sizeof(sprite_t::y)>          y{};
    /** @brief Field tag for ::sprite_t::tile. */
    inline constexpr field_t<offsetof(sprite_t, tile),       sizeof(sprite_t::tile)>       tile{};
    /** @brief Field tag for ::sprite_t::attributes. */
    inline constexpr field_t<offsetof(sprite_t, attributes), sizeof(sprite_t::attributes)> attributes{};
    /** @brief Field tag for ::sprite_t::x. */
    inline constexpr field_t<offsetof(sprite_t, x),          sizeof(sprite_t::x)>          x{};

    void OAMFromProvider(sprite_t *oam, u8 slot, u16 off,
                 u8 width, oam_t (*fn)(u16), u16 count);
    void OAMFromBuffer(sprite_t *oam, u8 slot, u16 off,
                       u8 width, const u8 *src, u16 count);

    /**
     * @brief Writes one ::sprite_t field across @p count consecutive sprites.
     *
     * Hardware-specific OAM write: the sprite stride is baked in, and the
     * field's byte offset and width are carried by the @p field tag at
     * compile time. On NES every field is one byte; on desktop ::oam_t
     * coordinate fields are two bytes and are written in full, so off-screen
     * sprite positions are preserved. The provider returns ::oam_t.
     *
     * @param buf    OAM buffer to write into.
     * @param slot   First sprite index to write.
     * @param field  A ::oam::field_t tag (::oam::x, ::oam::y, ::oam::tile, ...).
     * @param fn     Provider returning the value for iteration `i`.
     * @param count  Number of sprites to write.
     */
    template<auto Off, auto Width>
    void PopulateFromProvider(sprite_t *buf, u8 slot,
                                        field_t<Off, Width> /*field*/,
                                        oam_t (*fn)(u16), u16 count) {
        OAMFromProvider(buf, slot, Off, Width, fn, count);
    }

    /**
     * @brief Copies one ::sprite_t field into @p count consecutive sprites.
     *
     * The buffer counterpart of ::oam::PopulateFromProvider: instead of a
     * callback, each value is read from @p src, which is laid out as
     * ::sprite_t records (e.g. a metasprite table). The field selected by the
     * @p field tag is copied from `src[i]` to `buf[slot + i]`; the sprite
     * stride, the field offset and its width are rest handled internally.
     *
     * @param buf    OAM buffer to write into.
     * @param slot   First sprite index to write.
     * @param field  A ::oam::field_t tag (::oam::x, ::oam::tile, ...).
     * @param src    Source laid out as ::sprite_t records.
     * @param count  Number of sprites to write.
     */
    template<auto Off, auto Width>
    void PopulateFromBuffer(sprite_t *buf, u8 slot,
                            field_t<Off, Width> /*field*/,
                            const void *src, u16 count) {
        OAMFromBuffer(buf, slot, Off, Width,
                      static_cast<const u8 *>(src), count);
    }
}

namespace ppu {
    /**
     * @brief Total bytes of the embedded flat CHR ROM image -- defined by the
     *        TU that embeds the image, alongside the image itself, so it
     *        always matches exactly what's actually embedded, on every
     *        target.
     *
     * Defined unconditionally, so the symbol exists on NES too, simply
     * unused. Off-NES tile-address translators wrap
     * their resolved offset against this, so a bank register addressing past
     * what the build embeds cannot read out of bounds: the software
     * counterpart to a smaller CHR chip's address pins aliasing.
     */
    extern const unsigned chrRomBytes;

    namespace raw {
        enum PPU {
            PPUCTRL     = 0x2000, /**< Base nametable, VRAM increment, NMI enable. */
            PPUMASK     = 0x2001, /**< Rendering enable and color emphasis. */
            PPUSTATUS   = 0x2002, /**< VBlank, sprite 0, sprite-overflow flags. */
            OAMADDR     = 0x2003, /**< OAM write address. */
            OAMDATA     = 0x2004, /**< OAM read/write data port. */
            PPUSCROLL   = 0x2005, /**< Fine X/Y scroll write. */
            PPUADDR     = 0x2006, /**< VRAM address write. */
            PPUDATA     = 0x2007, /**< VRAM data port. */

            OAMDMA      = 0x4014  /**< OAM DMA transfer trigger. */
        };
    }


    namespace ctrl {
        enum CTRL {
          GEN_NMI     = 0x80, /**< Generate NMI on VBlank. */
          POLARITY    = 0x04, /**< VRAM address auto-increment direction (horizontal/vertical). */
          BG_ADDR     = 0x10, /**< Background pattern table at \$1000 (otherwise \$0000). */
          SPRITE_ADDR = 0x08, /**< Sprite pattern table at \$1000 (otherwise \$0000). Ignored in 8x16 mode. */
          /** @brief 8x16 sprites. ::oam::sprite_t::tile bit 0 then selects the
           *  pattern table (overriding ::SPRITE_ADDR) and bits 7-1 select the tile
           *  pair: top tile is `tile & 0xFE`, bottom tile is `(tile & 0xFE) + 1`. */
          SPRITE_SIZE = 0x20
      };
    }

    namespace mask {
        enum MASK {
          BG             = 0x08, /**< Show background. */
          SPRITE         = 0x10, /**< Show sprites. */
          BG_L           = 0x0a, /**< Show background in the left 8 pixels of the screen. */
          SPRITE_L       = 0x14, /**< Show sprites in the left 8 pixels of the screen. */
          RED            = 0x20, /**< Emphasise red. */
          GREEN          = 0x40, /**< Emphasise green. */
          BLUE           = 0x80, /**< Emphasise blue. */
      };
    }

#ifdef TARGET_NES
    /**
     * @brief Platform-specific scroll encoding (NES: packed PPU registers).
     *
     * Layout:
     * - `data[0]` = ::PPUCTRL byte, nametable select already merged in.
     * - `data[1]` = fine X scroll (`px & 0xFF`).
     * - `data[2]` = fine Y scroll (`py % 240` after nametable wrap).
     */
    typedef struct { u8 data[3]; } scroll_t;
#else
    /** @brief Platform-specific scroll encoding (desktop: plain XY pixels). */
    typedef vec2<u16> scroll_t;
#endif

    /**
     * @brief Palette selector values for attribute-table writes.
     *
     * Each value encodes a palette index in the upper bits of an
     * attribute byte: BG_N selects background palette N; SPRITE_N selects
     * sprite palette N.
     */
        enum PALETTE {
        BG_0          = 0 << 2, /**< Background palette 0. */
        BG_1          = 1 << 2, /**< Background palette 1. */
        BG_2          = 2 << 2, /**< Background palette 2. */
        BG_3          = 3 << 2, /**< Background palette 3. */
        SPRITE_0      = 4 << 2, /**< Sprite palette 0. */
        SPRITE_1      = 5 << 2, /**< Sprite palette 1. */
        SPRITE_2      = 6 << 2, /**< Sprite palette 2. */
        SPRITE_3      = 7 << 2, /**< Sprite palette 3. */
    };

    /**
     * @brief Converts a pixel position into a PPU VRAM address.
     * @param pos Tile position.
     * @return  Absolute PPU address of the corresponding nametable byte.
     */
    u16 CartesianToAddress(const vec2<u16>& pos);

    /**
     * @brief Overload for a position already read out of volatile storage
     *        (e.g. a UI widget's live on-screen position). Copies out of
     *        @p pos once and forwards to the non-volatile overload.
     *
     * Taking @p pos by reference (rather than by value) here and above
     * matters: both overloads must stay in the reference-binding tier so a
     * non-volatile caller resolves to the non-volatile overload by a strict
     * best match (identity vs. a volatile-qualification adjustment) instead
     * of being ambiguous against this one.
     * @param pos Tile position.
     * @return  Absolute PPU address of the corresponding nametable byte.
     */
    inline u16 CartesianToAddress(const volatile vec2<u16>& pos) {
        return CartesianToAddress(vec2<u16>{pos.x, pos.y});
    }

    /**
     * @brief Overload for a caller that already holds a precomputed PPU
     *        address rather than a tile position -- passes it through
     *        unchanged so generic callers don't need to special-case it.
     * @param pos Precomputed PPU address.
     * @return  @p pos, unchanged.
     */
    constexpr u16 CartesianToAddress(const u16 pos) {
        return pos;
    }


    void StreamFromVideoMemory(u16 offset, atomic u8* target, u8 size);

#ifndef TARGET_NES
    /**
     * @brief Resolves a tile's PPU pattern-table address (0x0000-0x1FFF, the
     *        same value the emu PPU would otherwise index ::patternTable
     *        with directly) into the byte offset to actually index
     *        ::patternTable with -- i.e. that address resolved through
     *        whatever CHR banks are currently switched in.
     *
     * Not a fetch: returns an address, the caller does the read. Weak default is
     * identity, matching a board with no CHR banking; a bank-switching mapper's
     * emu-side TU supplies a strong definition resolving through its own
     * switched banks. Game code always references other symbols from that TU,
     * so it is never optional in the link and the strong definition wins with
     * no runtime dispatch.
     *
     * Called per tile by the emu PPU's per-pixel fetches, and by the
     * native-2D backends when baking their tile atlas -- see ::chrGeneration.
     *
     * @param tileVMA PPU pattern-table address, 0x0000-0x1FFF.
     * @return        Byte offset to index ::CHR_ROM with.
     */
    u32 ResolveTile(u16 tileVMA);

    /**
     * @brief Bumped by a mapper every time it switches a CHR bank off-NES.
     *
     * The per-pixel software rasterizer (src/emu/ppu.cpp) re-resolves every
     * tile fetch through ::ResolveTile already, so it never needs this.
     * It exists for the native-hardware-2D backends (GBA/DS/3DS/GameCube-Wii)
     * that instead bake a whole tile atlas once and index into it by NES tile
     * number: they compare this counter once per frame against the value they
     * last rebuilt at, and re-bake the atlas (through ::ResolveTile) only
     * when it has moved, rather than re-expanding all 512 tiles every frame
     * regardless of whether any CHR bank actually changed.
     */
    extern u32 chrGeneration;

    /**
     * @brief Reads one byte of nametable/attribute VRAM at a flattened
     *        logical offset -- the same value ::CartesianToAddress / the
     *        internal xy_to_nt_addr / xy_to_at_addr already compute: one
     *        ::video::nametable_plane_bytes()-sized page per physical
     *        nametable, ::ppu::nametableCount pages total, all inside the one
     *        flat ::VideoRAM allocation (see ::nametableCount's own comment
     *        for how a logical (x,y) maps onto a page).
     *
     * Weak default is `VideoRAM[logical]`, correct for every board: unlike
     * real NES hardware (a fixed 2 KiB on the console plus, on some board
     * wirings, a separate cartridge-side chip), nothing here needs routing to
     * a second buffer -- ::vram_bytes() already sizes ::VideoRAM for every
     * page ::nametableCount claims. A board could still supply a strong
     * definition (same relationship as ::ResolveTile) if it ever needed to
     * answer from somewhere else, but none currently do.
     *
     * @param logical Flattened nametable/attribute offset.
     * @return        The byte at that offset.
     */
    u8 ReadNametable(u16 logical);

    /**
     * @brief Writes one byte of nametable/attribute VRAM. Write counterpart
     *        of ::ReadNametable -- see its own comment for the addressing
     *        and weak/strong relationship.
     *
     * @param logical Flattened nametable/attribute offset.
     * @param value   Byte to store.
     */
    void WriteNametable(u16 logical, u8 value);

    /**
     * @brief How many physical nametables the linked mapper provides -- a
     *        board property, fixed at link time, independent of viewport size
     *        or the runtime mirroring switch: 2 for an ordinary
     *        switchable-mirroring board (e.g. MMC3), 4 for a four-screen
     *        board (a real extra cartridge VRAM chip, no aliasing needed), 1
     *        for a fixed single-screen board.
     *
     * This is the off-NES equivalent of what real NES mirroring wiring gives
     * for free: on hardware, $2000/$2400/$2800/$2C00 always exist as logical
     * addresses, and mirroring is just which of those alias the same physical
     * chip. Off-NES, every backend (the per-pixel SDL core and each
     * native-tilemap backend) derives the SAME arrangement from this value
     * together with the runtime ::mirroring flag: a 2-nametable board grids as
     * 2-wide/1-tall when ::mirroring is vertical (false) or 1-wide/2-tall when
     * horizontal (true); a 4-nametable board always grids 2x2, ::mirroring
     * playing no part (matching a real four-screen board's own $A000 write
     * being meaningless -- each quadrant already has fixed, dedicated
     * storage). Each nametable in that grid matches this viewport's own size
     * (floored at the NES-native 32x30 minimum -- see
     * ::emu::ComputeNtGeometry's own doc comment, emu.hpp, for why: real
     * hardware only forces exactly 32x30 for a target whose viewport crops a
     * fixed background, e.g. GBA/NDS/DSi -- everything else's viewport
     * already IS the addressable world, whatever size it runtime-renders at).
     *
     * Weak default `2`; a board needing a different count supplies a strong
     * definition, same relationship as ::ResolveTile.
     */
    extern const u8 nametableCount;
#endif
}
#ifndef TARGET_NES
/**
 * @brief Desktop shadow of MMC3's $A000 mirroring bit: false = vertical,
 *        true = horizontal. Forward-declared here (full doc comment below,
 *        near ::VideoRAM) so ::video::nametable_grid_w()/::nametable_grid_h()
 *        can read it -- those are declared inside `namespace video`, further
 *        down this file, ahead of ::mirroring's own declaration otherwise.
 */
extern bool mirroring;
#endif
#if !defined(TARGET_NES) && !defined(TARGET_OGC) && !defined(TARGET_CTR) && !defined(TARGET_NX) && !defined(TARGET_WIIU) && !defined(TARGET_PSP) && !defined(TARGET_NDS) && !defined(TARGET_GBA) && !defined(TARGET_PS2) && !defined(TARGET_PS3)
/** @brief Current desktop display mode (window + refresh info). SDL backend only. */
extern const SDL_DisplayMode* mode;
/** @brief Integer upscaling factor applied to the NES virtual framebuffer. SDL backend only. */
extern u8 scale;
#endif
#if defined(TARGET_OGC)
/** @brief World columns visible this run (tiles). The GameCube/Wii TV output
 *         resolution is runtime-only, so this is computed once at init from the
 *         real TV pixel width (VIDEO_GetPreferredMode) rather than being a
 *         compile-time constant. GC/Wii backend only. */
extern u16 ogc_world_tx;
#endif

namespace video {
#if defined(TARGET_NES)
    // Real NES PPU hardware: physically fixed at 32x30 tiles (256x240px). There
    // is no "cover a bigger screen" here -- the console IS the screen.
    /** @brief Viewport width in tiles (NES: fixed 32, hardware PPU). */
    constexpr u16 viewport_tx() { return 32; }
    /** @brief Viewport height in tiles (NES: fixed 30, hardware PPU). */
    constexpr u16 viewport_ty() { return 30; }
    /** @brief Viewport width in pixels (tiles * 8). */
    constexpr u16 viewport_px() { return viewport_tx() << 3; }
    /** @brief Viewport height in pixels (tiles * 8). */
    constexpr u16 viewport_py() { return viewport_ty() << 3; }
#elif defined(TARGET_OGC)
    // The GameCube/Wii TV output resolution (NTSC/PAL, 4:3/16:9,
    // interlace/progressive) is known only at runtime, unlike the fixed 3DS
    // panel below. Height stays pinned to the NES's 30 rows/240px; width is
    // ogc_world_tx, computed once at init (src/ogc/video.cpp) from the real TV
    // pixel width, so GX renders more of the game world to fill a wider screen
    // instead of stretching a fixed 256px image -- the same "render more world"
    // model the SDL LANDSCAPE desktop path uses. A tiny/unusual TV mode simply
    // crops the camera (ogc_world_tx can resolve below 32); the underlying
    // nametable VRAM is sized separately to never go below the NES's own
    // 2-page/0x800-byte minimum (see the InitMemory call in src/ogc/video.cpp).
    /** @brief Viewport width in tiles (GC/Wii: runtime, covers the real TV width). */
    constexpr u16 viewport_tx() { return ogc_world_tx; }
    /** @brief Viewport height in tiles (GC/Wii: fixed 30, matching the NES). */
    constexpr u16 viewport_ty() { return 30; }
    /** @brief Viewport width in pixels (tiles * 8). */
    constexpr u16 viewport_px() { return viewport_tx() << 3; }
    /** @brief Viewport height in pixels (tiles * 8). */
    constexpr u16 viewport_py() { return viewport_ty() << 3; }
#elif defined(TARGET_CTR)
    // The 3DS presents the viewport across BOTH panels, so the viewport is the
    // bounding box of the two rather than the top screen alone: 400x240 (top)
    // and 320x240 (bottom), stacked on their shared horizontal edge. Width is
    // the WIDEST panel -- 400px/50 tiles, the top screen -- and height is the
    // SUM of the two panel heights, 240+240 = 480px/60 tiles. The 80px the
    // bottom panel lacks becomes a 40px (5-tile) inset on each side; that split
    // lives in exactly one place, ::panel_rect() below, and nowhere else.
    //
    // The physical hinge gap is deliberately NOT modelled: viewport row 239
    // (bottom of the top panel) and row 240 (top of the bottom panel) are
    // adjacent in world space, so background crossing the seam is continuous
    // and merely interrupted by the bezel. Both panels have the same ~132ppi
    // pixel pitch, so 1:1 native scale is correct on both with no resampling.
    //
    // Both axes are the same "render more world" model every non-hardware
    // backend already uses, now applied vertically as well as horizontally: 60
    // rows of real game world drawn native 1:1, not a stretched 256x240 image.
    /** @brief Viewport width in tiles (3DS: 50 = the 400px top panel, the wider of the two). */
    constexpr u16 viewport_tx() { return 50; }
    /** @brief Viewport height in tiles (3DS: 60 = both 240px panels stacked, 480px). */
    constexpr u16 viewport_ty() { return 60; }
    /** @brief Viewport width in pixels (tiles * 8 = 400). */
    constexpr u16 viewport_px() { return viewport_tx() << 3; }
    /** @brief Viewport height in pixels (tiles * 8 = 480, both panels). */
    constexpr u16 viewport_py() { return viewport_ty() << 3; }
#elif defined(TARGET_NDS)
    // The Nintendo DS (and DSi) presents the viewport across BOTH panels, so the
    // viewport is the bounding box of the two. Both panels are 256x192 and are
    // stacked on their shared horizontal edge, so width stays the NES-native
    // 256px/32 tiles and height is the SUM of the two: 192+192 = 384px/48 tiles.
    // The panels are identical here (unlike the 3DS's 400/320 split), so there
    // is no horizontal inset -- but ::panel_rect() below still owns the split,
    // so no backend re-derives it. The hinge gap is deliberately not modelled;
    // see TARGET_CTR's own comment above for why.
    //
    // This makes the DS the first target TALLER than the NES's 30 rows rather
    // than shorter. It used to show a 256x192 WINDOW that cropped the bottom
    // 48px of the NES frame off the panel; it now shows 384px of real game world
    // spread across the two panels, which is the same "render more world" model
    // the wider-panel backends already use, applied vertically. Any code reading
    // video::viewport_ty() must not assume 30 -- in EITHER direction.
    /** @brief Viewport width in tiles (DS/DSi: 32, full NES width -- both panels are 256px). */
    constexpr u16 viewport_tx() { return 32; }
    /** @brief Viewport height in tiles (DS/DSi: 48 = both 192px panels stacked, 384px). */
    constexpr u16 viewport_ty() { return 48; }
    /** @brief Viewport width in pixels (tiles * 8 = 256). */
    constexpr u16 viewport_px() { return viewport_tx() << 3; }
    /** @brief Viewport height in pixels (tiles * 8 = 384, both panels). */
    constexpr u16 viewport_py() { return viewport_ty() << 3; }
#elif defined(TARGET_GBA)
    // The Game Boy Advance panel is 240x160 -- the first target NARROWER than the
    // NES horizontally (30 tiles vs 32) as well as shorter (20 tiles vs 30). Same
    // family as the DS: the NES game is never compromised, so the GBA shows a
    // 240x160 WINDOW onto the same 256x240 world the game renders, cropping 2 tiles
    // (16px) of width and 10 tiles (80px) of height. The backend (src/gba/video.cpp)
    // maps the NES PPU onto the GBA's native 2D hardware (BG tilemap + hardware OBJ
    // + per-scanline HBlank scroll), so raster splits and sprite-0 still work.
    //
    // viewport_tx()/ty() are the VISIBLE window only. The emulated PPU VRAM stays
    // generous against the viewport (the nametable is 32 tiles wide regardless),
    // which is where the correctness margin for sub-tile scroll + lookahead lives --
    // NOT in these accessors. Any code reading viewport_tx() must not assume 32, and
    // any code reading viewport_ty() must not assume 30.
    /** @brief Viewport width in tiles (GBA: 30 = the 240px panel, < NES 32). */
    constexpr u16 viewport_tx() { return 30; }
    /** @brief Viewport height in tiles (GBA: 20 = the 160px panel, < NES 30). */
    constexpr u16 viewport_ty() { return 20; }
    /** @brief Viewport width in pixels (tiles * 8 = 240). */
    constexpr u16 viewport_px() { return viewport_tx() << 3; }
    /** @brief Viewport height in pixels (tiles * 8 = 160). */
    constexpr u16 viewport_py() { return viewport_ty() << 3; }
#elif defined(TARGET_NX) || defined(TARGET_WIIU)
    // The Switch and Wii U are 16:9 consoles, so unlike the 4:3 GX/3DS consoles
    // they do NOT pillarbox a 256-wide frame. Instead they widen the viewport
    // (the same "render more of the world horizontally" model as the SDL
    // LANDSCAPE desktop path), keeping the NES's 30-tile height. 52 tiles (416px)
    // x 30 (240px) is ~16:9; the height stays a multiple of the source so a clean
    // integer 3x fills 720p vertically, and the backend (src/switch/video.cpp,
    // src/wiiu/video.cpp) scales the width to fill the panel. 52 is a multiple of
    // 4 tiles, keeping the 32px attribute regions aligned (matching the SDL
    // path's `& ~3u`). VRAM is the same two pages (0x800) the SDL path uses for
    // any sub-512px render width.
    /** @brief Viewport width in tiles (Switch/Wii U: 52, widescreen). */
    constexpr u16 viewport_tx() { return 52; }
    /** @brief Viewport height in tiles (Switch/Wii U: 30). */
    constexpr u16 viewport_ty() { return 30; }
    /** @brief Viewport width in pixels (tiles * 8). */
    constexpr u16 viewport_px() { return viewport_tx() << 3; }
    /** @brief Viewport height in pixels (tiles * 8). */
    constexpr u16 viewport_py() { return viewport_ty() << 3; }
#elif defined(TARGET_PS2)
    // Unlike every other console backend here, the PS2's display hardware (the
    // GS) never limits this choice: it presents through a textured quad (gsKit,
    // see src/ps2/video.cpp) whose destination corners are arbitrary floats, so
    // scaling the rendered frame up to fill the TV costs the GPU nothing and
    // needs no "render more world" or "match the panel exactly" decision the
    // way GC/3DS/Switch/Wii U/PSP each had to make for their own fixed-function
    // display path. So this just stays at the NES's own native 32x30/256x240 --
    // the most period-accurate choice available, not a workaround for any
    // limit -- and src/ps2/video.cpp scales that quad to fill a 4:3 area of the
    // real screen, pillarboxed (matching the NES's own non-square pixel aspect
    // stretched to 4:3, same as most NES emulators' default display mode).
    /** @brief Viewport width in tiles (PS2: 32, NES-native -- the GS scales freely, so there is no reason to render more or less). */
    constexpr u16 viewport_tx() { return 32; }
    /** @brief Viewport height in tiles (PS2: 30, NES-native). */
    constexpr u16 viewport_ty() { return 30; }
    /** @brief Viewport width in pixels (tiles * 8 = 256). */
    constexpr u16 viewport_px() { return viewport_tx() << 3; }
    /** @brief Viewport height in pixels (tiles * 8 = 240). */
    constexpr u16 viewport_py() { return viewport_ty() << 3; }
#elif defined(TARGET_PS3)
    // Same conclusion as the PS2 branch above, reached a different way. The PS3
    // has no fixed panel either, but unlike the PS2 this backend does NOT get
    // free scaling from the display hardware: src/ps3/video.cpp blits into an
    // RSX-mapped buffer and flips it (no shaders, no textured quad -- see that
    // file's header for why a first pass skips the RSX 3D pipeline, the same
    // call the PSP and Switch backends each made for theirs), so whatever
    // scaling happens is the PPU's own work, pixel by pixel.
    //
    // That makes the NES-native 32x30 the *only* sensible viewport here, and
    // for a better reason than "it fits": 240 goes into 720p's 720 lines
    // exactly 3 times, so a 256x240 frame scales to 768x720 at an integer 3x
    // on both axes and every destination pixel is exactly one source pixel --
    // the same guarantee the PSP branch below spells out at 1x, which it calls
    // the only way to be sure there is no warping. The leftover 256px each side
    // of 1280 is pillarboxing, matching the NES's own 4:3 display aspect.
    /** @brief Viewport width in tiles (PS3: 32, NES-native -- scales to 768px at an exact 3x, see above). */
    constexpr u16 viewport_tx() { return 32; }
    /** @brief Viewport height in tiles (PS3: 30, NES-native -- 240 * 3 == 720p exactly). */
    constexpr u16 viewport_ty() { return 30; }
    /** @brief Viewport width in pixels (tiles * 8 = 256). */
    constexpr u16 viewport_px() { return viewport_tx() << 3; }
    /** @brief Viewport height in pixels (tiles * 8 = 240). */
    constexpr u16 viewport_py() { return viewport_ty() << 3; }
#elif defined(TARGET_PSP)
    // The PSP's panel is a fixed 480x272. Width matches it EXACTLY (480 is a
    // clean 60 tiles): unlike the Switch/Wii U branch above, this is not scaled
    // to fit, so the backend (src/psp/video.cpp) renders straight into VRAM at
    // native resolution with no horizontal scale step at all -- a non-integer
    // scale (the first cut of this backend tried stretching the 416x240
    // Switch/Wii U viewport up to 480x272) unavoidably duplicates a different,
    // uneven set of source rows/columns every frame, since neither axis is an
    // integer ratio between those two sizes; that is warping, not a clean
    // stretch, and every destination pixel being exactly one source pixel is
    // the only way to guarantee there is none.
    //
    // Height CANNOT similarly follow the panel to 272px (34 tiles): the shared
    // core's vertical walk (emu::GenerateFrame in src/emu/ppu.cpp, via
    // ::emu::ComputeNtGeometry) wraps at world_h = geo.gridH * viewport_py() --
    // gridH follows the linked mapper's own ::ppu::nametableCount and current
    // mirroring (2/1/1 tall for an ordinary vertically/horizontally-mirrored
    // board, 2 for four-screen), never taller than that grid, so a viewport
    // taller than what the grid actually provides walks back into row 0 of a
    // nametable partway down the screen, which reads as the image mirroring/
    // tearing near the bottom -- not a rendering bug, a request for pixels no
    // nametable in the grid has. This is exactly why every OTHER backend,
    // without exception, keeps its viewport at or under the NES's native 30
    // tiles; the PSP branch has to as well. The result is a
    // clean 480x240 render letterboxed inside 480x272 (16px black bars top and
    // bottom, filled once at init -- see src/psp/video.cpp) rather than either
    // scaling into distortion or rendering into a boundary the core doesn't
    // support crossing. 60 is a multiple of 4 tiles, keeping the 32px attribute
    // regions aligned (matching the SDL path's `& ~3u`).
    /** @brief Viewport width in tiles (PSP: 60, exact panel width). */
    constexpr u16 viewport_tx() { return 60; }
    /** @brief Viewport height in tiles (PSP: 30, NES-native -- panel is letterboxed, not scaled). */
    constexpr u16 viewport_ty() { return 30; }
    /** @brief Viewport width in pixels (tiles * 8 = 480). */
    constexpr u16 viewport_px() { return viewport_tx() << 3; }
    /** @brief Viewport height in pixels (tiles * 8 = 240, panel is 272 -- see viewport_ty()'s comment). */
    constexpr u16 viewport_py() { return viewport_ty() << 3; }
#elif defined(LANDSCAPE)
    /** @brief Viewport width in tiles — flexible on landscape, derived from window width (runtime). */
    constexpr u16 viewport_tx() { return ((mode->w / scale) >> 3) & ~3u; }
    /** @brief Viewport height in tiles — pinned to 30, matching the axis `scale` was chosen for. */
    constexpr u16 viewport_ty() { return 30; }
    /** @brief Viewport width in pixels (tiles * 8) — runtime, follows ::video::viewport_tx. */
    constexpr u16 viewport_px() { return viewport_tx() << 3; }
    /** @brief Viewport height in pixels (tiles * 8). */
    constexpr u16 viewport_py() { return viewport_ty() << 3; }
#else
    /** @brief Viewport width in tiles — pinned to 32, matching the axis `scale` was chosen for. */
    constexpr u16 viewport_tx() { return 32; }
    /** @brief Viewport height in tiles — flexible on portrait, derived from window height (runtime). */
    constexpr u16 viewport_ty() { return (mode->h / scale) >> 3; }
    /** @brief Viewport width in pixels (tiles * 8). */
    constexpr u16 viewport_px() { return viewport_tx() << 3; }
    /** @brief Viewport height in pixels (tiles * 8) — runtime, follows ::video::viewport_ty. */
    constexpr u16 viewport_py() { return viewport_ty() << 3; }
#endif

    /**
     * @brief One physical panel's rectangle inside the viewport, in pixels.
     *
     * ::viewport_px() x ::viewport_py() is the bounding box of every panel this
     * target presents (see each target's own viewport_tx()/viewport_ty() doc
     * comment above). On a single-screen target that box IS the panel. On the
     * dual-screen handhelds it is the union of two panels, and this struct is
     * the one place that says where each one sits inside it -- so no backend
     * re-derives the split, exactly as ::emu::ComputeNtGeometry is the one place
     * the nametable grid comes from.
     */
    struct PanelRect {
        u16 x;   ///< Panel's left edge within the viewport, in pixels.
        u16 y;   ///< Panel's top edge within the viewport, in pixels.
        u16 w;   ///< Panel width in pixels.
        u16 h;   ///< Panel height in pixels.
    };

#if defined(TARGET_NDS)
    /** @brief Panels the viewport spans (DS/DSi: 2, stacked vertically). */
    constexpr u8 panel_count() { return 2; }
    /**
     * @brief DS panel rects: two identical 256x192 screens on a shared
     *        horizontal edge, so panel 1 is simply panel 0 shifted down one
     *        panel height. No horizontal inset -- both are the full 256px.
     */
    constexpr PanelRect panel_rect(const u8 i) {
        return i == 0 ? PanelRect{0, 0, 256, 192}
                      : PanelRect{0, 192, 256, 192};
    }
#elif defined(TARGET_CTR)
    /** @brief Panels the viewport spans (3DS: 2, stacked vertically). */
    constexpr u8 panel_count() { return 2; }
    /**
     * @brief 3DS panel rects: a 400x240 top screen and a 320x240 bottom screen,
     *        the bottom one centred under the top. The viewport is 400 wide (the
     *        wider panel), so the bottom panel is inset (400-320)/2 == 40px --
     *        exactly 5 tiles -- on each side. Viewport columns 0..39 and 360..399
     *        therefore exist on the top panel only, and fall in the bezel beside
     *        the bottom one.
     */
    constexpr PanelRect panel_rect(const u8 i) {
        return i == 0 ? PanelRect{0, 0, 400, 240}
                      : PanelRect{40, 240, 320, 240};
    }
#else
    /** @brief Panels the viewport spans (single-screen targets: 1). */
    constexpr u8 panel_count() { return 1; }
    /** @brief The one panel, which is the whole viewport. */
    constexpr PanelRect panel_rect(u8) {
        return PanelRect{0, 0, viewport_px(), viewport_py()};
    }
#endif

    /** @brief ::panel_rect width in tiles, for the tile-space layout maths. */
    constexpr u16 panel_tx(const u8 i) { return static_cast<u16>(panel_rect(i).w >> 3); }
    /** @brief ::panel_rect height in tiles, for the tile-space layout maths. */
    constexpr u16 panel_ty(const u8 i) { return static_cast<u16>(panel_rect(i).h >> 3); }

#ifndef TARGET_NES
    /**
     * @brief How many nametables wide/tall the addressable background world
     *        grids as -- purely the linked mapper's ::ppu::nametableCount and
     *        the current runtime ::mirroring, exactly the alias real
     *        mirroring wiring gives for free: 2-wide/1-tall when ::mirroring
     *        is vertical (false), 1-wide/2-tall when horizontal (true), or
     *        always 2x2 for a 4-nametable board (::mirroring playing no part,
     *        matching a real four-screen board's own $A000 write being
     *        meaningless -- each quadrant already has fixed, dedicated
     *        storage).
     *
     * No viewport-width term here: unlike the old NES-fixed-32-wide-nametable
     * scheme (which needed extra nametable-sized slices just to cover a wider
     * viewport), each nametable already matches this viewport's own size
     * (::nametable_plane_bytes() below) -- one nametable already covers the
     * whole screen, exactly the way a real 2-nametable vertically-mirrored
     * NES board already gives a full screen of scroll-ahead lookahead without
     * needing a third nametable.
     */
    constexpr u16 nametable_grid_w() {
        return static_cast<u16>(ppu::nametableCount >= 4 ? 2 : (ppu::nametableCount >= 2 ? (mirroring ? 1 : 2) : 1));
    }
    constexpr u16 nametable_grid_h() {
        return static_cast<u16>(ppu::nametableCount >= 4 ? 2 : (ppu::nametableCount >= 2 ? (mirroring ? 2 : 1) : 1));
    }

    /**
     * @brief Bytes one physical nametable occupies: a tile plane (one byte
     *        per tile, ::viewport_tx() * ::viewport_ty() -- floored at the
     *        NES-native 32x30 minimum, and the WIDTH rounded up to a whole
     *        number of attribute bytes; see ::emu::ComputeNtGeometry's own doc
     *        comment, emu.hpp, for both) plus its attribute plane (one byte
     *        per 4x4-tile block).
     *
     * Must stay byte-for-byte in step with ::emu::ComputeNtGeometry: that is
     * the page layout every read and write actually uses, and this is what
     * sizes the allocation it indexes into.
     */
    constexpr unsigned nametable_plane_bytes() {
        const unsigned txMin = viewport_tx() < 32 ? 32u : viewport_tx();
        const unsigned tx = (txMin + 3u) & ~3u;
        const unsigned ty = viewport_ty() < 30 ? 30u : viewport_ty();
        const unsigned at_w = (tx + 3) / 4, at_h = (ty + 3) / 4;
        return tx * ty + at_w * at_h;
    }

    /** @brief Nametable VRAM size in bytes required for this run.
     *
     * ::nametable_grid_w() * ::nametable_grid_h() physical nametables, each
     * ::nametable_plane_bytes().
     */
    constexpr unsigned vram_bytes() {
        return static_cast<unsigned>(nametable_grid_w()) * nametable_grid_h() * nametable_plane_bytes();
    }
#endif
}


#ifndef TARGET_NES
/** @brief Desktop shadow of the PPU VRAM. */
extern u8* VideoRAM;
/** @brief Desktop shadow of the PPU palette RAM. */
extern u8* paletteRAM;
/** @brief Current horizontal scroll (pixels). */
extern u16 xScroll;
/** @brief Current vertical scroll (pixels). */
extern u16 yScroll;
/**
 * @brief Desktop shadow of MMC3's $A000 mirroring bit: false = vertical,
 *        true = horizontal. There's no hardware register to poke off-NES, so
 *        this is the library-visible state mapper code (e.g. ::mmc3::SetMirroring)
 *        writes directly, for the emu PPU (and anything else that needs to
 *        know the current mirroring) to read.
 */
extern bool mirroring;
#endif

#ifdef TARGET_NES
  /**
   * @brief Latches a ::scroll_t into the PPU scroll registers (NES).
   * @param s A ::scroll_t produced by ::ppu::CartesianToScroll.
   */
  #define WRITE_SCROLL(s) do { \
      (*(volatile u8*)PPUCTRL)   = (s).data[0]; \
      (*(volatile u8*)PPUSCROLL) = (s).data[1]; \
      (*(volatile u8*)PPUSCROLL) = (s).data[2]; \
  } while(0)
#else
  /** @brief Writes a ::scroll_t into the desktop scroll globals. */
  #define WRITE_SCROLL(s) do { xScroll = (s).x; yScroll = (s).y; } while(0)
#endif

namespace video {
    /**
     * @brief Blocks until the renderer has presented the current frame.
     *
     * On NES builds this waits for VBlank via ::PPUSTATUS; on desktop it
     * waits on the SDL3 present fence.
     */
    void WaitForPresent();
}

namespace ppu {
    /**
     * @brief Enables rendering by writing to ::PPUCTRL and ::PPUMASK.
     * @param ppuCtrl_ Value to latch into ::PPUCTRL (see ::ppu::ctrl flags).
     * @param ppuMask_ Value to latch into ::PPUMASK (see ::ppu::mask flags).
     */
    void EnableRendering(u8 ppuCtrl_, u8 ppuMask_);

    /**
     * @brief Converts a pixel position into the platform ::scroll_t representation.
     *
     * NES builds encode nametable select plus fine X/Y into the 3-byte
     * PPU format. Desktop builds just store the pixel coordinates.
     *
     * @param pos Pixel position.
     * @return   Encoded scroll value.
     */
    scroll_t CartesianToScroll(vec2<u16> pos);

    /**
     * @brief Sets the absolute scroll of the screen.
     * @param pos New scroll, in pixels.
     */
    void SetScroll(vec2<u16> pos);

    /**
     * @brief Adds a signed delta to the current scroll.
     * @param delta Signed delta, in pixels.
     */
    void DeltaScroll(vec2<i8> delta);

    /**
     * @brief Writes an array of bytes into nametable memory with a stride.
     *
     * Copies @p source byte-by-byte starting at the tile position
     * @p pos. @p polarity selects horizontal
     * (stride 1) or vertical (stride 32) writes, matching ::ppu::ctrl::POLARITY.
     *
     * @param pos      Tile position (pixels / 8).
     * @param source   Source buffer to push into PPU video RAM.
     * @param sBuffer  Size of @p source in bytes.
     * @param polarity Non-zero for vertical writes, zero for horizontal.
     */
    void WriteFromBufferToNameTable(vec2<u16> pos, const u8* source, u8 sBuffer, u8 polarity);

    /**
     * @brief Writes an array of bytes into nametable memory with a stride,
     *        at a precomputed address.
     *
     * Address overload of ::ppu::WriteFromBufferToNameTable -- see
     * ::ppu::WriteSingleToNameTable's own address overload for why this
     * exists (skips the (x,y)->address divide+modulo).
     *
     * @param address  Precomputed VRAM address (see ::ppu::CartesianToAddress).
     * @param source   Source buffer to push into PPU video RAM.
     * @param sBuffer  Size of @p source in bytes.
     * @param polarity Non-zero for vertical writes, zero for horizontal.
     */
    void WriteFromBufferToNameTable(u16 address, const u8* source, u8 sBuffer, u8 polarity);

    /**
     * @brief Writes the same byte value repeatedly into nametable memory.
     *
     * Same layout as ::ppu::WriteFromBufferToNameTable but pokes one fixed
     * @p value @p amt times instead of copying a source buffer -- for filling
     * a run of tiles (e.g. clearing a row) without needing a buffer or a
     * per-element callback.
     *
     * @param pos      Tile position (pixels / 8).
     * @param value    Byte value to repeat.
     * @param amt      Number of bytes to write.
     * @param polarity Non-zero for vertical writes, zero for horizontal.
     */
    void WriteRepeatedToNameTable(vec2<u16> pos, u8 value, u8 amt, u8 polarity);

    /**
     * @brief Writes the same byte value repeatedly into nametable memory,
     *        at a precomputed address.
     *
     * Address overload of ::ppu::WriteRepeatedToNameTable -- see
     * ::ppu::WriteSingleToNameTable's own address overload for why this exists.
     *
     * @param address  Precomputed VRAM address (see ::ppu::CartesianToAddress).
     * @param value    Byte value to repeat.
     * @param amt      Number of bytes to write.
     * @param polarity Non-zero for vertical writes, zero for horizontal.
     */
    void WriteRepeatedToNameTable(u16 address, u8 value, u8 amt, u8 polarity);

    /**
     * @brief Writes a single byte into nametable memory.
     * @param pos   Tile position.
     * @param value Byte value to write.
     */
    void WriteSingleToNameTable(vec2<u16> pos, u8 value);

    /**
     * @brief Writes a single byte into nametable memory at a precomputed address.
     *
     * Address overload of ::ppu::WriteSingleToNameTable. The (x,y)->address projection
     * is the costly part of the write (a divide+modulo by the 30-row nametable height);
     * this lets a caller pay it ONCE, off the hot path, via ::ppu::CartesianToAddress,
     * then replay the write -- e.g. inside the tight vblank window -- as three register
     * pokes with no arithmetic. @p address must be what ::ppu::CartesianToAddress returns
     * for the active backend (a \$2000-based PPU address on NES, a 0-based VRAM offset on
     * desktop).
     *
     * @param address Precomputed VRAM address (see ::ppu::CartesianToAddress).
     * @param value   Byte value to write.
     */
    void WriteSingleToNameTable(u16 address, u8 value);

    /**
     * @brief Writes bytes produced by a provider callback into nametable memory.
     *
     * Equivalent to ::ppu::WriteFromBufferToNameTable but sources each byte
     * from `fn(i)` instead of a preallocated buffer — useful for patterns and
     * procedurally generated rows.
     *
     * @p Idx is generic, so a `u8` provider binds without forcing the callback
     * signature up to `u16`. The body differs per target, so it is defined
     * out-of-line in each backend with explicit instantiations for `u8`/`u16`.
     *
     * @tparam Idx     Parameter type of @p fn (the value handed to the callback).
     * @param pos      Tile position.
     * @param fn       Provider returning the byte to write for iteration `i`.
     * @param amt      Number of iterations.
     * @param polarity Non-zero for vertical writes, zero for horizontal.
     */
    template <typename Idx>
    void WriteFromProviderToNameTable(vec2<u16> pos, u8 (*fn)(Idx), u8 amt, u8 polarity);

    /**
     * @brief Writes bytes produced by a provider callback into nametable
     *        memory, at a precomputed address.
     *
     * Address overload of ::ppu::WriteFromProviderToNameTable -- see
     * ::ppu::WriteSingleToNameTable's own address overload for why this exists.
     *
     * @tparam Idx     Parameter type of @p fn (the value handed to the callback).
     * @param address  Precomputed VRAM address (see ::ppu::CartesianToAddress).
     * @param fn       Provider returning the byte to write for iteration `i`.
     * @param amt      Number of iterations.
     * @param polarity Non-zero for vertical writes, zero for horizontal.
     */
    template <typename Idx>
    void WriteFromProviderToNameTable(u16 address, u8 (*fn)(Idx), u8 amt, u8 polarity);

    /**
     * @brief Writes an array of bytes into the attribute table with a stride.
     *
     * Same layout as ::ppu::WriteFromBufferToNameTable but targets attribute
     * memory instead of the nametable.
     *
     * @param pos      Tile position.
     * @param source   Source buffer of attribute bytes.
     * @param sBuffer  Size of @p source in bytes.
     * @param polarity Non-zero for vertical, zero for horizontal.
     */
    void WriteFromBufferToAttributeTable(vec2<u16> pos, const u8* source, u8 sBuffer, u8 polarity);

    /**
     * @brief Writes an array of bytes into the attribute table with a
     *        stride, at a precomputed address.
     *
     * Address overload of ::ppu::WriteFromBufferToAttributeTable -- see
     * ::ppu::WriteSingleToNameTable's own address overload for why this exists.
     *
     * @param address  Precomputed VRAM address (see ::ppu::CartesianToAddress).
     * @param source   Source buffer of attribute bytes.
     * @param sBuffer  Size of @p source in bytes.
     * @param polarity Non-zero for vertical, zero for horizontal.
     */
    void WriteFromBufferToAttributeTable(u16 address, const u8* source, u8 sBuffer, u8 polarity);

    /**
     * @brief Writes the same byte value repeatedly into attribute memory.
     *
     * Same layout as ::ppu::WriteFromBufferToAttributeTable but pokes one
     * fixed @p value @p amt times instead of copying a source buffer,
     * mirroring how ::ppu::WriteRepeatedToNameTable relates to
     * ::ppu::WriteFromBufferToNameTable.
     *
     * @param pos      Tile position.
     * @param value    Attribute byte to repeat.
     * @param amt      Number of bytes to write.
     * @param polarity Non-zero for vertical, zero for horizontal.
     */
    void WriteRepeatedToAttributeTable(vec2<u16> pos, u8 value, u8 amt, u8 polarity);

    /**
     * @brief Writes the same byte value repeatedly into attribute memory,
     *        at a precomputed address.
     *
     * Address overload of ::ppu::WriteRepeatedToAttributeTable -- see
     * ::ppu::WriteSingleToNameTable's own address overload for why this exists.
     *
     * @param address  Precomputed VRAM address (see ::ppu::CartesianToAddress).
     * @param value    Attribute byte to repeat.
     * @param amt      Number of bytes to write.
     * @param polarity Non-zero for vertical, zero for horizontal.
     */
    void WriteRepeatedToAttributeTable(u16 address, u8 value, u8 amt, u8 polarity);

    /**
     * @brief Writes a single byte into attribute memory.
     * @param pos   Tile position.
     * @param value Attribute byte (palette + flip flags).
     */
    void WriteSingleToAttributeTable(vec2<u16> pos, u8 value);

    /**
     * @brief Writes a single byte into attribute memory at a precomputed address.
     *
     * Address overload of ::ppu::WriteSingleToAttributeTable -- see
     * ::ppu::WriteSingleToNameTable's own address overload for why this exists.
     *
     * @param address Precomputed VRAM address (see ::ppu::CartesianToAddress).
     * @param value   Attribute byte (palette + flip flags).
     */
    void WriteSingleToAttributeTable(u16 address, u8 value);

    /**
     * @brief Writes bytes produced by a provider callback into attribute memory.
     *
     * Equivalent to ::ppu::WriteFromBufferToAttributeTable but sources each byte
     * from `fn(i)` instead of a preallocated buffer, mirroring how
     * ::ppu::WriteFromProviderToNameTable relates to ::ppu::WriteFromBufferToNameTable.
     *
     * @p Idx is generic, so a `u8` provider binds without forcing the callback
     * signature up to `u16`. The body differs per target, so it is defined
     * out-of-line in each backend with explicit instantiations for `u8`/`u16`.
     *
     * @tparam Idx     Parameter type of @p fn (the value handed to the callback).
     * @param pos      Tile position.
     * @param fn       Provider returning the attribute byte to write for iteration `i`.
     * @param amt      Number of iterations.
     * @param polarity Non-zero for vertical writes, zero for horizontal.
     */
    template <typename Idx>
    void WriteFromProviderToAttributeTable(vec2<u16> pos, u8 (*fn)(Idx), u8 amt, u8 polarity);

    /**
     * @brief Writes bytes produced by a provider callback into attribute
     *        memory, at a precomputed address.
     *
     * Address overload of ::ppu::WriteFromProviderToAttributeTable -- see
     * ::ppu::WriteSingleToNameTable's own address overload for why this exists.
     *
     * @tparam Idx     Parameter type of @p fn (the value handed to the callback).
     * @param address  Precomputed VRAM address (see ::ppu::CartesianToAddress).
     * @param fn       Provider returning the attribute byte to write for iteration `i`.
     * @param amt      Number of iterations.
     * @param polarity Non-zero for vertical, zero for horizontal.
     */
    template <typename Idx>
    void WriteFromProviderToAttributeTable(u16 address, u8 (*fn)(Idx), u8 amt, u8 polarity);

    /**
     * @brief Flushes pending nametable and attribute-table writes to the PPU.
     * @param nt Nametable index to flush.
     * @param at Attribute-table index to flush.
     */
    void Flush(u8 nt, u8 at);

    /**
     * @brief Sets the color emphasis bits in ::PPUMASK (bits 5-7).
     * @param priority OR of ::ppu::mask bits (::ppu::mask::RED, GREEN, BLUE).
     */
    void SetColorPriority(u8 priority);

    /** @brief Palette-RAM writes. */
    namespace pal {
        /**
         * @brief Writes an array of bytes into palette memory.
         * @param offset   Palette RAM offset to start writing at.
         * @param source   Source buffer of palette indices.
         * @param sBuffer  Size of @p source in bytes.
         */
        void WriteFromBuffer(u8 offset, const u8* source, u8 sBuffer);

        /**
         * @brief Writes a single byte into palette memory.
         * @param offset Palette RAM offset.
         * @param value  Palette index to store.
         */
        void WriteSingle(u8 offset, u8 value);
    }
}

namespace oam {
    /**
     * @brief Uploads an OAM buffer to the PPU via OAM DMA.
     *
     * Call once per frame, after the application has populated sprites in
     * @p oam. On NES this triggers the hardware OAM DMA from the buffer's
     * page (so @p oam must be 256-byte aligned); on desktop it freezes a
     * snapshot the renderer reads for the next frame.
     *
     * @param oam Pointer to the ::OAM_SPRITES-sprite buffer to upload.
     */
    void RefreshSprites(const sprite_t* oam);
}

namespace video {
#ifdef TARGET_NES
/** @brief Sprite-zero-hit handler — NES variant (parameterless). */
typedef void (*spriteZeroHandler_t)();
#else
/**
 * @brief Sprite-zero-hit handler — desktop variant.
 *
 * The desktop renderer needs to know where the sprite-zero test
 * should trip, so the handler is bundled with its trigger pixel.
 */
typedef struct {
  void (*method)(void); /**< Callback fired on sprite-zero hit. */
  vec2<u16> pos;   /**< Pixel position at which to fire. */
} spriteZeroHandler_t;

/**
 * @brief Registers a sprite-zero-hit handler (desktop only).
 * @param pos Pixel position at which to fire.
 * @param fn  Callback to invoke when the sprite-zero test trips.
 */
void SetSpriteZeroHandler(vec2<u16> pos, void (*fn)(void));
/** @brief Convenience wrapper around ::video::SetSpriteZeroHandler. */
#define SET_SPRITE_ZERO_HANDLER(px, py, fn) ::video::SetSpriteZeroHandler({px, py}, fn)
#endif

/**
 * @brief Spins until the beam crosses @p pos, then invokes @p fn.
 *
 * The NES implementation busy-waits on the sprite-zero hit flag in
 * ::PPUSTATUS; the desktop implementation schedules @p fn via the
 * renderer. In both cases @p latch is set to a non-zero value when
 * the handler has run, so frame code can detect completion.
 *
 * @param pos   Pixel trigger position.
 * @param fn    Callback to fire.
 * @param latch Flag written non-zero when @p fn has completed.
 */
void WaitThenReactToSpriteZero(vec2<u16> pos, void (*fn)(), atomic u8* latch);

/**
 * @brief As above, but accepts any invocable rather than a bare function
 *        pointer -- so the handler can carry a bank.
 *
 * A `void (*)()` is two bytes with nowhere to put a bank number. That is fine
 * when the target's bank is a compile-time constant -- a captureless lambda
 * converts to the plain overload above and the bank rides in the constant:
 *
 *     constexpr auto split = mmc3::GetCallable<BankedSplit>();
 *     video::WaitThenReactToSpriteZero(pos, []{ mmc3::Call(split); }, &done);
 *
 * It is not fine when the target is chosen at runtime. A capturing lambda
 * holding a runtime-selected callable cannot convert to a function pointer, so
 * it needs this overload:
 *
 *     video::WaitThenReactToSpriteZero(pos, [&]{ mmc3::Call(table[i]); }, &done);
 *
 * Nothing here names a mapper: which bank the handler lives in is the project's
 * business. Bank-switching inside is safe -- sprite-zero hit is a polled
 * PPUSTATUS flag, not an interrupt source, so @p fn runs in ordinary context.
 *
 * @note Off-NES @p fn must still convert to `void (*)()`: the handler is stored
 *       in a single pointer-shaped slot that cannot own a stateful callable,
 *       and there are no banks to carry. Resolve the choice before the call.
 */
template <typename Handler>
void WaitThenReactToSpriteZero(const vec2<u16> pos, Handler &&fn,
                               atomic u8* latch) {
#ifdef TARGET_NES
    (void)pos;
    // Same loop as the out-of-line NES definition in src/nes/video.cpp: clear
    // any stale hit left over from the previous frame's pre-render line, then
    // wait for the real one.
    while (!*latch) {
        while (  tech::peek(ppu::raw::PPUSTATUS) & 0x40)  { }
        while (!(tech::peek(ppu::raw::PPUSTATUS) & 0x40)) { }
        fn();
        *latch = true;
    }
#else
    void (*thunk)() = fn;   // captureless off-NES; see @note above
    WaitThenReactToSpriteZero(pos, thunk, latch);
#endif
}
} // namespace video

#ifdef TARGET_NES
namespace ppu {
/** @brief Write-through register+shadow for ::ppu::raw::PPUCTRL (see ::wo_register). */
extern tech::wo_register<ppu::raw::PPUCTRL>   PPUCTRL;
/** @brief Write-through register+shadow for ::ppu::raw::PPUMASK (see ::wo_register). */
extern tech::wo_register<ppu::raw::PPUMASK>   PPUMASK;
}   // namespace ppu

namespace oam {
/** @brief Write-through register+shadow for ::ppu::raw::OAMADDR (see ::wo_register). */
extern tech::wo_register<ppu::raw::OAMADDR>   OAMADDR;
/** @brief Write-through register+shadow for ::ppu::raw::OAMDMA (see ::wo_register). */
extern tech::wo_register<ppu::raw::OAMDMA>    OAMDMA;
}   // namespace oam

#else
namespace ppu {
/** @brief Desktop equivalent of the ::ppu::PPUCTRL register — a plain shadow byte the renderer reads. */
extern u8 PPUCTRL;
/** @brief Desktop equivalent of the ::ppu::PPUMASK register — a plain shadow byte the renderer reads. */
extern u8 PPUMASK;
}   // namespace ppu

/**
 * @brief Safe-VRAM-access block (desktop).
 *
 * No-op bracket that the desktop renderer synchronises internally;
 * source compatible with the NES ::VRAM macro.
 */
#define VRAM \
if (1)

#endif