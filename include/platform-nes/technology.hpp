/**
 * @file technology.hpp
 * @brief Low-level memory access primitives, toolchain shims, and
 *        assembly-backed data helpers.
 *
 * This header groups the "platform plumbing" used by the rest of the
 * library:
 *
 * - ::PEEK / ::POKE / ::SPACESHIP — portable memory and compare ops.
 * - ::atomic — maps to `volatile` on NES and `_Atomic` elsewhere.
 * - ::AI — force a function to be copied into every caller.
 * - ::NI — force a function to never be inlined into its caller(s).
 * - ::MINSIZE — Clang-only size-optimisation hint (silently dropped
 *   under GCC).
 * - ::CHARMAP, ::STRING, ::STRING_NT — define per-project character maps and
 *   emit compile-time-translated strings as header-only `inline constexpr`
 *   `std::array`s in `.rodata`.
 * - ::PopulateFromBuffer / ::PopulateFromProvider — generic strided
 *   byte copies used by the video and audio subsystems.
 * - ::CAT, ::STR, ::STRCAT — expand-then-paste and expand-then-stringize
 *   helpers, including for computed `#include` paths.
 */
#pragma once

#include <intsh>
using namespace br0::intsh;
#include <type_traits>
#include <array>
#include <cstddef>

/**
 * @brief Token-pastes @p a and @p b into one token, macro-expanding both first.
 *
 * Plain `a##b` pastes the raw spelling of its operands without expanding
 * them -- the classic gotcha: with `TARGET` defined to `nes`, `CAT_(TARGET, _data)`
 * gives `TARGETdata`, not `nesdata`. ::CAT forwards through ::CAT_ so both
 * operands are macro-expanded as ordinary arguments before the paste (an
 * argument directly touching `##` in the SAME macro is exempt from
 * expansion; routing it through an outer macro first is the standard fix).
 *
 * @note The paste result must itself be a single valid preprocessing token --
 *       identifier/number fragments combine safely; pasting across a `/`,
 *       `.`, or other punctuator is undefined behaviour (GCC/Clang reject
 *       it). Use ::STRCAT for building a path instead.
 *
 * @param a First token.
 * @param b Second token.
 */
#define CAT_(a, b) a##b
#define CAT(a, b)  CAT_(a, b)

/**
 * @brief Stringizes @p x into a string literal, macro-expanding @p x first.
 *
 * Plain `#x` stringizes @p x's literal spelling without expanding it, so
 * with `TARGET` defined to `nes`, `#TARGET` yields `"TARGET"`, not `"nes"`.
 * ::STR forwards through ::STR_ so @p x is expanded as an ordinary argument
 * before the `#` sees it (same fix as ::CAT, for stringizing instead of
 * pasting).
 *
 * @param x Token sequence to expand and stringize.
 */
#define STR_(x) #x
#define STR(x)  STR_(x)

/**
 * @brief Builds a computed `#include` argument from an unquoted, macro-
 *        expandable path, in one step.
 *
 * A computed include must collapse to exactly ONE string-literal token after
 * macro expansion:
 *
 *     #include STRCAT(gen/TARGET/some_file.hpp)
 *
 * Write the path bareword (no quotes, no internal spaces) with any macro
 * pieces -- like a `TARGET` the build defines to `nes`, `gba`, ... -- embedded
 * directly; ::STRCAT expands them and stringizes the whole path in one go,
 * same as ::STR.
 *
 * @warning This is NOT string concatenation of separately-quoted pieces:
 *          `#include` requires one string-literal token, and pasting quoted
 *          string literals together with `##` is undefined behaviour. Do not
 *          try to build a path from `"gen/"`, ::STR(TARGET), `"file.hpp"`
 *          separately -- write it as one unquoted argument instead, as above.
 *
 * @param path Bareword, macro-expandable path.
 */
#define STRCAT(path) STR(path)

/**
 * @brief Force a function to be copied into every caller.
 *
 * Not a speed hint: the inliner already says yes to almost everything here.
 * ::AI is for when out-of-lining would be a CORRECTNESS bug -- an interrupt
 * handler's budget is one vblank or one scanline, and the cost model can start
 * declining a function on its own as call sites multiply.
 *
 * @warning Does not compose with `noinline`, which wins with no diagnostic --
 *          so a function taking this must NOT also take ::MODULE_PLACEMENT.
 *          The two are mutually exclusive: one copy in a named bank, or a copy
 *          in every caller.
 */
#define AI __attribute__((always_inline))

/**
 * @brief Force a function to NEVER be inlined into its caller(s).
 *
 * For when placement, not speed, is the point: a banked function reached
 * through a single call site inside a farcall trampoline (compiled as part
 * of ::fixed) is a prime LTO inlining target -- it'll happily relocate the
 * whole body out of its intended bank. ::NI pins a real JSR target instead.
 *
 * @warning Does not compose with `always_inline` -- don't take both ::NI and ::AI.
 */
#define NI __attribute__((noinline))


#ifdef TARGET_NES
/**
 * @brief Builds the body of a segment-placement keyword like ::fixed.
 *
 * Expands to the `__attribute__((section(...)))` pinning a symbol into a section
 * the mapper's linker script maps to a real PRG-ROM region. A builder, not a
 * keyword: the preprocessor cannot register a macro name from inside another
 * expansion, so this can only be the right-hand side of a keyword definition --
 * `#define fixed CREATE_SEGMENT_KEYWORD(".prg_rom_fixed")` -- after which the
 * bare word is usable as a qualifier. Nothing off-NES.
 */
#define CREATE_SEGMENT_KEYWORD(name) __attribute__((section(name)))
#else
#define CREATE_SEGMENT_KEYWORD(name)
#endif

#if defined(TARGET_NES) && defined(NES_MAPPER_BANKSWITCHED)
/**
 * @brief Builds a placement keyword for a whole library MODULE, from the
 *        section the consuming project chose for it.
 *
 * Same builder rule as ::CREATE_SEGMENT_KEYWORD, guarded on whether the project
 * asked for placement:
 *
 *     #ifdef PLATFORM_NES_VIDEO_SECTION
 *     #define VIDEO_BANK MODULE_PLACEMENT(PLATFORM_NES_VIDEO_SECTION)
 *     #else
 *     #define VIDEO_BANK
 *     #endif
 *
 * THE `noinline` IS THE WHOLE POINT: a section governs only a function's
 * out-of-line copy, and LTO will paste the body into callers elsewhere, leaving
 * the section empty. Placement therefore costs that module its inlining.
 * Functions that must stay inline take ::AI instead, and no section.
 *
 * No `used`/`retain`: every mechanism here names its target, so pinning would
 * only keep uncalled API alive. A module with a genuinely invisible caller says
 * so on the function that needs it.
 *
 * Nothing off-NES, and nothing on NROM -- one fixed bank has nowhere to move
 * code to. CMake warns if a section is set on such a build.
 */
#define MODULE_PLACEMENT(name) CREATE_SEGMENT_KEYWORD(name) \
                               __attribute__((noinline))

#else
#define MODULE_PLACEMENT(name)
#endif


#ifdef __cplusplus
namespace tech {
/**
 * @brief Reads one byte from a memory-mapped I/O address.
 *
 * Typed replacement for the former `PEEK()` macro: a single `volatile`
 * load. `always_inline` collapses it to the identical 6502 codegen the
 * macro produced, but with real parameter/return types and none of the
 * macro's expansion pitfalls. The C build keeps the macro form (`#else`).
 *
 * @param addr Hardware address to read.
 * @return The byte currently stored at @p addr.
 */
inline AI
u8 peek(const u16 addr) {
    return *reinterpret_cast<volatile const u8 *>(addr);
}

/**
 * @brief Writes one byte to a memory-mapped I/O address.
 *
 * Typed replacement for the former `POKE()` macro: a single `volatile`
 * store, inlined to identical codegen.
 *
 * @param addr Hardware address to write.
 * @param data Byte to store.
 */
inline AI
void poke(const u16 addr, const u8 data) {
    *reinterpret_cast<volatile u8 *>(addr) = data;
}

#ifdef TARGET_NES
/**
 * @brief Busy-waits for a precise number of CPU cycles.
 *
 * ::AI, so the asm body is spliced into the caller with no `jsr`/`rts`. Elapsed
 * time is exactly `c + 15` cycles for any `c` -- 15 to 270 -- independent of how
 * the caller gets @p c into place. For a shorter compile-time-known wait,
 * hand-pick a NOP/BIT sled instead (nesdev.org/wiki/Fixed_cycle_delay).
 *
 * From NESdev's "Delay code" article: an `sbc #5` loop peels 5-cycle slices off
 * @p c, then a branch cascade burns the 0-4 remainder. The wiki's `+ 27` assumes
 * a `jsr`-called body; inlined, only the `+ 15` survives.
 *
 * @warning Same assumptions as that source: no branch crosses a page boundary
 * (link-time placement decides that, so it is probabilistic), no interrupt
 * fires during the wait, no concurrent DMA.
 *
 * @param c Delay selector; total wait is `c + 15` CPU cycles.
 */
inline AI
void SpinWait(const u8 c) {
    __asm__ volatile (
        "sec\n"
        "1:\n"
        "sbc #5\n"
        "bcs 1b\n"
        "adc #3\n"
        "bcc 2f\n"
        "lsr a\n"
        "beq 3f\n"
        "2:\n"
        "lsr a\n"
        "3:\n"
        "bcs 4f\n"
        "4:\n"
        :
        : "a" (c)
        : "p"
    );
}
#else
/**
 * @brief No-op off NES.
 *
 * @note The desktop/emulated backends have no cycle-exact CPU timing
 * model -- they schedule IRQs by pixel/scanline position, not by counting
 * CPU cycles -- so there is nothing meaningful to busy-wait on here. See
 * the NES-side ::tech::SpinWait above for the real implementation and its
 * exact-cycle guarantee.
 */
inline void SpinWait(const u8) {}
#endif
} // namespace tech
#else
/** @brief C fallback: byte read via a `volatile const` dereference. */
#define PEEK(addr)       (*(volatile const unsigned char *)(addr))
/** @brief C fallback: byte write via a `volatile` store. */
#define POKE(addr, data) (*(volatile unsigned char *)(addr)) = (data)
#endif

/**
 * @brief Three-way comparison.
 *
 * Yields `-1`, `0`, or `+1` for `l < r`, `l == r`, `l > r` respectively.
 * @param l Left operand.
 * @param r Right operand.
 */
#define SPACESHIP(l, r) \
    (l == r             \
        ? 0             \
        : l > r         \
            ? 1         \
            : -1)

#ifdef TARGET_NES
/**
 * @brief Atomic qualifier — `volatile` on the single-core NES.
 *
 * The NES is single-core with no concurrent access, so `volatile` is
 * sufficient to prevent the optimiser from reordering accesses.
 */
#define atomic volatile
#elif defined(__cplusplus)
/**
 * @brief Atomic qualifier — `volatile` on C++ desktop builds.
 *
 * C++ has no `_Atomic` type qualifier: GCC rejects it outright and
 * C++23's `<stdatomic.h>` only exposes the functional `_Atomic(T)`
 * form, not the prefix qualifier this macro is used as. The SDL3
 * emulator touches every `atomic`-qualified object from a single
 * thread (the renderer drains IRQs synchronously), so `volatile`
 * preserves the same ordering guarantees the NES build relies on.
 * The one genuinely cross-thread flag, `_vblank_flag`, is a real
 * `atomic_int` from `<stdatomic.h>` and does not use this macro.
 */
#define atomic volatile
#else
/** @brief Atomic qualifier — standard `_Atomic` on desktop C builds. */
#define atomic _Atomic
#endif

#ifdef TARGET_NES
/**
 * @brief Pins a variable into 6502 zero page ("direct page").
 *
 * Forces the symbol into `.zp.bss`, so access is a 2-byte/3-cycle direct-page
 * instruction rather than the 3-byte/4-cycle absolute form. Zero page is 256
 * bytes shared with the compiler's imaginary registers, so reserve this for the
 * hottest mutable state. Objects with a static initialiser belong in `.zp.data`.
 * Nothing off-NES.
 */
#define direct __attribute__((section(".zp.bss")))
/**
 * @brief Forces a variable out of zero page, into absolute-addressed memory.
 *
 * llvm-mos's LTO zero-page allocator opportunistically hoists small globals
 * into zero page; `absolute` opts a symbol out by pinning it to `.rodata`, so a
 * read-only table that is merely walked can never evict hotter state or starve
 * the FamiStudio enclave out of page zero. Use it for the level RLE tables and
 * similar cold lookup data. Read-only data is always absolute-addressed, so
 * there is no separate ROM qualifier.
 *
 * Expands to nothing off-NES.
 */
#define absolute __attribute__((section(".rodata")))
#else
#define direct
#define absolute
#endif

#ifdef __cplusplus
#include <br0/tuple>

namespace tech {

/**
 * @brief Write-only hardware register with a RAM shadow.
 *
 * Models a memory-mapped port that cannot be read back (e.g. the NES
 * PPUCTRL/PPUMASK registers). The RAM shadow is the readable copy, and because
 * the only write path also pokes the hardware, shadow and register can never
 * drift: `get()` returns the shadow, `set()` writes both.
 *
 * `operator u8` / `operator=(u8)` let an instance be used exactly like the bare
 * `atomic u8` it replaces. Copy-construction snapshots *without* poking while
 * copy-assignment writes through -- what ::SHADOW needs to save and restore.
 *
 * The shadow stays `atomic`, so ordering guarantees survive for interrupt-side
 * callers, and the trivial default constructor keeps it in `.bss` with no
 * startup constructor.
 *
 * @tparam Addr Memory-mapped address backing this register.
 */
template <u16 Addr>
class wo_register {
    atomic u8 shadow_;
public:
    wo_register() = default;                                  ///< trivial: instance lives in .bss
    wo_register(const wo_register &o) : shadow_(o.shadow_) {} ///< snapshot, no poke

    u8   get() const          { return shadow_; }                  ///< read  = shadow
    void set(const u8 v)      { shadow_ = v; poke(Addr, v); }      ///< write = shadow + hardware
    void poke_only(const u8 v){ poke(Addr, v); }                    ///< write hardware only, no shadow update

    operator u8() const { return shadow_; }
    wo_register &operator=(const u8 v)           { set(v);         return *this; }
    wo_register &operator=(const wo_register &o) { set(o.shadow_); return *this; } ///< restore path
};

/**
 * @brief Zero-overhead bitmask wrapper for a scoped enum.
 *
 * A scoped enum keeps flag names out of the surrounding namespace but does not
 * implicitly convert to its underlying integer, so `a | b` and `if (flags)`
 * stop compiling and every use needs a static_cast.
 *
 * `enum_flags<E>` is the storage type for such a register: one
 * `std::underlying_type_t<E>` plus the bitwise operators and a contextual
 * `bool`, so flag code reads like integer code while staying type-checked
 * against `E`. Enumerators convert implicitly *into* it but not back out, so
 * two unrelated enums still cannot mix.
 *
 * Pure compile-time abstraction: no vtable, `sizeof` equals the underlying type,
 * every operation `constexpr` and lowering to the same `and`/`ora`/`eor` raw
 * byte math would.
 *
 * @note On an `atomic` register keep read-modify-write as one
 *       `reg = reg | E::FLAG;` statement -- one read, one write, side-stepping
 *       the `-Wdeprecated-volatile` compound-assignment pitfall.
 *
 * @tparam E A scoped enumeration whose enumerators are single-bit flag values.
 */
template <class E>
class enum_flags {
    using U = std::underlying_type_t<E>;
    U bits_{};
public:
    constexpr enum_flags() = default;                                  ///< trivial: lives in .bss
    constexpr enum_flags(E e) : bits_(static_cast<U>(e)) {}            ///< enumerator -> flag set
    constexpr explicit enum_flags(U raw) : bits_(raw) {}              ///< from a raw underlying value

    // Volatile interop: an `atomic enum_flags<E>` register is read once when
    // copied by value into the operators below, and written once on assignment.
    // Both stay single-access (no read-modify-write on the volatile object) so
    // the `-Wdeprecated-volatile` (P1152) pitfall never arises; the volatile
    // operator= returns void for the same reason (its result is never read back).
    // The volatile-source ctor / volatile operator= match copy signatures, so the
    // ordinary (non-volatile) copy ops are re-defaulted alongside them.
    constexpr enum_flags(const enum_flags &) = default;
    constexpr enum_flags &operator=(const enum_flags &) = default;
    enum_flags(const volatile enum_flags &o) : bits_(o.bits_) {}        ///< read a volatile register
    void operator=(const enum_flags &o) volatile { bits_ = o.bits_; }   ///< write a volatile register

    constexpr explicit operator bool() const { return bits_ != 0; }   ///< `if (flags & mask)`
    constexpr U raw()   const { return bits_; }                       ///< underlying integer
    constexpr E value() const { return static_cast<E>(bits_); }       ///< back to the enum type

    /// True iff every bit in @p m is set (membership test for a multi-bit mask).
    constexpr bool has(enum_flags m) const { return (bits_ & m.bits_) == m.bits_; }

    friend constexpr enum_flags operator|(enum_flags a, enum_flags b) { return enum_flags(static_cast<U>(a.bits_ | b.bits_)); }
    friend constexpr enum_flags operator&(enum_flags a, enum_flags b) { return enum_flags(static_cast<U>(a.bits_ & b.bits_)); }
    friend constexpr enum_flags operator^(enum_flags a, enum_flags b) { return enum_flags(static_cast<U>(a.bits_ ^ b.bits_)); }
    friend constexpr enum_flags operator~(enum_flags a)               { return enum_flags(static_cast<U>(~a.bits_)); }

    constexpr enum_flags &operator|=(enum_flags o) { bits_ = static_cast<U>(bits_ | o.bits_); return *this; }
    constexpr enum_flags &operator&=(enum_flags o) { bits_ = static_cast<U>(bits_ & o.bits_); return *this; }
    constexpr enum_flags &operator^=(enum_flags o) { bits_ = static_cast<U>(bits_ ^ o.bits_); return *this; }

    friend constexpr bool operator==(enum_flags, enum_flags) = default;   ///< also yields !=
};

namespace prsv {
    template <std::size_t... I, class... Ts>
    void save_impl(u8* snap, br0::index_sequence<I...>, Ts&... regs) {
        ((snap[I] = static_cast<u8>(regs)), ...);
    }

    template <std::size_t... I, class... Ts>
    void restore_impl(u8* snap, br0::index_sequence<I...>, Ts&... regs) {
        ((regs = snap[I]), ...);
    }

    /**
     * @brief Write shadow values of @p regs into the flat byte array @p snap.
     *
     * Called by ::PRESERVE. Reads each lvalue once through its shadow (no
     * hardware access) and stores the byte at the corresponding index.
     */
    template <class... Ts>
    void save(u8* snap, Ts&... regs) {
        save_impl(snap, br0::index_sequence_for<Ts...>{}, regs...);
    }

    /**
     * @brief Write bytes from @p snap back to the lvalues @p regs.
     *
     * Called by ::RESTORE. For ::wo_register lvalues this writes both the
     * shadow and the hardware port; for raw `atomic u8` lvalues it is a
     * plain volatile store.
     */
    template <class... Ts>
    void restore(u8* snap, Ts&... regs) {
        restore_impl(snap, br0::index_sequence_for<Ts...>{}, regs...);
    }
} // namespace prsv

/**
 * @brief Snapshot a register family into its paired static storage.
 *
 * A register family is a pair of definitions that live together:
 * @code
 *   #define MY_REGS         reg_a, reg_b, reg_c
 *   inline u8 MY_REGS_snapshot[3] = {};
 * @endcode
 *
 * Saves every listed register's shadow into `MY_REGS_snapshot`. The `##` stops
 * the family name expanding while building that symbol, so ::PRESERVE and
 * ::RESTORE agree on it across translation units. No hardware is read.
 *
 * @param name Bare identifier of the family macro (e.g. `APU_REGISTERS`).
 */
#define PRESERVE(name) ::tech::prsv::save(name##_snapshot, name)

/**
 * @brief Restore a ::PRESERVE-d register family from static storage.
 *
 * Writes `name##_snapshot` back into every listed register. For
 * ::wo_register lvalues this pokes the hardware port and updates the shadow;
 * for raw `atomic u8` lvalues it is a plain volatile store.
 *
 * @param name Bare identifier of the family macro (e.g. `APU_REGISTERS`).
 */
#define RESTORE(name) ::tech::prsv::restore(name##_snapshot, name)

/**
 * @brief Scoped save/restore of N registers — truly variadic, and cheap.
 *
 * `SHADOW(a, b, ...) { body }` snapshots each listed register, runs the body
 * once, then restores every snapshot in reverse (LIFO) order as the block
 * exits — including on `return` out of the body. Arguments may be of mixed
 * types; `volatile`/`atomic` lvalues are read and written exactly once.
 *
 * The registers are TEMPLATE parameters, so their addresses are compile-time and
 * the scope holds only the saved bytes -- one per register. Base destructors run
 * in reverse declaration order, which gives LIFO for free.
 *
 * @note They must therefore have LINKAGE. A local or parameter cannot be a
 *       template argument, so save such a byte by hand.
 *
 * @warning Expands to an `if` with an init-statement, so a trailing `else` binds
 *          to it, and `break`/`continue` bind to the enclosing loop.
 */
template <auto &Reg>
struct one_shadow {
    /// One read at entry, through the register's own copy constructor
    /// (::wo_register snapshots the shadow without poking hardware).
    //
    // remove_reference_t is load-bearing: `decltype` on a reference template
    // parameter yields the REFERENCE type, and remove_cv_t does not strip it.
    // Without it `saved` binds to the register instead of copying it, and the
    // destructor's restore degenerates into writing the current value back --
    // silently, since it compiles either way. remove_cvref_t would say this in
    // one step but llvm-mos's freestanding library does not ship it.
    std::remove_cv_t<std::remove_reference_t<decltype(Reg)>> saved{Reg};

    /// One write back on exit. For ::wo_register this pokes the hardware port
    /// and updates the shadow; for a raw `atomic u8` it is a volatile store.
    ~one_shadow() { Reg = saved; }
};

template <auto &...Regs>
struct shadow_scope : one_shadow<Regs>... {};

/// Retained only for ::PRESERVE/::RESTORE-style call sites that pass lvalues.
template <class... Ts>
struct shadow_scope_dynamic {
    br0::tuple<Ts&...> refs;                    ///< the originals (pointers only)
    br0::tuple<std::remove_cv_t<Ts>...> saved;  ///< one read each at entry
    bool first = true;

    explicit shadow_scope_dynamic(Ts&... rs) : refs(rs...), saved(rs...) {}
    ~shadow_scope_dynamic() { restore(br0::index_sequence_for<Ts...>{}); }

    // One store, written as a discarded-value statement so the volatile
    // assignment's result is never "used" (avoids -Wdeprecated-volatile under
    // P1152, exactly like the longhand stores elsewhere in this header).
    template <class D, class S>
    static void store(D& dst, const S& src) { dst = src; }

    // LIFO restore to match the documented contract; each element is a single
    // write back through the original reference.
    template <std::size_t... I>
    void restore(br0::index_sequence<I...>) {
        constexpr std::size_t N = sizeof...(Ts);
        (store(br0::get<N - 1 - I>(refs), br0::get<N - 1 - I>(saved)), ...);
    }

    explicit operator bool() { const bool f = first; first = false; return f; }
};
template <class... Ts>
shadow_scope_dynamic(Ts&...) -> shadow_scope_dynamic<Ts...>;

} // namespace tech

/**
 * @brief Open a SHADOW scope over the listed registers.
 *
 * Expands to an `if` with an init-statement, whose variable is a
 * ::tech::shadow_scope parameterised on the registers themselves. The body runs
 * once; on exit the scope's bases restore every snapshot, last-listed first.
 *
 * The registers are TEMPLATE arguments, not constructor arguments — that is
 * what keeps their addresses out of the object at runtime (see
 * ::tech::shadow_scope). They must have linkage; every hardware register in
 * this library does.
 */
#define SHADOW(...)                                                    \
    if (::tech::shadow_scope<__VA_ARGS__> CAT(_shadow_, __LINE__){}; true)
#endif // __cplusplus

#if !defined(__cplusplus) && __STDC_VERSION__ < 202311L
#define auto __auto_type   // pre-C23: borrow the extension
#endif                     // C23+ / C++: native auto, no macro needed

#ifdef __cplusplus
/** @brief C linkage for symbols shared with hand-written/ca65 assembly. */
#define ASM_LINKAGE extern "C"
#else
#define ASM_LINKAGE extern
#endif

/**
 * @brief Clang-only function attribute biasing the optimiser toward small code.
 *
 * Applied to functions where ROM size matters more than speed. GCC
 * builds silently drop the attribute — expanding to nothing — because
 * it only exists in Clang/LLVM.
 */
#if defined(__clang__)
#define MINSIZE __attribute__((minsize))
#else
#define MINSIZE
#endif

/**
 * @brief Compile-time support for ::CHARMAP / ::STRING.
 */
namespace tech::nes_str {
    /**
     * @brief Fallback for a character with no ::CM entry.
     *
     * Declared but never defined, and not @c constexpr: naming it during the
     * constant evaluation of a ::STRING makes that evaluation non-constant,
     * so an unmapped character is a compile error pointing at the offending string.
     * Because a charmap is only ever evaluated at compile time, this is never
     * ODR-used at run time, so the missing definition never reaches the linker.
     */
    [[noreturn]] u8 unmapped(char c);

    /** @brief Map @p s[0 .. N-2] through @p Map into bytes (drops the literal NUL). */
    template <u8 (*Map)(char), std::size_t N>
    constexpr std::array<u8, N - 1> encode(const char (&s)[N]) {
        std::array<u8, N - 1> out{};
        for (std::size_t i = 0; i + 1 < N; ++i) out[i] = Map(s[i]);
        return out;
    }

    /** @brief As ::encode, but keeps a trailing 0x00 terminator. */
    template <u8 (*Map)(char), std::size_t N>
    constexpr std::array<u8, N> encode_nt(const char (&s)[N]) {
        std::array<u8, N> out{};
        for (std::size_t i = 0; i + 1 < N; ++i) out[i] = Map(s[i]);
        return out;
    }

    /** @brief ::SIZED_OBJ plumbing: a pointer+count that works for both a raw C
     *         array and a @c std::array (so existing call sites are unchanged). */
    template <class T, std::size_t N> constexpr const T*      data(const T (&a)[N]) { return a; }
    template <class T, std::size_t N> constexpr std::size_t   size(const T (&)[N])  { return N; }
    template <class A> constexpr auto data(const A& a) -> decltype(a.data()) { return a.data(); }
    template <class A> constexpr auto size(const A& a) -> decltype(a.size()) { return a.size(); }
}

/**
 * @brief Internal building block used inside ::CHARMAP.
 *
 * Expands to one arm of the charmap's @c constexpr lookup: when the input
 * character equals token @p ch, return byte @p val. ::CM entries are juxtaposed
 * (no commas) inside ::CHARMAP, exactly as before -- each is now an @c if
 * statement instead of a string-literal fragment.
 *
 * @param ch  Bareword character token (e.g. `M`); taken as `(#ch)[0]`.
 * @param val Byte value to return when @p ch is matched.
 */
#define CM(ch, val) if (_c == (#ch)[0]) return (u8)(val);

/**
 * @brief Defines a named character map used by ::STRING.
 *
 * Expands to a @c constexpr `charmap_<mapname>(char)` whose body is the
 * juxtaposed ::CM arms; an unmapped character falls through to
 * ::nes_str::unmapped, a compile error at the offending string. Being an
 * ordinary constexpr function it can live in a header and be included freely.
 *
 * @note ::CM takes bareword tokens (`CM(M, 0xed)`), stringized with the first
 *       character taken.
 *
 * @param mapname Identifier for the character map.
 */
#define CHARMAP(mapname, ...)                   \
constexpr u8 charmap_##mapname(char _c) {       \
    __VA_ARGS__                                 \
    return ::tech::nes_str::unmapped(_c);       \
}

/**
 * @brief Defines a string translated through a ::CHARMAP (no terminator).
 *
 * Header-only: each character of @p chars is mapped through `charmap_<mapname>`
 * at compile time into an @c inline @c constexpr `std::array<u8, N>` of the
 * unterminated source length, which the linker folds to one rodata object
 * however many TUs include it. `sizeof(name)` is therefore the character count,
 * usable in constant expressions. An unmapped character is a compile error.
 *
 * @note Requires `charmap_<mapname>` to be in scope at the point of use (include
 *       the header that declares the ::CHARMAP).
 *
 * @param mapname Name of a previously declared ::CHARMAP.
 * @param name    Symbol to define.
 * @param chars   Source characters to translate (bareword, not a string).
 */
#define STRING(mapname, name, chars)            \
inline constexpr auto name = ::tech::nes_str::encode<charmap_##mapname>(#chars)

/**
 * @brief Defines a string translated through a ::CHARMAP, with a 0x00 terminator.
 *
 * Identical to ::STRING but keeps a trailing 0x00, so the array is
 * `sizeof(#chars)` bytes (the source length including the terminator).
 *
 * @param mapname Name of a previously declared ::CHARMAP.
 * @param name    Symbol to define.
 * @param chars   Source characters to translate (bareword, not a string).
 */
#define STRING_NT(mapname, name, chars)         \
inline constexpr auto name = ::tech::nes_str::encode_nt<charmap_##mapname>(#chars)

/**
 * @brief Expands to `pointer, count` for passing a buffer+length pair.
 *
 * Works for both a raw C array and a `std::array` (see ::nes_str::data /
 * ::nes_str::size), so ::STRING results and plain arrays are accepted alike.
 *
 * @param obj A C array or `std::array`.
 */
#define SIZED_OBJ(obj) ::tech::nes_str::data(obj), ::tech::nes_str::size(obj)

/**
 * @brief General-purpose byte copy from @p buffer into @p target with stride.
 *
 * Writes `buffer[i]` to `target[offset + i * step]` for `i` in `[0, sBuffer)`.
 * A negative @p step walks backward; the caller keeps every index in range.
 * Count and stride types are generic, so pass `u8`/`i8` for short runs to keep
 * the loop counter single-byte. Index arithmetic is evaluated in `int` so
 * negative strides are portable.
 *
 * @tparam Count Unsigned integer type of @p sBuffer / the loop counter.
 * @tparam Step  Signed integer type of @p step.
 * @param target  Destination buffer.
 * @param offset  Starting index inside @p target.
 * @param buffer  Source byte array.
 * @param sBuffer Number of bytes to copy from @p buffer.
 * @param step    Signed stride in @p target between consecutive writes.
 */
namespace tech {
template <typename Count, typename Step>
void PopulateFromBuffer(u8* target, const u16 offset,
                        const u8* buffer, const Count sBuffer, const Step step) {
    const auto base = target + offset;
    for (Count i = 0; i < sBuffer; ++i) base[static_cast<int>(i) * step] = buffer[i];
}

/**
 * @brief General-purpose fill from a provider function with stride.
 *
 * Writes `fn(i)` to `target[offset + i * step]` for `i` in `[0, amt)`.
 * @p step is signed; a negative value walks backward from @p offset.
 *
 * @p Idx (deduced from @p fn), @p Count and @p Step are generic, so narrow
 * integers keep the loop control and callback argument single-byte. The index
 * multiply is evaluated in `int` for portable negative strides.
 *
 * @tparam Idx   Parameter type of @p fn (the value handed to the callback).
 * @tparam Count Unsigned integer type of @p amt / the loop counter.
 * @tparam Step  Signed integer type of @p step.
 * @param target Destination buffer.
 * @param offset Starting index inside @p target.
 * @param fn     Provider callback returning the byte to store at iteration `i`.
 * @param amt    Number of iterations to perform.
 * @param step   Signed stride in @p target between consecutive writes.
 */
template <typename Idx, typename Count, typename Step>
void PopulateFromProvider(u8* target, const u16 offset,
                          u8 (*fn)(Idx), const Count amt, const Step step) {
    const auto base = target + offset;
    for (Count i = 0; i < amt; ++i)
        base[static_cast<int>(i) * step] = fn(static_cast<Idx>(i));
}
} // namespace tech
