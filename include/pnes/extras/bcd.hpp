#pragma once

#include <pnes/extras/math.hpp>
#include <pnes/technology.hpp>
#include <intsh>
#include <type_traits>
#include <consh>

using namespace br0::intsh;
using namespace br0::consh;

// Tiles bcd<T>() writes, terminator excluded: one per digit, plus a
// sign column if T is signed. The field is fixed width and zero
// padded, so this is both the width to reserve on the nametable and
// the length to draw.
//
// CAPACITY: 2 * sizeof(T) digits, slightly less than T's range -- a
// u16 reaches 65535 but prints four digits, the fifth shifted out of
// the top. Size the type for the width you want.
template <nx T>
inline constexpr u8 bcd_tiles = static_cast<u8>((sizeof(std::make_unsigned_t<T>) << 1) + std::is_signed_v<T>);

/*
 *  @p num as bcd_tiles<T> charmapped tiles, most significant digit
 *  first, followed by the charmap's terminator -- a string in the
 *  sense the rest of the library means one, so it can go straight to
 *  ui::text::Make with no length. A signed T gets a leading '-' or
 *  ' ' column.
 *
 *  HEAP: the caller owns the buffer and delete[]s it, the same deal
 *  ui::text::Make's row array comes with. bcd_tiles<T> + 1 bytes.
 *
 *  @p Map is a template parameter, not an argument: a charmap is only
 *  ever constant-evaluated, and as a runtime object its operator()'s
 *  fall-through to the deliberately undefined nes_str::unmapped would
 *  become a real call. Its tiles are bound to constexpr locals for
 *  the same reason -- an unoptimised build may not fold a call left
 *  in a plain expression. Map's digit tiles must be contiguous from
 *  '0', and a signed T also needs it to map '-'.
 *
 *  NI: everything below is one body per <Map, T>, shared by every
 *  call site, rather than the conversion pasted in (and possibly
 *  unrolled, its trip count being a known 8/16/32/64) at each one.
 */
template <auto& Map, nx T>
[[nodiscard]] MINSIZE NI u8* bcd(const T num) {
    using store_t = std::make_unsigned_t<T>;

    constexpr auto BITS  = static_cast<u8>(sizeof(store_t) << 3);
    constexpr auto TOP   = static_cast<store_t>(1) << (BITS - 1);
    constexpr auto ONES  = static_cast<store_t>(~static_cast<store_t>(0)) / 0xf;  // 0x1111...
    constexpr auto THREE = static_cast<store_t>(ONES * 3);
    constexpr auto EIGHT = static_cast<store_t>(ONES * 8);

    constexpr u8 zero = Map('0');
    constexpr u8 term = Map('\0');

    /*
     *  Binary magnitude -> packed digits, two per byte. Double
     *  dabble, so no multiply or divide anywhere -- which is the
     *  whole reason the digits are reached this way and not by %10
     *  (the 6502 has neither). The per-nibble ">= 5" test is done for
     *  every digit at once: a nibble n is >= 5 exactly when n + 3
     *  sets its bit 3, which never carries out of the nibble while
     *  all digits are <= 9.
     */
    auto    v      = static_cast<store_t>(abs<T>(num));
    store_t packed = 0;

    // Leading zero bits dabble to nothing, so shift them out first:
    // one shift-and-test each instead of a full correct-and-shift. A
    // small value in a wide type is the normal case, and this is what
    // makes it cheap.
    u8 bits = BITS;
    while (bits && !(v & TOP)) {
        v = static_cast<store_t>(v << 1);
        --bits;
    }

    for (; bits; --bits) {
        const auto c = static_cast<store_t>(((packed + THREE) & EIGHT) >> 3);
        packed = static_cast<store_t>(packed + c + (c << 1));

        packed = static_cast<store_t>(packed << 1) | ((v & TOP) != 0);
        v = static_cast<store_t>(v << 1);
    }

    const auto out = new u8[bcd_tiles<T> + 1];
    u8*        w   = out;

    if constexpr (std::is_signed_v<T>) {
        constexpr u8 minus = Map('-');
        constexpr u8 space = Map(' ');

        // A magnitude that truncated away to nothing is not negative.
        *w++ = (num < 0 && packed) ? minus : space;
    }

    // Nibbles read straight out of packed through an unsigned char*
    // -- the one pun the aliasing rules allow both ways, and free
    // here: packed already IS its bytes, little end first on every
    // target, so raw[last] is the top two digits with no shifting.
    const auto raw = reinterpret_cast<const u8*>(&packed);
    for (u8 i = static_cast<u8>(sizeof(store_t)); i--; ) {
        *w++ = static_cast<u8>(zero + (raw[i] >> 4));
        *w++ = static_cast<u8>(zero + (raw[i] & 0x0F));
    }

    *w = term;
    return out;
}
