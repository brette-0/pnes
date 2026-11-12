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

    /**
     * @brief One region of character ROM, recognised at the CHR window it is
     *        banked in at: translates a tile's index in character ROM space
     *        into the index the PPU sees that same tile at.
     *
     * A tile has two numbers, and only one of them is a property of the tile.
     * Where it sits in the CHR ROM image is fixed by whatever packed it (for
     * this project, blobman -- see tools/blobman). Which index the PPU reads
     * it as depends entirely on the window the region holding it is banked
     * into, and so changes whenever the region is banked somewhere else. This
     * turns the first into the second, so the only tile numbers that need
     * naming anywhere are the packer's own, with the placement named once
     * here rather than folded into every constant.
     *
     * Deliberately knows nothing about any mapper. It holds no register, no
     * bank number and no notion of what is currently switched in -- a result
     * is the PPU index of that tile FOR AS LONG AS the region really is
     * banked at ::origin, which is the caller's business to arrange (on NES,
     * through whatever the linked mapper's own CHR switch is). Two regions
     * naming the same ::origin is the ordinary case, not a conflict: they are
     * alternatives for one window, and whichever was switched in last is the
     * one a translation is true of.
     *
     * Costs nothing in ROM, RAM or cycles: a window is a literal type, so an
     * instance that is @c constexpr (or a @c constexpr non-type template
     * argument -- it is a structural type, so it can be one) is never
     * materialised, and ::Get over a constant tile folds to the single byte
     * it returns. Both conditions are about how an instance is spelled at the
     * point of use: give a window automatic storage and hand its address to
     * something, and it becomes three real bytes like any other object.
     */
    class ChrWindow {
    public:
        /**
         * @brief Index, in character ROM space, of the first tile of the
         *        region this window describes -- the tile ::origin itself
         *        names. Subtracted back out by ::Get, so what reaches the PPU
         *        side is an offset into the region rather than an absolute
         *        position in the image.
         *
         * A tile index, not a byte offset: CHR tiles are 16 bytes, so a
         * region's byte position in the image divides down into this exactly,
         * and no sub-tile value is representable to be got wrong.
         */
        const u16 chr;

        /**
         * @brief Base index, in PPU space, the region is seen at -- the index
         *        of its first tile once banked in, and what ::Get counts
         *        forward from.
         *
         * One byte, because that is the whole of what the hardware reads: the
         * pattern table a fetch comes from is not part of a tile index at all
         * (::ppu::ctrl::BG_ADDR / ::ppu::ctrl::SPRITE_ADDR select it, and in
         * 8x16 mode ::oam::sprite_t::tile bit 0 does). PPU $0400 and $1400
         * are therefore the same @c origin of @c 0x40, differing only in a
         * control bit this class plays no part in.
         */
        const u8 origin;

        /**
         * @brief Describes a region packed at @p chrOrigin as banked in at
         *        @p ppuOrigin.
         *
         * @param chrOrigin Index of the region's first tile in character ROM
         *                  space; see ::chr.
         * @param ppuOrigin Index that tile is seen at once banked in; see
         *                  ::origin.
         */
        constexpr ChrWindow(const u16 chrOrigin, const u8 ppuOrigin)
            : chr{chrOrigin}, origin{ppuOrigin} {}

        /**
         * @brief The index @p tile is seen at by the PPU while this region is
         *        banked in at ::origin.
         *
         * @param tile A tile's index in character ROM space. Expected to be
         *             one of this region's own -- a tile from elsewhere in
         *             the image still translates arithmetically, to the index
         *             of whatever is actually that far into this window.
         * @return     That tile's PPU index.
         */
        constexpr u8 Get(const u16 tile) const {
            return static_cast<u8>(origin + (tile - chr));
        }

        /**
         * @brief Infix spelling of ::Get -- @c window+tile is that tile's PPU
         *        index, identical in every respect to @c window.Get(tile).
         *
         * Reads as the offsetting it is at a call site naming one tile out of
         * a region (@c ppu::WriteSingleToNameTable(addr, ui + blob::ui::font)),
         * where the method spelling buries the tile behind the call. Delegates
         * rather than repeating the arithmetic, so there is still one
         * definition of what a translation is.
         *
         * Note the asymmetry with ordinary addition this is NOT: the result is
         * a tile index in a different space from @p tile, not a window moved
         * along, so adding a window to a window, or a window to its own
         * result, means nothing and does not compile.
         *
         * @param tile A tile's index in character ROM space; see ::Get.
         * @return     That tile's PPU index.
         */
        constexpr u8 operator+(const u16 tile) const {
            return Get(tile);
        }
    };

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
     * @brief Platform-specific scroll encoding (NES: the four-write sequence,
     *        pre-packed).
     *
     * One byte per write of the $2006/$2005/$2005/$2006 sequence
     * ::ppu::SetScroll performs, in that order -- ::WRITE_SCROLL just stores
     * them. The point is that every shift and mask happens in
     * ::ppu::CartesianToScroll, ahead of the raster window, leaving the write
     * itself four unconditional stores.
     *
     * Layout:
     * - `data[0]` -> `$2006`: nametable select, `nt << 2`.
     * - `data[1]` -> `$2005`: Y, low byte (coarse + fine) after nametable wrap.
     * - `data[2]` -> `$2005`: X, low byte (coarse + fine).
     * - `data[3]` -> `$2006`: t's low byte, `(Y & 0xF8) << 2 | (X & 0xFF) >> 3`,
     *   whose write is what copies t into v and makes the scroll land now.
     *
     * @note Carries no ::PPUCTRL byte. `data[0]` used to be one, because the
     *       nametable select travelled via $2000; it goes through $2006 now.
     *       A caller that latches these directly is therefore NOT updating the
     *       PPUCTRL shadow's select bits the way ::ppu::SetScroll does, so a
     *       later PPUCTRL write can re-assert a stale select -- see that
     *       function (src/nes/video.cpp) for the mechanism.
     */
    typedef struct { u8 data[4]; } scroll_t;
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
 *         resolution AND display aspect are runtime-only, so this is computed
 *         once at init from the real TV pixel width (VIDEO_GetPreferredMode)
 *         and the display aspect (see ::video::GamecubeWidescreen) rather than
 *         being a compile-time constant. GC/Wii backend only. */
extern u16 ogc_world_tx;
#endif
#if defined(TARGET_WIIU)
/** @brief World columns visible this run (tiles). The Wii U's TV aspect is a
 *         system setting, not a fixed property of the console, so this is
 *         computed once at init from AVMGetTVAspectRatio rather than being a
 *         compile-time constant. Wii U backend only. */
extern u16 wiiu_world_tx;
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
    // The GameCube/Wii TV output resolution (NTSC/PAL, interlace/progressive)
    // AND its display aspect (4:3 or 16:9) are known only at runtime, unlike
    // the fixed 3DS panel below. Height stays pinned to the NES's 30
    // rows/240px; width is ogc_world_tx, computed once at init
    // (src/ogc/video.cpp) from the real TV pixel width and that aspect, so GX
    // renders more of the game world to fill a wider screen instead of
    // stretching a fixed 256px image -- the same "render more world" model the
    // SDL LANDSCAPE desktop path uses. A tiny/unusual TV mode simply crops the
    // camera (ogc_world_tx can resolve below 32); the underlying nametable
    // VRAM is sized separately to never go below the NES's own 2-page/
    // 0x800-byte minimum (see the InitMemory call in src/ogc/video.cpp).
    //
    // 16:9 does NOT change the framebuffer: GX still renders 640x480 (or
    // whatever the TV mode is) and the TV stretches it, i.e. anamorphic. What
    // changes is how many world columns that framebuffer has to cover before
    // the result reads as square pixels, which is why this is a viewport
    // question and not a VI one. A 4:3 480-line mode resolves to 40 columns
    // (320x240 world, square pixels at 4:3); the same mode at 16:9 resolves to
    // 52 (416x240, ~16:9), the same widescreen viewport TARGET_NX uses.
    /** @brief Viewport width in tiles (GC/Wii: runtime, covers the real TV width at its real aspect). */
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
#elif defined(TARGET_NX)
    // The Switch's own output is 16:9 and cannot be anything else -- docked
    // HDMI and the handheld panel are both 16:9, and SetSysTvSettings
    // (libnx, switch/services/set.h) carries resolution, RGB range and colour
    // settings but no aspect field, because there is nothing to choose. So
    // unlike the GX/3DS/Wii U paths this is a genuine compile-time constant and
    // nothing is probed.
    //
    // Rather than pillarbox a 256-wide frame it widens the viewport (the same
    // "render more of the world horizontally" model as the SDL LANDSCAPE
    // desktop path), keeping the NES's 30-tile height. 52 tiles (416px) x 30
    // (240px) is ~16:9; the height stays a multiple of the source so a clean
    // integer 3x fills 720p vertically, and the backend (src/switch/video.cpp)
    // scales the width to fill the panel. 52 is a multiple of 4 tiles, keeping
    // the 32px attribute regions aligned (matching the SDL path's `& ~3u`).
    // VRAM is the same two pages (0x800) the SDL path uses for any sub-512px
    // render width.
    /** @brief Viewport width in tiles (Switch: 52, widescreen -- fixed, the console has no 4:3 mode). */
    constexpr u16 viewport_tx() { return 52; }
    /** @brief Viewport height in tiles (Switch: 30). */
    constexpr u16 viewport_ty() { return 30; }
    /** @brief Viewport width in pixels (tiles * 8). */
    constexpr u16 viewport_px() { return viewport_tx() << 3; }
    /** @brief Viewport height in pixels (tiles * 8). */
    constexpr u16 viewport_py() { return viewport_ty() << 3; }
#elif defined(TARGET_WIIU)
    // Same widescreen model as TARGET_NX above (render more world horizontally
    // rather than pillarbox a 256-wide frame, NES-native 30-tile height), but
    // the Wii U's aspect is NOT fixed: it is a system setting, and a Wii U
    // wired to a 4:3 set is a configuration Cafe OS supports. Hardcoding the
    // 52-tile widescreen viewport there stretched every tile by 4/3.
    //
    // So width is wiiu_world_tx, read once at init (src/wiiu/video.cpp) from
    // AVMGetTVAspectRatio: 52 tiles (416x240, ~16:9) on a widescreen set, 40
    // (320x240, exactly 4:3) on a 4:3 one. Both are multiples of 4 tiles,
    // keeping the 32px attribute regions aligned (matching the SDL path's
    // `& ~3u`), and both stay at or under the 512px/2-page VRAM step.
    /** @brief Viewport width in tiles (Wii U: runtime, 52 widescreen or 40 at 4:3). */
    constexpr u16 viewport_tx() { return wiiu_world_tx; }
    /** @brief Viewport height in tiles (Wii U: 30). */
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
 * @brief GameCube/Wii-specific display controls.
 *
 * Declared on every target, so game code can call into it without guarding on
 * the platform; everything in here is an inline no-op away from the console it
 * belongs to.
 */
namespace ogc {
#if defined(TARGET_GC)
    /**
     * @brief Switches the GameCube between the 4:3 and 16:9 anamorphic
     *        viewports, live.
     *
     * Every GameCube build can do this -- it is a runtime switch, not a build
     * configuration -- and every build boots 4:3 until told otherwise, which is
     * the era-correct default and what an unmodified console on an unknown TV
     * should assume.
     *
     * It has to be a call the game makes, because the GameCube is the one
     * target here that cannot work the answer out for itself. The Wii holds the
     * TV screen format in SYSCONF and the backend reads it with
     * CONF_GetAspectRatio(); the Wii U has AVMGetTVAspectRatio(). The
     * GameCube's entire persistent config is the RTC-backed SRAM the IPL
     * writes -- language, video mode, mono/stereo, progressive scan, EuRGB60,
     * horizontal screen offset and the RTC counter bias (the `syssram` struct,
     * libogc's ogc/system.h) -- which has no aspect field, and there is no EDID
     * on an analog cable to probe either. That is exactly why GameCube games
     * each shipped their own widescreen toggle, and this is that toggle's
     * engine side: the game owns the setting (and its persistence, if any) and
     * tells the engine.
     *
     * Nothing about the video mode changes: VI still scans out the same
     * framebuffer and the TV still stretches it, i.e. ordinary anamorphic
     * widescreen. What changes is ::viewport_tx() -- how much of the game world
     * is rendered into that framebuffer, so the stretch lands back on square
     * pixels (40 columns at 4:3, 52 at 16:9, for the ordinary 480-line modes).
     *
     * @warning The viewport width IS the nametable page width
     *          (::nametable_plane_bytes, ::emu::ComputeNtGeometry), so changing
     *          it re-strides the whole plane and everything already in VRAM
     *          lands at the wrong offsets. A caller must therefore treat this
     *          like a scene change: switch with rendering disabled, then redraw
     *          the background from scratch and re-run the generated UI's
     *          `Make_*` calls so every anchor is re-derived against the new
     *          viewport. The allocation itself never moves -- irq::init()
     *          already sizes VRAM for the wider of the two -- so no pointer the
     *          game holds is invalidated. Calling it with the value already in
     *          effect does nothing at all.
     *
     * @param on True for the 16:9 anamorphic viewport, false for 4:3.
     */
    void SetWidescreen(bool on);
#else
    /**
     * @brief No-op away from the GameCube.
     *
     * The Wii, the Wii U and the desktop/LANDSCAPE path all read the real
     * display geometry themselves, so there is nothing for a game to set; on a
     * fixed-panel target there is nothing to set either. Present so a call site
     * shared with the GameCube build needs no `#ifdef`.
     */
    inline void SetWidescreen(bool) {}
#endif
}

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
   *
   * The four-write sequence with every value precomputed -- see ::scroll_t for
   * the byte order and ::ppu::SetScroll (src/nes/video.cpp) for why the order
   * is what it is. Reads `$2002` first to reset the write toggle, which this
   * sequence depends on: it is positional, and entering at `w == 1` makes the
   * leading `$2006` latch a bogus v immediately.
   *
   * @param s A ::scroll_t produced by ::ppu::CartesianToScroll.
   */
  #define WRITE_SCROLL(s) do { \
      (void)tech::peek(ppu::raw::PPUSTATUS); \
      tech::poke(ppu::raw::PPUADDR,   (s).data[0]); \
      tech::poke(ppu::raw::PPUSCROLL, (s).data[1]); \
      tech::poke(ppu::raw::PPUSCROLL, (s).data[2]); \
      tech::poke(ppu::raw::PPUADDR,   (s).data[3]); \
  } while(0)
#else
  /**
   * @brief Latches a ::scroll_t off-NES.
   *
   * ::scroll_t is a plain pixel pair on these targets, so this is just
   * ::ppu::SetScroll -- the emulated PPU's scroll is internal to that core
   * (::emu::scrollX / ::emu::scrollY, src/emu/emu.hpp) rather than a pair of
   * public globals, so there is nothing here to poke directly. Routing through
   * the setter is also what keeps ::emu::scrollWritten correct, which the
   * emu's mid-frame Y re-anchor depends on.
   */
  #define WRITE_SCROLL(s) do { ppu::SetScroll(s); } while(0)
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
     * NES builds pack the four-write sequence (see ::scroll_t). Desktop builds
     * just store the pixel coordinates. Call this off the raster-critical path
     * -- the whole point is that ::ppu::StageScroll / ::ppu::LatchScroll then
     * do no arithmetic.
     *
     * @param pos Pixel position.
     * @return   Encoded scroll value.
     */
    scroll_t CartesianToScroll(vec2<u16> pos);

    /**
     * @brief Sets the absolute scroll of the screen.
     *
     * Performs the whole sequence back to back. For a raster split that has to
     * land on an exact dot, prefer ::ppu::StageScroll + ::ppu::LatchScroll,
     * which let the delay sit between the two halves.
     *
     * @param pos New scroll, in pixels.
     */
    void SetScroll(vec2<u16> pos);

    /**
     * @brief Loads the PPU's `t` register from a precomputed ::scroll_t,
     *        without moving the screen.
     *
     * The first three writes of the four-write sequence ($2006/$2005/$2005),
     * plus the `$2002` read that resets the write toggle. These only stage
     * `t`, and the PPU renders from `v`, never from `t` -- so NOTHING here is
     * visible and the timing of this call does not matter. That is the whole
     * reason the pair exists: the arithmetic and three of the four writes get
     * to happen early, and only ::ppu::LatchScroll has to hit the dot.
     *
     * Also keeps the ::PPUCTRL shadow's nametable-select bits in step, derived
     * from the staged value, for the reason given in ::ppu::SetScroll
     * (src/nes/video.cpp): a later PPUCTRL write would otherwise re-assert a
     * stale select.
     *
     * Off-NES there is no `t`, so this does nothing at all and
     * ::ppu::LatchScroll performs the whole move.
     *
     * @param s A ::scroll_t produced by ::ppu::CartesianToScroll.
     */
    void StageScroll(scroll_t s);

    /**
     * @brief Commits a staged ::scroll_t to the screen: `v = t`.
     *
     * The fourth and final write ($2006). This is the raster-critical one --
     * the scroll changes at the exact dot this store lands on -- and it is a
     * single store of a precomputed byte, so the critical window is as narrow
     * as the 6502 allows. Put the split's cycle delay BEFORE this call and
     * after ::ppu::StageScroll.
     *
     * Must be paired with a ::ppu::StageScroll of the same value, which is
     * what left the other three quarters of the scroll in `t`; on its own this
     * commits whatever `t` happens to hold.
     *
     * @param s The same ::scroll_t that was staged.
     */
    void LatchScroll(scroll_t s);

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