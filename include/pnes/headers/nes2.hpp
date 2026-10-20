/**
 * @file header.hpp
 * @brief Emits the 16-byte NES2.0 ROM header (https://www.nesdev.org/wiki/NES_2.0).
 */
#pragma once

#include <intsh>
using namespace br0::intsh;

#ifdef TARGET_NES

// Alternative nametable wiring (NES2.0 flags 6, bit 3 -- the "four-screen"
// bit): set by the board's own CMake config (ALTERNATIVE_NAMETABLE in
// local.cmake), not by the HEADER() call site -- it's a fact about which
// board is being linked, same as MAPPER/SUBMAPPER below. Only its
// non-zero-ness matters here; the value itself is board glue code's own
// business, not something the header format records.
#ifndef ALTERNATIVE_NAMETABLE
#define ALTERNATIVE_NAMETABLE 0
#endif

/**
 * @brief Places the 16 raw NES2.0 header bytes at the front of the ROM file.
 *
 * Call exactly once, in exactly one TU, with the 16 header bytes in file
 * order (identifier x4, PRG-ROM size, CHR-ROM size, flags 6-15 per the
 * NES2.0 spec). The bytes land in the `.nes2_header` section, which the
 * mapper's linker script (e.g. vrc1.ld) places via its `nes2_header` MEMORY
 * region ahead of PRG-ROM/CHR-ROM, replacing the old `INCLUDE
 * ines-header.ld` mechanism.
 *
 * This is the low-level primitive: 16 unchecked positional bytes the caller
 * has hand-computed. Prefer ::HEADER, which computes every byte from named
 * fields with static_asserts on their ranges.
 */
#define NES2_HEADER(b0, b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11, b12, b13, b14, b15) \
    extern "C" [[gnu::section(".nes2_header"), gnu::used, gnu::retain]]                  \
    constexpr u8 _nes2_header[16] = {                                                    \
        (b0), (b1), (b2), (b3), (b4), (b5), (b6), (b7),                                  \
        (b8), (b9), (b10), (b11), (b12), (b13), (b14), (b15)                             \
    }

/** @brief Hard-wired nametable mirroring (NES2.0 flags 6, bit 0). */
enum class Mirroring : u8 {
    Horizontal = 0,
    /// Alias for Horizontal (same bit): the mapper drives nametables at
    /// runtime (MMC1, MMC3, ...), so this bit is a don't-care default, not
    /// a claim about physical wiring. Spelled out at the call site for
    /// readability only.
    Mapper     = 0,
    Vertical   = 1,
};

/** @brief CPU/PPU timing region (NES2.0 byte 12, bits 0-1). */
enum class Timing : u8 {
    NTSC  = 0,
    PAL   = 1,
    MULTI = 2,
    DENDY = 3,
};

namespace nes2 {
    /**
     * @brief Splits @p bytes into a power-of-two exponent and odd remainder.
     * @param bytes Value to factor.
     * @param e     Set to the number of trailing zero bits of @p bytes.
     * @return The odd part of @p bytes (bytes >> e).
     */
    constexpr unsigned long long odd_part(unsigned long long bytes, unsigned &e) {
        e = 0;
        while (bytes != 0 && (bytes & 1) == 0) { bytes >>= 1; ++e; }
        return bytes;
    }

    /**
     * @brief True if @p bytes is exactly representable as NES2.0's
     *        exponent-multiplier form, 2^E * (2M + 1) with M in 0..3.
     *
     * Every power-of-two byte count qualifies (odd part 1, M 0). @p bytes == 0
     * is also representable (as the plain-notation zero; the exponent form
     * itself can't spell zero).
     */
    constexpr bool exponent_representable(unsigned long long bytes) {
        if (bytes == 0) return true;
        unsigned e;
        return odd_part(bytes, e) <= 7;
    }

    /** @brief NES2.0 exponent-multiplier byte for a PRG-ROM/CHR-ROM size (header byte 4 / 5). */
    constexpr u8 exponent_byte(unsigned long long bytes) {
        if (bytes == 0) return 0;
        unsigned e;
        const auto odd = odd_part(bytes, e);
        return static_cast<u8>((e << 2) | ((odd - 1) / 2));
    }

    /**
     * @brief True if @p bytes is exactly representable in NES2.0's plain
     *        (linear) notation: a multiple of @p unit whose quotient fits in
     *        the 12-bit ($000-$EFF) size field -- $F is reserved to select
     *        the exponent-multiplier form, so $EFF is the largest linear
     *        quotient.
     * @param unit 16384 for PRG-ROM, 8192 for CHR-ROM.
     */
    constexpr bool linear_representable(unsigned long long bytes, unsigned long long unit) {
        return bytes % unit == 0 && (bytes / unit) <= 0xEFF;
    }

    /**
     * @brief True if @p bytes (a PRG-ROM/CHR-ROM size) can be represented at
     *        all -- via the linear notation or, failing that, the
     *        exponent-multiplier form. The spec permits the exponent form
     *        only when the linear one can't express the size, so callers
     *        should prefer linear whenever both apply (see ::rom_size_byte /
     *        ::rom_msb_nibble).
     * @param unit 16384 for PRG-ROM, 8192 for CHR-ROM.
     */
    constexpr bool rom_size_representable(unsigned long long bytes, unsigned long long unit) {
        return linear_representable(bytes, unit) || exponent_representable(bytes);
    }

    /**
     * @brief Header byte 4 / 5 for a PRG-ROM/CHR-ROM size: the low 8 bits of
     *        the linear unit count when the linear notation applies (see
     *        ::linear_representable), otherwise the exponent-multiplier byte.
     * @param unit 16384 for PRG-ROM, 8192 for CHR-ROM.
     */
    constexpr u8 rom_size_byte(unsigned long long bytes, unsigned long long unit) {
        if (bytes == 0) return 0;
        if (linear_representable(bytes, unit)) {
            return static_cast<u8>((bytes / unit) & 0xFF);
        }
        return exponent_byte(bytes);
    }

    /**
     * @brief MSB nibble for header byte 9: the high 4 bits of the linear unit
     *        count ($0-$E) when the linear notation applies; 0xF selects the
     *        exponent-multiplier form (byte 4/5 holds E/M instead); 0x0 is
     *        the plain-notation zero used when the ROM is absent.
     * @param unit 16384 for PRG-ROM, 8192 for CHR-ROM.
     */
    constexpr u8 rom_msb_nibble(unsigned long long bytes, unsigned long long unit) {
        if (bytes == 0) return 0x0;
        if (linear_representable(bytes, unit)) {
            return static_cast<u8>((bytes / unit >> 8) & 0xF);
        }
        return 0xF;
    }

    /**
     * @brief True if @p bytes is 0 (absent) or exactly 64 << n for some n in 1..15
     *        -- the shift-count form shared by PRG-RAM, PRG-NVRAM/EEPROM,
     *        CHR-RAM and CHR-NVRAM (header bytes 10-11).
     */
    constexpr bool ram_size_representable(unsigned long long bytes) {
        if (bytes == 0) return true;
        unsigned long long v = 128;
        unsigned n = 1;
        while (v < bytes && n < 15) { v <<= 1; ++n; }
        return v == bytes;
    }

    /** @brief Shift count for a PRG-RAM/PRG-NVRAM/CHR-RAM/CHR-NVRAM size (header bytes 10-11). */
    constexpr u8 ram_shift(unsigned long long bytes) {
        if (bytes == 0) return 0;
        unsigned long long v = 128;
        unsigned n = 1;
        while (v < bytes) { v <<= 1; ++n; }
        return static_cast<u8>(n);
    }
} // namespace nes2

/**
 * @brief Builds and places the 16-byte NES2.0 header from named fields.
 *
 * Console type is always NES/Famicom, trainer is never present, and Vs.
 * System/Extended Console Type, Miscellaneous ROMs, and Default Expansion
 * Device are always zero -- none of these are supported yet, so none are
 * parameters here.
 *
 * Mapper, submapper and four-screen wiring (flags 6, bit 3) come from the
 * ::MAPPER / ::SUBMAPPER / ::ALTERNATIVE_NAMETABLE compiler defines rather than
 * arguments: each is a compile-time fact about the board being built, not
 * something a call site should restate. Only ALTERNATIVE_NAMETABLE's
 * non-zero-ness is used.
 *
 * @param mirroring       Hard-wired nametable mirroring (::Mirroring).
 * @param battery         Battery-backed PRG-RAM/NVRAM present.
 * @param prg_rom_bytes   PRG-ROM size, in bytes.
 * @param chr_rom_bytes   CHR-ROM size, in bytes (0 for CHR-RAM-only carts).
 * @param prg_ram_bytes   Volatile PRG-RAM size, in bytes (0 = absent).
 * @param prg_nvram_bytes Non-volatile PRG-RAM/EEPROM size, in bytes (0 = absent).
 * @param chr_ram_bytes   Volatile CHR-RAM size, in bytes (0 = absent).
 * @param chr_nvram_bytes Non-volatile CHR-RAM size, in bytes (0 = absent).
 * @param timing          CPU/PPU timing region (::Timing).
 */
#define HEADER(mirroring, battery,                                                               \
               prg_rom_bytes, chr_rom_bytes,                                                     \
               prg_ram_bytes, prg_nvram_bytes,                                                   \
               chr_ram_bytes, chr_nvram_bytes,                                                   \
               timing)                                                                           \
    static_assert(::nes2::rom_size_representable(prg_rom_bytes, 16384),                          \
        "PRG-ROM size is not representable in NES2.0's header: it must be a "                    \
        "multiple of 16 KiB no larger than 0xEFF units, or (2^E)*(1,3,5 or 7) bytes");            \
    static_assert(::nes2::rom_size_representable(chr_rom_bytes, 8192),                           \
        "CHR-ROM size is not representable in NES2.0's header: it must be a "                    \
        "multiple of 8 KiB no larger than 0xEFF units, or (2^E)*(1,3,5 or 7) bytes");             \
    static_assert(::nes2::ram_size_representable(prg_ram_bytes),                                 \
        "PRG-RAM size must be 0 (absent) or 64 << n bytes, n in 1..15 (128 B .. 2 MiB)");         \
    static_assert(::nes2::ram_size_representable(prg_nvram_bytes),                                \
        "PRG-NVRAM/EEPROM size must be 0 (absent) or 64 << n bytes, n in 1..15");                 \
    static_assert(::nes2::ram_size_representable(chr_ram_bytes),                                 \
        "CHR-RAM size must be 0 (absent) or 64 << n bytes, n in 1..15");                          \
    static_assert(::nes2::ram_size_representable(chr_nvram_bytes),                               \
        "CHR-NVRAM size must be 0 (absent) or 64 << n bytes, n in 1..15");                        \
    static_assert(MAPPER <= 0xFFF, "MAPPER exceeds NES2.0's 12-bit mapper number field");         \
    static_assert(SUBMAPPER <= 0xF, "SUBMAPPER exceeds NES2.0's 4-bit submapper field");          \
    NES2_HEADER(                                                                                 \
        'N', 'E', 'S', 0x1a,                                                                     \
        ::nes2::rom_size_byte(prg_rom_bytes, 16384),                                              \
        ::nes2::rom_size_byte(chr_rom_bytes, 8192),                                                \
        (static_cast<u8>(mirroring) | (static_cast<u8>(battery) << 1) |                          \
            (static_cast<u8>((ALTERNATIVE_NAMETABLE) != 0) << 3) | ((MAPPER & 0xF) << 4)),       \
        (0b1000 | (((MAPPER >> 4) & 0xF) << 4)),                                                 \
        (((MAPPER >> 8) & 0xF) | (SUBMAPPER << 4)),                                               \
        ((::nes2::rom_msb_nibble(chr_rom_bytes, 8192) << 4) |                                     \
            ::nes2::rom_msb_nibble(prg_rom_bytes, 16384)),                                        \
        ((::nes2::ram_shift(prg_nvram_bytes) << 4) | ::nes2::ram_shift(prg_ram_bytes)),           \
        ((::nes2::ram_shift(chr_nvram_bytes) << 4) | ::nes2::ram_shift(chr_ram_bytes)),           \
        static_cast<u8>(timing),                                                                 \
        0, 0, 0                                                                                  \
    )

#else
/** @brief Off-NES builds produce no ROM file, hence no header to emit. */
#define NES2_HEADER(...)
/** @brief Off-NES builds produce no ROM file, hence no header to emit. */
#define HEADER(...)
#endif
