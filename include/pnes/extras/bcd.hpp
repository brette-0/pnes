#pragma once

#include <pnes/extras/math.hpp>
#include <intsh>
#include <type_traits>
#include <consh>

#include <pnes/video.hpp>
using namespace br0::intsh;
using namespace br0::consh;

/*
 *  Packed BCD held in the number's OWN type: two decimal digits per
 *  byte, so bcd<T> carries 2 * sizeof(T) digits -- bcd<u16> is a u16
 *  of four digits, bcd<u32> a u32 of eight. Nothing is heap allocated
 *  and nothing is stored as tiles, so the value stays arithmetic:
 *  later operations (BCD add/sub/compare, increment a score by 50)
 *  work on Raw() directly instead of on a char array.
 *
 *  CAPACITY: 2 * sizeof(T) digits is slightly less than T's full
 *  range (a u16 reaches 65535, five digits, but holds four). Make()
 *  truncates from the top -- digits above the capacity are shifted
 *  out, as Max() reports. Size the type for the display width you
 *  want: a six-digit score is a bcd<u32>, not a bcd<u16>.
 *
 *  Conversion is double dabble: shift-and-add-3, so no multiply or
 *  divide anywhere (the 6502 has neither). The per-nibble ">= 5"
 *  test is done for every digit at once -- a nibble n is >= 5 exactly
 *  when n + 3 sets its bit 3, which never carries out of the nibble
 *  while all digits are <= 9.
 */
template <nx T>
class bcd {
public:
    using store_t = std::make_unsigned_t<T>;

    static constexpr u8 BYTES  = sizeof(store_t);
    static constexpr u8 DIGITS = BYTES << 1;
    // Tiles Draw() pokes: one per digit, plus a sign column if T is signed.
    static constexpr u8 TILES  = DIGITS + std::is_signed_v<T>;

    // Largest value Make() can hold without dropping digits -- 10^DIGITS
    // - 1, as a plain number (the x10 is folded at compile time, so no
    // multiply reaches the 6502).
    static constexpr store_t Max() {
        store_t m = 0;
        for (u8 i = 0; i < DIGITS; ++i) m = static_cast<store_t>(m * 10 + 9);
        return m;
    }

    /*
     *  Binary magnitude -> packed digits, the double dabble itself.
     *
     *  Deliberately NOT MINSIZE, unlike Make(): an out-of-line body
     *  can take no constant from its caller, and that alone turned
     *  `lives += 1` into a jsr that converted the literal 1 at run
     *  time, at int width. Left inlinable, a compile-time operand
     *  folds to its digits here and nothing but the add survives --
     *  `lives += 1` is now 30 instructions with no call in them.
     */
    AI static constexpr store_t DabbleHere(store_t v) {
        store_t out = 0;

        // Leading zero bits dabble to nothing, so shift them out first:
        // one shift-and-test each instead of a full correct-and-shift.
        // This is what keeps `wide += small` affordable -- see Digits().
        u8 i = BITS;
        while (i && !(v & TOP)) {
            v = static_cast<store_t>(v << 1);
            --i;
        }

        for (; i; --i) {
            // Every nibble >= 5 gets a 1 in its low bit, then += 3 * that.
            const auto c = static_cast<store_t>(((out + THREE) & EIGHT) >> 3);
            out = static_cast<store_t>(out + c + (c << 1));

            out = static_cast<store_t>(out << 1) | ((v & TOP) != 0);
            v = static_cast<store_t>(v << 1);
        }

        return out;
    }

    /*
     *  The same conversion as one shared out-of-line copy, for operands
     *  whose value only exists at runtime.
     *
     *  NI pins what -Os already chooses here: exactly one dabble body
     *  per width, reused by every call site, rather than a loop pasted
     *  in (and possibly unrolled, its trip count being a known 8/16/32/
     *  64) wherever a bcd is added to. Nothing is lost by forbidding
     *  the inline, since the call sites that WANT the loop inlined are
     *  the constant ones, and those go through DabbleHere instead.
     */
    MINSIZE NI static constexpr store_t Dabble(const store_t v) { return DabbleHere(v); }

    MINSIZE constexpr void Make(const T num) {
        packed = Dabble(abs<T>(num));

        // A magnitude that truncated away to nothing is not negative.
        if constexpr (std::is_signed_v<T>) neg = num < 0 && packed;
    }

    /*
     *  Writes TILES tiles, most significant digit first, from addr
     *  outward (stride 1 horizontally, 32 vertically -- same polarity
     *  convention as ppu::WriteFromBufferToNameTable). CharMap's digit
     *  tiles must be contiguous from '0'. Leading digits are not
     *  suppressed: the field is fixed width, zero padded.
     *
     *  The nibbles are read straight out of packed through an
     *  unsigned char* -- the only pun the aliasing rules allow both
     *  ways, and free here: packed already IS BYTES little-endian
     *  bytes in memory, so raw[BYTES - 1] is the top two digits with
     *  no shifting and no copy.
     */
    template <typename CharMap>
    MINSIZE void Draw(u16 addr, const u8 polarity = 0) const {
        const u8 stride = polarity ? 32 : 1;
        const auto* raw = reinterpret_cast<const u8*>(&packed);

        if constexpr (std::is_signed_v<T>) {
            ppu::WriteSingleToNameTable(addr, neg ? CharMap('-') : CharMap(' '));
            addr += stride;
        }

        for (u8 i = BYTES; i--; ) {
            ppu::WriteSingleToNameTable(addr, CharMap('0') + (raw[i] >> 4));
            addr += stride;
            ppu::WriteSingleToNameTable(addr, CharMap('0') + (raw[i] & 0x0F));
            addr += stride;
        }
    }

    /*
     *  a + b over packed digits, every nibble at once and with no
     *  decimal mode (the 2A03 has none): bias each nibble by 6 so a
     *  decimal carry becomes a binary one, add, then take the 6 back
     *  off every nibble that did NOT carry. The carry into bit 4k+4 is
     *  a ^ b ^ sum there, and the top nibble's carry leaves the word
     *  entirely, so it is read off the wraparound instead.
     *
     *  Overflows modulo 10^DIGITS, exactly as Make() truncates.
     */
    static constexpr store_t AddPacked(const store_t a, const store_t b) {
        const auto a6 = static_cast<store_t>(a + SIX);
        const auto t  = static_cast<store_t>(a6 + b);

        auto fix = static_cast<store_t>((~(a6 ^ b ^ t) & CARRY) >> 4);
        if (t >= a6) fix |= TOPDIG;             // top nibble kept its bias too

        return static_cast<store_t>(t - (fix << 2) - (fix << 1));   // -6 each
    }

    /*
     *  a - b over packed digits. A binary borrow lends 16 where a
     *  decimal one lends 10, so every nibble that borrowed comes out 6
     *  too high; the borrow bits sit at the same places AddPacked's
     *  carries do. Underflows to the ten's complement (mod 10^DIGITS).
     */
    static constexpr store_t SubPacked(const store_t a, const store_t b) {
        const auto t = static_cast<store_t>(a - b);

        auto fix = static_cast<store_t>(((a ^ b ^ t) & CARRY) >> 4);
        if (b > a) fix |= TOPDIG;               // borrow out of the top nibble

        return static_cast<store_t>(t - (fix << 2) - (fix << 1));
    }

    /*
     *  Applies a sign-magnitude term to this one: packed +/- mag.
     *  Packed BCD compares as a plain integer (digits are in order and
     *  every nibble is <= 9), so picking the larger magnitude costs an
     *  ordinary compare -- no digit walk.
     *
     *  On unsigned T there is no sign to track, so a negative term is
     *  just SubPacked and borrowing past zero wraps mod 10^DIGITS,
     *  like any other unsigned type.
     */
    constexpr bcd& Combine(const store_t mag, const bool mNeg) {
        if constexpr (std::is_signed_v<T>) {
            if (neg == mNeg)          packed = AddPacked(packed, mag);
            else if (packed >= mag)   packed = SubPacked(packed, mag);
            else { packed = SubPacked(mag, packed); neg = mNeg; }

            if (!packed) neg = false;   // never hand Draw() a -0
        }
        else packed = mNeg ? SubPacked(packed, mag) : AddPacked(packed, mag);

        return *this;
    }

    /*
     *  += / -= on a raw number: the operand is converted to digits
     *  first, at whichever of the two types is wider, so nothing is
     *  lost that this bcd had room for. A literal (`score += 50`)
     *  becomes its packed form at compile time and costs only the
     *  add; a runtime value pays one shared Dabble() call.
     *
     *  AI, because an outlined operator receives even a literal as an
     *  argument -- and then Digits() cannot tell it from a variable.
     *  What it force-inlines is this one line: the conversion and
     *  Combine() are still out of line, so the call site stays small.
     */
    template <nx U>
    AI constexpr bcd& operator+=(const U v) {
        return Combine(Digits(v), IsNeg(v));
    }

    template <nx U>
    AI constexpr bcd& operator-=(const U v) {
        return Combine(Digits(v), !IsNeg(v));
    }

    // += / -= between bcds of any two widths: already digits, so this
    // is the bare add -- narrowing to a smaller bcd keeps the low
    // digits, widening into a larger one is exact.
    template <nx U>
    constexpr bcd& operator+=(const bcd<U>& o) {
        return Combine(static_cast<store_t>(o.packed), o.Negative());
    }

    template <nx U>
    constexpr bcd& operator-=(const bcd<U>& o) {
        return Combine(static_cast<store_t>(o.packed), !o.Negative());
    }

    // Sign as a plain bool for every T, including the unsigned ones
    // where neg carries no byte at all.
    [[nodiscard]] constexpr bool Negative() const {
        if constexpr (std::is_signed_v<T>) return neg;
        else return false;
    }

    struct none {};   // zero sign byte on unsigned T

    /*
     *  The digits themselves, public and first: own them directly.
     *  Add, subtract, compare, bump a score by 50 -- all of it is
     *  arithmetic on this one integer, with no accessor in the way.
     *
     *  Both members are public deliberately. Mixing access would cost
     *  the class its standard layout, and with packed declared first
     *  and nothing ahead of it, &b, &b.packed and the u8* Draw() pins
     *  down are the same address -- so punning the object behaves
     *  exactly like punning the number.
     */
    store_t packed{};
    [[no_unique_address]] std::conditional_t<std::is_signed_v<T>, bool, none> neg{};

private:
    static constexpr u8 BITS = sizeof(T) << 3;
    static constexpr store_t TOP = static_cast<store_t>(1) << (BITS - 1);

    static constexpr store_t Repeat(const u8 nib) {
        store_t v = 0;
        for (u8 i = 0; i < DIGITS; ++i) v = static_cast<store_t>((v << 4) | nib);
        return v;
    }

    static constexpr store_t THREE  = Repeat(3);
    static constexpr store_t EIGHT  = Repeat(8);
    static constexpr store_t SIX    = Repeat(6);
    // Nibble carry/borrow positions: bits 4, 8, ... 4*(DIGITS-1).
    static constexpr store_t CARRY  = Repeat(1) & static_cast<store_t>(~store_t(1));
    // Bit 0 of the top nibble, where its own -6 correction lands.
    static constexpr store_t TOPDIG = static_cast<store_t>(1) << (BITS - 4);

    // `v < 0` written so an unsigned U never compiles it at all.
    template <nx U>
    static constexpr bool IsNeg(const U v) {
        if constexpr (std::is_signed_v<U>) return v < 0;
        else return false;
    }

    // A raw number as packed digits of this bcd's width. The magnitude
    // is converted at max(sizeof(U), sizeof(T)) so a wide operand does
    // not lose digits before it is even added, then narrowed -- which
    // drops only digits beyond DIGITS, the same truncation Make() does.
    template <nx U>
    AI static constexpr store_t Digits(const U v) {
        using wide = std::conditional_t<(sizeof(U) > sizeof(store_t)),
                                        std::make_unsigned_t<U>, store_t>;
        const auto mag = static_cast<wide>(abs<U>(v));

        /*
         *  A literal operand (`score += 50`) must not pay for a
         *  conversion at all, but a runtime one must not inline the
         *  whole dabble at every call site either. __builtin_constant_p
         *  is resolved after inlining, so the branch not taken is gone
         *  by codegen: constants fold through DabbleHere, runtime
         *  values share Dabble's single copy.
         */
        if (__builtin_constant_p(mag)) return static_cast<store_t>(bcd<wide>::DabbleHere(mag));

        return static_cast<store_t>(bcd<wide>::Dabble(mag));
    }
};
