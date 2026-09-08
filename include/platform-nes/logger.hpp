/**
 * @file logger.hpp
 * @brief Zero-footprint NES logging.
 *
 * log("message") never becomes an instruction on TARGET_NES: it declares
 * `static const` data (never read or written by anything at runtime) tagged
 * into `.pnes_log`, a section none of the mapper linker scripts (see
 * src/nes/mappers/debug-log.ld, INCLUDEd from mmc3-helper.ld/vrc1-helper.ld)
 * ever assign to a MEMORY region -- it survives in the linked .elf for the
 * host-side `logger` tool to read back out after the build, but contributes
 * zero bytes to the final ROM image and zero cycles to anything that runs.
 *
 * `logger` pairs each call site's __FILE__/__LINE__/message (read straight
 * out of .pnes_log -- never by reopening the .cpp/.hpp source) with the
 * nearest real instruction's address via the .elf's own DWARF line table,
 * then resolves that address to a flat, bank-switching-agnostic PRG-ROM
 * offset using nothing but standard ELF section metadata (see this
 * project's design notes -- this step needs no mapper-specific knowledge).
 * tools/pnes.lua then prints the message in Mesen whenever the CPU
 * actually reaches that physical location, regardless of which mapper is in
 * play or which bank currently occupies that CPU window.
 *
 * Off NES, log(msg) is real: there is no ROM budget to protect and no
 * emulator needed to see it, so it just prints to stdout immediately.
 */
#pragma once

#include <intsh>
using namespace br0::intsh;

#ifdef TARGET_NES

#include <type_traits>

namespace log_detail {

/**
 * @brief One %-format argument's compile-time-captured location.
 *
 * Never the VALUE -- log() itself never reads or formats anything on NES.
 * `address` is a real, live CPU address (RAM/zero-page/WRAM), not a
 * .pnes_log offset like Entry::file/message are: tools/pnes.lua reads it
 * straight out of Mesen's own memory when the log point fires. That's only
 * possible because `address` is fixed at link time, which means `value` in
 * ::MakeArg must be a real object with static storage duration (a global or
 * static, or a member of one) -- a local variable or temporary has no
 * address that's still meaningful by the time Mesen gets around to reading
 * it, so passing one here simply fails to compile.
 */
struct Arg {
    const volatile void *address;
    u8 size;
    u8 is_signed;
};

/**
 * @brief One log() call site's compiler-generated metadata.
 *
 * Never read at runtime -- the host-side `logger` tool is the only reader,
 * and only after the build, straight out of the linked .elf's .pnes_log
 * section. `file` and `message` point at sibling .pnes_log data, never at
 * .rodata: see ::PNES_LOG_TEXT for why that distinction matters. `args`
 * likewise points at sibling .pnes_log data (an ::Arg array); `arg_count`
 * says how many of it are real -- see ::PNES_LOG_IMPL for why the array
 * itself always has at least one (possibly unused) element.
 */
struct Entry {
    const char *file;
    u32 line;
    const char *message;
    const Arg *args;
    u8 arg_count;
    u8 kind; // PNES_LOG_KIND_LOG or PNES_LOG_KIND_PAUSE
};

/// See ::Arg's own header comment for why `value` must be a fixed-address
/// object. Enums are captured by their underlying type's signedness.
template <typename T>
constexpr Arg MakeArg(const volatile T &value) {
    using U = std::conditional_t<std::is_enum_v<T>, std::underlying_type_t<T>, T>;
    return Arg{ &value, static_cast<u8>(sizeof(T)), static_cast<u8>(std::is_signed_v<U> ? 1 : 0) };
}

} // namespace log_detail

#define PNES_LOG_CAT_(a, b) a##b
#define PNES_LOG_CAT(a, b)  PNES_LOG_CAT_(a, b)

/**
 * @brief Places a string literal's bytes into .pnes_log instead of .rodata.
 *
 * Needed for both __FILE__ and the caller's message: an ::Entry pointing at
 * ordinary .rodata data would leak those bytes into the ROM even though the
 * Entry itself doesn't. `used` keeps the compiler from treating an
 * apparently-dead (never read) static array as eligible for removal --
 * nothing at runtime ever dereferences these pointers, which is exactly
 * what "zero footprint" requires, but it means nothing at runtime looks
 * like a "use" either.
 */
#define PNES_LOG_TEXT(name, text) \
    static const char name[] __attribute__((section(".pnes_log"), used)) = text

// Entry::kind values -- see tools/pnes.lua for how each is handled: a log
// entry gets formatted and printed, a pause entry halts emulation instead.
#define PNES_LOG_KIND_LOG   0
#define PNES_LOG_KIND_PAUSE 1

// Counts __VA_ARGS__ (0 through 8). The leading `dummy` absorbs the
// zero-args case, where __VA_OPT__ contributes no comma and __VA_ARGS__
// contributes no tokens at all.
#define PNES_LOG_NARG_(_0, _1, _2, _3, _4, _5, _6, _7, _8, N, ...) N
#define PNES_LOG_NARG(...) \
    PNES_LOG_NARG_(dummy __VA_OPT__(,) __VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1, 0)

// Maps ::log_detail::MakeArg over each of up to 8 comma-separated arguments.
// PNES_LOG_CAT forces PNES_LOG_NARG(...) to expand to a plain digit before
// pasting, the same way it's already used to build unique per-call-site names.
#define PNES_LOG_MAP1(x)      ::log_detail::MakeArg(x)
#define PNES_LOG_MAP2(x, ...) ::log_detail::MakeArg(x), PNES_LOG_MAP1(__VA_ARGS__)
#define PNES_LOG_MAP3(x, ...) ::log_detail::MakeArg(x), PNES_LOG_MAP2(__VA_ARGS__)
#define PNES_LOG_MAP4(x, ...) ::log_detail::MakeArg(x), PNES_LOG_MAP3(__VA_ARGS__)
#define PNES_LOG_MAP5(x, ...) ::log_detail::MakeArg(x), PNES_LOG_MAP4(__VA_ARGS__)
#define PNES_LOG_MAP6(x, ...) ::log_detail::MakeArg(x), PNES_LOG_MAP5(__VA_ARGS__)
#define PNES_LOG_MAP7(x, ...) ::log_detail::MakeArg(x), PNES_LOG_MAP6(__VA_ARGS__)
#define PNES_LOG_MAP8(x, ...) ::log_detail::MakeArg(x), PNES_LOG_MAP7(__VA_ARGS__)
#define PNES_LOG_MAP(N, ...)  PNES_LOG_CAT(PNES_LOG_MAP, N)(__VA_ARGS__)

/**
 * @brief Zero-footprint log call site. See this file's own header comment.
 *
 * log("plain message") works exactly as before. log("hp=%d", hp) also
 * captures hp's address/size/signedness (see ::log_detail::MakeArg) into
 * .pnes_log, right beside the format string -- nothing is ever formatted on
 * NES itself; tools/pnes.lua reads hp's LIVE value out of Mesen's own
 * memory and interpolates it there when this line actually executes.
 *
 * __COUNTER__ is captured once via the id parameter (expanded as an
 * argument, then pasted repeatedly) so every declaration below names the
 * same call site consistently -- referencing __COUNTER__ directly more than
 * once would yield a different value each time.
 *
 * The Arg array always has a trailing dummy element even with zero real
 * args, so it's never a zero-size array (ill-formed) and Entry::args is
 * always a valid, non-null pointer -- Entry::arg_count (not the array's own
 * size) is what tools/pnes.lua actually trusts.
 */
#define log(msg, ...) \
    PNES_LOG_IMPL(PNES_LOG_KIND_LOG, msg, __COUNTER__ __VA_OPT__(,) __VA_ARGS__)

/**
 * @brief Halts emulation the instant this line executes. See this file's
 * own header comment. Always on, on NES, same as log() -- gating a debug
 * aid behind a build type would leave it silently absent from a release
 * ROM's actual behavior; see tools/pnes.lua for how emu.breakExecution()
 * is invoked once this call site's LMA is reached.
 */
#define pause() PNES_LOG_IMPL(PNES_LOG_KIND_PAUSE, "", __COUNTER__)

#define PNES_LOG_IMPL(kind, msg, id, ...)                                       \
    do {                                                                        \
        PNES_LOG_TEXT(PNES_LOG_CAT(_pnes_log_file_, id), __FILE__);             \
        PNES_LOG_TEXT(PNES_LOG_CAT(_pnes_log_msg_, id), msg);                   \
        static const ::log_detail::Arg PNES_LOG_CAT(_pnes_log_args_, id)[]      \
            __attribute__((section(".pnes_log"), used)) = {                     \
                __VA_OPT__(PNES_LOG_MAP(PNES_LOG_NARG(__VA_ARGS__), __VA_ARGS__),) \
                ::log_detail::Arg{ nullptr, 0, 0 }                              \
            };                                                                  \
        static const ::log_detail::Entry PNES_LOG_CAT(_pnes_log_entry_, id)     \
            __attribute__((section(".pnes_log"), used)) = {                     \
                PNES_LOG_CAT(_pnes_log_file_, id),                              \
                __LINE__,                                                       \
                PNES_LOG_CAT(_pnes_log_msg_, id),                               \
                PNES_LOG_CAT(_pnes_log_args_, id),                              \
                static_cast<u8>(PNES_LOG_NARG(__VA_ARGS__)),                    \
                static_cast<u8>(kind)                                          \
            };                                                                  \
    } while (0)

#elif !defined(NDEBUG)

#include <iostream>
#include <utility>
#include <type_traits>
#include "console.hpp"

namespace log_detail {

/// operator<< has no overload for an arbitrary scoped enum (eGameModes and
/// friends) -- print its underlying value instead, same information the
/// NES/Mesen side ends up showing (it never sees the enum type at all,
/// only ::MakeArg's raw size/signedness). A byte-sized integral (u8/i8,
/// or an enum with one as its underlying type -- exactly eGameModes'
/// shape) is further promoted to int/unsigned, since operator<< treats
/// (unsigned) char as a character to print, not a number to format.
template <typename T>
constexpr auto LogPrintable(T &&value) {
    using Raw = std::remove_cvref_t<T>;
    using Underlying = std::conditional_t<std::is_enum_v<Raw>, std::underlying_type_t<Raw>, Raw>;
    if constexpr (std::is_integral_v<Underlying> && sizeof(Underlying) == 1) {
        using Promoted = std::conditional_t<std::is_signed_v<Underlying>, int, unsigned>;
        return static_cast<Promoted>(static_cast<Underlying>(value));
    } else if constexpr (std::is_enum_v<Raw>) {
        return static_cast<Underlying>(value);
    } else {
        return static_cast<Raw>(value);
    }
}

/// Base case: no arguments left -- print whatever's left of the format
/// string verbatim (including a stray "%d" if the caller passed fewer
/// arguments than format specifiers -- a caller bug, not something to hide).
inline void LogFormat(const char *fmt) {
    std::cout << fmt;
}

/// Off NES there's a real runtime and real values in hand, so interpolation
/// just happens immediately via operator<< -- no address/size/signedness
/// capture needed (contrast ::MakeArg on the NES side, where nothing can be
/// read until Mesen gets around to it). %x/%X get hex formatting to match
/// what tools/pnes.lua produces for the same specifier on the NES side;
/// everything else (%d, %u, ...) is just the value's own default formatting.
template <typename T, typename... Rest>
void LogFormat(const char *fmt, T &&value, Rest &&...rest) {
    while (*fmt) {
        if (fmt[0] == '%' && fmt[1] != '\0') {
            const char spec = fmt[1];
            auto printable = LogPrintable(std::forward<T>(value));
            if (spec == 'x') std::cout << std::hex << printable << std::dec;
            else if (spec == 'X') std::cout << std::uppercase << std::hex << printable << std::nouppercase << std::dec;
            else std::cout << printable;
            LogFormat(fmt + 2, std::forward<Rest>(rest)...);
            return;
        }
        std::cout << *fmt++;
    }
}

} // namespace log_detail

/// Off NES, debug builds only -- log() is gated behind DEBUG entirely here,
/// not just the console-creation fallback: a release build must never pay
/// for (or emit) this, matching log()'s NES side always costing nothing.
/// No EnsureConsoleOnce() call here: that runs once, unconditionally, from
/// ::RESET's own expansion (interrupts.hpp) at actual program startup --
/// not lazily from the first log() call, so a console is guaranteed to
/// exist by the time ANY log() call runs, even a run that only calls it
/// once, deep into the game.
/// "log: " matches tools/pnes.lua's own prefix for a log() message on the
/// NES/Mesen side -- there's no "app: " counterpart here, since there's no
/// separate loader/tool step on this path to have diagnostics of its own.
#define log(msg, ...) \
    ((std::cout << "log: "), ::log_detail::LogFormat(msg __VA_OPT__(,) __VA_ARGS__), (std::cout << '\n'))

/// Off NES, debug builds only, mirroring log()'s own gating -- prints a
/// notice and blocks for a single keypress via ::tech::WaitForKeypress()
/// (console.hpp), which puts the terminal in raw mode so Enter isn't
/// required. NOTE: WaitForKeypress() reads real stdin, which is NOT the
/// synthesized terminal ::tech::EnsureConsoleOnce() may have spawned for
/// stdOUT -- that terminal's `tail -f` isn't interactive. pause() therefore
/// only works as intended when this process was launched from a real
/// terminal to begin with; otherwise it blocks on whatever the original
/// (non-interactive) stdin actually is.
#define pause() \
    ((std::cout << "log: pause() hit at " << __FILE__ << ":" << __LINE__ \
                 << " -- press any key to continue...\n" << std::flush), \
     ::tech::WaitForKeypress())

#else

/// Off NES, release build: log()/pause() cost nothing and do nothing, same
/// as the NES side always does.
#define log(msg, ...) ((void)0)
#define pause() ((void)0)

#endif
