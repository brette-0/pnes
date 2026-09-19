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
 * - **CHR ROM linkage.** ::CHARACTER_ROM places tile data in a
 *   platform-specific section so the linker lands it on the PPU's 8 KB
 *   boundary; ::CHARACTER_ROM_ALIGN forces that explicitly.
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
// all.) All are "non-NES", so
// the SDL3-only includes/globals are gated on
// (!TARGET_NES && !OGC && !CTR && !NX && !WIIU && !PSP && !NDS && !GBA).
#if !defined(TARGET_NES) && !defined(TARGET_OGC) && !defined(TARGET_CTR) && !defined(TARGET_NX) && !defined(TARGET_WIIU) && !defined(TARGET_PSP) && !defined(TARGET_NDS) && !defined(TARGET_GBA)
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
 *  Symbolic CHR tiles (#embed)                                             *
 *                                                                          *
 *  Unlike ::CHARACTER_ROM, which `.incbin`s a blob and exposes only its    *
 *  link-time start/end, these `#embed` each `.chr` file so its size is a   *
 *  compile-time constant. From that comes a `<name>_tile` constant -- the  *
 *  blob's base tile index, byte offset / 16 -- at zero runtime cost. Tile  *
 *  ids are never hand-assigned and work inside `constexpr` tables.         *
 *                                                                          *
 *  Authoring (one ordered list, e.g. in a `chr.hpp` shared header):        *
 *                                                                          *
 *      CHARACTER_ROM_BEGIN(chrSprite0)                                     *
 *      #embed "chr/sprites/sprite0.chr"                                   *
 *      CHARACTER_ROM_END(chrSprite0, CHR_ORIGIN);   // first: chains origin *
 *                                                                          *
 *      CHARACTER_ROM_BEGIN(chrBush)                                        *
 *      #embed "chr/tiles/bush.chr"                                         *
 *      CHARACTER_ROM_END(chrBush, chrSprite0);      // prev = line above   *
 *      ...                                                                 *
 *      CHARACTER_ROM_BEGIN(chrLast)                                        *
 *      #embed "chr/tiles/last.chr"                                         *
 *      CHARACTER_ROM_END_FINAL(chrLast, chrBush, 0x2000); // last: + image *
 *                                                                          *
 *  `#include` the list wherever you use tile ids. CHARACTER_ROM_END_FINAL  *
 *  emits the padded CHR image as an `inline` object, so the linker folds   *
 *  it to one copy however many TUs include the list. `#embed` cannot come  *
 *  from a macro, so that line is written literally between BEGIN/END.      *
 *                                                                          *
 *  Byte order (hence every `_tile`) follows the `prev` chain, not source   *
 *  position, so ids and placement can never disagree.                      *
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

namespace nes_chr {
  /** @brief Copy a `#embed` staging buffer into a sized, constexpr-friendly
   *  array of bytes. The source is `int` because the two compilers disagree on
   *  the element type a `#embed` expands to: GCC yields ints in [0,255], while
   *  Clang yields char-typed values (0xFF -> -1 on a signed-char target). No
   *  8-bit type holds that combined [-128,255] range without a braced-init
   *  narrowing error, so the staging buffer is `int`; the byte value is recovered
   *  here with an explicit cast and every downstream type (accum, the CHR image,
   *  the `_tile` ids) stays `u8`. The buffer is internal-linkage constexpr,
   *  consumed purely at compile time, so its width carries no runtime cost. */
  template <size_t N>
  constexpr std::array<u8, N> make(const int (&a)[N]) {
    std::array<u8, N> r{};
    for (size_t i = 0; i < N; ++i) r[i] = static_cast<u8>(a[i]);
    return r;
  }
  /** @brief Concatenate two byte blocks at compile time (order preserved). */
  template <size_t A, size_t B>
  constexpr std::array<u8, A + B> cat(const std::array<u8, A>& x,
                                      const std::array<u8, B>& y) {
    std::array<u8, A + B> r{};
    for (size_t i = 0; i < A; ++i) r[i]     = x[i];
    for (size_t i = 0; i < B; ++i) r[A + i] = y[i];
    return r;
  }
  /** @brief Right-pad a byte block with zeros to a fixed CHR-image size. */
  template <size_t Total, size_t N>
  constexpr std::array<u8, Total> pad(const std::array<u8, N>& s) {
    static_assert(N <= Total, "CHR tile data exceeds the CHR ROM image size");
    std::array<u8, Total> r{};
    for (size_t i = 0; i < N; ++i) r[i] = s[i];
    return r;
  }
}   // namespace nes_chr

/** @brief Chain anchor: the first blob's `prev`. Resolves to tile 0. */
constexpr u8 CHR_ORIGIN_tile   = 0;
constexpr u8 CHR_ORIGIN_ntiles = 0;
constexpr std::array<u8, 0> CHR_ORIGIN_accum{};

/** @brief CHR tiles per 4 KB PPU pattern table ($1000 bytes / 16). The boundary
 *         between the sprite table ($0000) and the background table ($1000). */
constexpr unsigned CHR_TILES_PER_TABLE = 256;

// The running placement-concat feeds the final padded image. It is built in
// every TU that includes the list, but is internal-linkage constexpr (consumed
// purely at compile time), so it never reaches any object file.
#define _CHR_PLACE(name, prev)                                         \
  constexpr auto name##_accum =                                        \
      nes_chr::cat(prev##_accum, nes_chr::make(name##_raw));

/** @brief Opens a `#embed` CHR blob named @p name (followed by a literal
 *         `#embed "path"` line, then ::CHARACTER_ROM_END). */
#define CHARACTER_ROM_BEGIN(name) constexpr int name##_raw[] = {

/** @brief Closes a `#embed` CHR blob and derives `<name>_tile` /
 *         `<name>_ntiles`. @p prev is the blob declared immediately before
 *         (or ::CHR_ORIGIN for the first), giving the byte/id ordering. */
#define CHARACTER_ROM_END(name, prev)                                  \
  };                                                                   \
  constexpr u8 name##_tile   = (u8)(prev##_tile + prev##_ntiles);      \
  constexpr u8 name##_ntiles =                                         \
      (u8)(sizeof(name##_raw) / sizeof(name##_raw[0]) / 16);           \
  _CHR_PLACE(name, prev)

/**
 * @brief Insert zero-fill into the CHR chain so the *next* blob begins at tile
 *        index @p to_tile -- e.g. align background graphics to the $1000
 *        pattern-table boundary while sprites stay in pattern table 0.
 *
 * The PPU uses two independent 4 KB pattern tables, selected by PPUCTRL. A blob
 * at tile 256 lives at $1000 and is addressed as PT-relative tile 0 -- exactly
 * what its `<name>_tile` holds, since these ids are `u8` and wrap at the table
 * boundary. So a pad keeps every downstream `_tile` correct for free.
 *
 * Defines the usual `_tile`/`_ntiles`/`_accum`, so the next blob names this pad
 * as its @p prev -- hence the otherwise-unread name. Prefer
 * ::CHARACTER_ROM_END_PAD_TO when the gap follows a blob you are closing; use
 * this when it does not:
 *
 *      CHARACTER_ROM_PAD_TO(chrBgGap, chrPrev, CHR_TILES_PER_TABLE);
 *      CHARACTER_ROM_BEGIN(chrBush)                       // first BG tile
 *      #embed "..."
 *      CHARACTER_ROM_END(chrBush, chrBgGap);
 *
 * @param name    Identifier for this padding link (any unused name).
 * @param prev    The blob declared immediately before.
 * @param to_tile Tile index the next blob should start at. Must be within one
 *                table (<= 256 tiles) of the current position.
 */
#define CHARACTER_ROM_PAD_TO(name, prev, to_tile)                      \
  constexpr u8  name##_tile      = (u8)(prev##_tile + prev##_ntiles);  \
  constexpr int name##_pad_tiles = (int)(to_tile)                      \
                                 - (int)(prev##_tile + prev##_ntiles); \
  static_assert(name##_pad_tiles >= 0,                                 \
      "CHARACTER_ROM_PAD: data before the pad already passes to_tile");\
  static_assert(name##_pad_tiles <= (int)CHR_TILES_PER_TABLE,          \
      "CHARACTER_ROM_PAD: gap exceeds one pattern table");             \
  constexpr u8  name##_ntiles    = (u8)name##_pad_tiles;               \
  constexpr auto name##_accum    =                                     \
      nes_chr::cat(prev##_accum,                                       \
                   std::array<u8, (size_t)name##_pad_tiles * 16>{});

/**
 * @brief Like ::CHARACTER_ROM_END, but rounds @p name's own end up to tile
 *        index @p to_tile with zero-fill instead of stopping where the
 *        `#embed`ed data ends.
 *
 * Use instead of ::CHARACTER_ROM_END plus a standalone ::CHARACTER_ROM_PAD_TO
 * when the gap follows the blob being closed -- the common case. The pad becomes
 * part of @p name, so the next blob names @p name as its `prev` like any other
 * link, with no throwaway identifier:
 *
 *      CHARACTER_ROM_END_PAD_TO(chrLastSprite, chrPrev,
 *                                CHR_TILES_PER_TABLE);   // last sprite
 *      CHARACTER_ROM_BEGIN(chrBush)                       // first BG tile
 *      #embed "..."
 *      CHARACTER_ROM_END(chrBush, chrLastSprite);
 *
 * @param name    This blob's identifier (as with ::CHARACTER_ROM_END).
 * @param prev    The blob declared immediately before (or ::CHR_ORIGIN).
 * @param to_tile Tile index @p name should end at (i.e. the next blob's
 *                start). Must be within one table (<= 256 tiles) of where
 *                the `#embed`ed data would otherwise end.
 */
#define CHARACTER_ROM_END_PAD_TO(name, prev, to_tile)                  \
  };                                                                    \
  constexpr u8  name##_tile       = (u8)(prev##_tile + prev##_ntiles); \
  constexpr u8  name##_raw_ntiles =                                    \
      (u8)(sizeof(name##_raw) / sizeof(name##_raw[0]) / 16);           \
  constexpr int name##_pad_tiles  = (int)(to_tile)                     \
                          - (int)(name##_tile + name##_raw_ntiles);    \
  static_assert(name##_pad_tiles >= 0,                                 \
      "CHARACTER_ROM_END_PAD_TO: blob already passes to_tile");        \
  static_assert(name##_pad_tiles <= (int)CHR_TILES_PER_TABLE,          \
      "CHARACTER_ROM_END_PAD_TO: gap exceeds one pattern table");      \
  constexpr u8  name##_ntiles     =                                    \
      (u8)(name##_raw_ntiles + name##_pad_tiles);                      \
  constexpr auto name##_accum     =                                    \
      nes_chr::cat(nes_chr::cat(prev##_accum, nes_chr::make(name##_raw)), \
                   std::array<u8, (size_t)name##_pad_tiles * 16>{});

/* Materialize the padded CHR ROM image from a blob's accumulation (the whole
 * chain) and place it in the CHR section. Internal helper for
 * ::CHARACTER_ROM_END_FINAL -- not for direct use.
 *
 * `inline` gives the image external linkage with COMDAT (linkonce_odr) folding:
 * any number of TUs may include the blob list, yet the linker keeps exactly one
 * copy of the byte-identical image -- so there is no dedicated emit TU and no
 * magic define. The object name is fixed (never supplied by the caller), so a
 * second CHARACTER_ROM_END_FINAL in the SAME TU is still a redefinition error,
 * enforcing "one cartridge, one CHR ROM image". (Across TUs the lists are the
 * same header, hence ODR-identical, so folding is well-defined.) */
#define _CHR_EMIT_IMAGE(name, total_bytes)                             \
  [[gnu::section(_CHR_SECTION), gnu::used, gnu::retain]]               \
  inline constexpr auto chr_rom_image = nes_chr::pad<total_bytes>(name##_accum); \
  [[gnu::used, gnu::retain]]                                          \
  inline constexpr unsigned ppu::chrRomBytes = (total_bytes)

/**
 * @brief Close the FINAL blob in the chain and, in the emitting TU, emit the
 *        whole cartridge's CHR ROM image.
 *
 * Use in place of ::CHARACTER_ROM_END for the last blob: does everything that
 * does, then materializes the padded CHR image from the whole chain's
 * accumulation into the CHR section.
 *
 * @p total_bytes is the ENTIRE CHR ROM, not the 8 KB the PPU sees at once -- the
 * mapper banks windows of it into $0000-$1FFF. No default; omitting it is a
 * preprocessor error.
 *
 * The image object is named internally, so a second CHARACTER_ROM_END_FINAL in
 * one TU is a redefinition error: one CHR image, enforced by the compiler.
 *
 * @param name        This (final) blob's identifier.
 * @param prev        The blob declared immediately before (or ::CHR_ORIGIN).
 * @param total_bytes Total CHR ROM size in bytes (required; no default).
 */
#define CHARACTER_ROM_END_FINAL(name, prev, total_bytes)             \
  CHARACTER_ROM_END(name, prev)                                      \
  _CHR_EMIT_IMAGE(name, total_bytes)

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
     * @brief Total bytes of the embedded flat CHR ROM image (the same
     *        @p total_bytes ::CHARACTER_ROM_END_FINAL pads it to) -- defined
     *        by that macro alongside the image itself, so it always matches
     *        exactly what's actually embedded, on every target.
     *
     * Declared unconditionally, since ::CHARACTER_ROM_END_FINAL is called on
     * every target and a macro cannot branch on TARGET_NES -- so the symbol
     * exists on NES too, simply unused. Off-NES tile-address translators wrap
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
    u16 CartesianToAddress(vec2<u16> pos);


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
#if !defined(TARGET_NES) && !defined(TARGET_OGC) && !defined(TARGET_CTR) && !defined(TARGET_NX) && !defined(TARGET_WIIU) && !defined(TARGET_PSP) && !defined(TARGET_NDS) && !defined(TARGET_GBA)
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
    // The 3DS top screen is a fixed 400x240 panel: exactly the NES's 240px
    // height (scale 1), but 144px (18 tiles) wider than the NES's 256px. Render
    // 50 tiles (400px) of actual game world at native 1:1 scale instead of
    // non-integer-stretching a fixed 256-wide image across 400px -- the same
    // "render more world" model as the other non-hardware backends. Known at
    // compile time because, unlike the GameCube/Wii TV, the 3DS panel
    // resolution never varies.
    /** @brief Viewport width in tiles (3DS: fixed 50, covers the 400px top screen). */
    constexpr u16 viewport_tx() { return 50; }
    /** @brief Viewport height in tiles (3DS: fixed 30, matching the NES). */
    constexpr u16 viewport_ty() { return 30; }
    /** @brief Viewport width in pixels (tiles * 8 = 400). */
    constexpr u16 viewport_px() { return viewport_tx() << 3; }
    /** @brief Viewport height in pixels (tiles * 8 = 240). */
    constexpr u16 viewport_py() { return viewport_ty() << 3; }
#elif defined(TARGET_NDS)
    // The Nintendo DS (and DSi) main engine is a fixed 256x192 panel -- the same
    // 32-tile NES width, but 6 tiles (48px) SHORTER than the NES's 240px height.
    // Per the project rule, the NES game is never compromised for another
    // platform: the DS simply shows a 256x192 WINDOW onto the same 256x240 world
    // the game renders. The backend (src/nds/video.cpp) maps the NES PPU onto the
    // DS's native 2D hardware (BG tilemap + hardware OBJ + per-scanline HBlank
    // scroll), so raster splits and sprite-0 still work; the bottom 48px of the
    // NES frame fall below the panel. This is the first target whose viewport is
    // shorter than the NES's 30 tiles, which is explicitly permitted: any code
    // reading video::viewport_ty() must not assume 30.
    /** @brief Viewport width in tiles (DS/DSi: fixed 32, full NES width). */
    constexpr u16 viewport_tx() { return 32; }
    /** @brief Viewport height in tiles (DS/DSi: 24 = the 192px panel, < NES 30). */
    constexpr u16 viewport_ty() { return 24; }
    /** @brief Viewport width in pixels (tiles * 8 = 256). */
    constexpr u16 viewport_px() { return viewport_tx() << 3; }
    /** @brief Viewport height in pixels (tiles * 8 = 192). */
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
     *        NES-native 32x30 minimum, see ::emu::ComputeNtGeometry's own doc
     *        comment, emu.hpp, for why) plus its attribute plane (one byte
     *        per 4x4-tile block).
     */
    constexpr unsigned nametable_plane_bytes() {
        const unsigned tx = viewport_tx() < 32 ? 32u : viewport_tx();
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