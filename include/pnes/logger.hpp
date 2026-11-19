/**
 * @file logger.hpp
 * @brief Zero-footprint NES logging.
 *
 * log("message") costs one instruction on TARGET_NES -- a single `nop`,
 * which exists so the call site has an address of its own for the host-side
 * tooling to find (see ::PNES_LOG_ANCHOR for what goes wrong without it,
 * and how to opt back out). Everything else it emits is free: it declares
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

// Fraction (0.0-1.0) of that region's total size below which
// tools/pnes.lua's heap/stack trackers stay quiet -- e.g. 0.25 warns past a
// quarter of the heap. lua_log_builder converts this to an absolute byte
// count once, at build time; nothing on NES itself ever reads either one.
extern const float silentHeapAmount;
extern const float silentStackAmount;

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

/**
 * @brief The one real instruction a log call site emits: its anchor.
 *
 * WITHOUT THIS, A LOG POINT HAS NO ADDRESS OF ITS OWN. The metadata below
 * records (file, line) and nothing else, so tools/luaLogBuilder has to find
 * the call site's address in the .elf's DWARF line table -- and a statement
 * that emits no instructions gets no line-table row. The builder then
 * borrows the nearest row at a LATER line, which fails in both directions,
 * and both failures were observed in this project's own demo:
 *
 *  - A log sitting just above a loop borrowed the row belonging to the
 *    loop's first instruction, so a once-at-boot message printed on every
 *    iteration, forever -- reporting a place control had already left.
 *  - The three logs inside that loop's switch arms had no later row in the
 *    file at all (the optimiser funnels every arm through one shared
 *    trampoline, whose rows sit at earlier lines), so they resolved to
 *    nothing and were dropped with a build warning. A log point that cannot
 *    fire is worse than none: its silence reads as evidence.
 *
 * One `nop` fixes both: the line table gets a row at exactly this line, the
 * builder's exact-match path always wins, and the address it resolves to is
 * an instruction that exists only to mark this call site -- so reaching it
 * means reaching the log, and nothing else does. `volatile` is what keeps
 * it: the compiler must not delete it, duplicate it, or hoist it out of a
 * loop, and it may not reorder it past another volatile asm or a memory
 * access. It cannot move across the statements that matter either, which is
 * the whole point of putting it here rather than trusting line attribution.
 *
 * THE COST IS ONE BYTE AND TWO CYCLES PER CALL SITE, which is no longer
 * "zero footprint" (see this file's header comment) -- the .pnes_log
 * metadata still costs nothing, but the anchor is real ROM. Define
 * ::PNES_LOG_NO_ANCHOR to get the old behaviour back: zero bytes, and log
 * positions that are approximate at best and silently absent at worst.
 */
#ifdef PNES_LOG_NO_ANCHOR
    #define PNES_LOG_ANCHOR() ((void)0)
#else
    #define PNES_LOG_ANCHOR() __asm__ volatile("nop")
#endif

#define PNES_LOG_IMPL(kind, msg, id, ...)                                       \
    do {                                                                        \
        PNES_LOG_ANCHOR();                                                      \
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

#elif defined(TARGET_PS2) && !defined(NDEBUG)

// ps2sdk's EE side has no hosted libc stdio behind it (no filesystem, no
// attached terminal) -- <iostream>'s global static-init (the real
// std::cout's ios_base::Init object) jumped through an unpopulated pointer
// and crashed the EE before main() ever ran when this used the generic
// branch below, so this never includes <iostream> at all. sio_init/sio_puts
// (ps2sdk's own low-level serial driver, <sio.h>) write straight to the EE's
// real serial port hardware instead -- no stream objects, no locale
// globals, nothing but the port itself -- which is exactly what PCSX2's "EE
// SIO output" logging option (and a real serial cable on hardware) reads.
// Formatting reuses plain snprintf (libc's buffer formatter, same reasoning:
// no stream objects) straight into a fixed buffer, so a caller passes
// ordinary printf-style specifiers (%d/%x/%u/...) matching its own argument
// types -- same contract as printf itself, unlike the generic LogFormat
// below, which auto-formats an arbitrary type through operator<<.
#include <sio.h>
#include <cstdio>

namespace log_detail {
/// Lazy, not tied to RESET/EnsureConsoleOnce: sio_puts() needs the port
/// configured first, but unlike the desktop console there's no "more
/// expensive to create later" concern -- it's just register writes.
inline void EnsureSioOnce() {
    static bool done = false;
    if (done) return;
    done = true;
    sio_init(38400, 0, 0, 0, 0);
}
} // namespace log_detail

#define log(msg, ...)                                                 \
    do {                                                              \
        ::log_detail::EnsureSioOnce();                                \
        char _pnes_sio_buf[192];                                      \
        std::snprintf(_pnes_sio_buf, sizeof(_pnes_sio_buf),           \
                      "log: " msg __VA_OPT__(,) __VA_ARGS__);         \
        sio_puts(_pnes_sio_buf);                                      \
    } while (0)

/// No interactive stdin on PS2 to block on (contrast
/// ::tech::WaitForKeypress's POSIX/Windows implementations) -- logs that
/// this was hit and carries straight on, rather than hanging forever.
#define pause()                                                       \
    do {                                                              \
        ::log_detail::EnsureSioOnce();                                \
        sio_puts("log: pause() hit -- no stdin on PS2, continuing");  \
    } while (0)

#elif defined(TARGET_PS3) && !defined(NDEBUG)

// Same problem the PS2 branch above exists to avoid, and the same shape of
// answer. PSL1GHT's newlib has no filesystem and no terminal behind stdio, so
// the generic <iostream> branch below would run std::ios_base::Init's global
// constructor before main() against a libc that has nowhere to put the bytes
// -- which is exactly what crashed the EE and is why the PS2 branch was
// written. Rather than find out whether the PPU survives it, this takes the
// serial-port equivalent: sysTtyWrite (sys/tty.h) is a static-inline lv2
// syscall, so there is no library to link, no stream object, no locale
// globals, nothing that has to be initialised before the first log() call.
// RPCS3 surfaces channel 0 in its TTY log; on hardware it is what ps3load's
// console captures.
//
// Formatting reuses snprintf into a fixed buffer for the same reason the PS2
// branch does, which means a caller passes ordinary printf-style specifiers
// (%d/%x/%u/...) matching its own argument types -- the same contract as
// printf, unlike the generic LogFormat below, which auto-formats an arbitrary
// type through operator<<.
//
// There is no EnsureSioOnce() counterpart: unlike the EE's serial port, the
// lv2 TTY needs no setting up before it is written to.
#include <sys/tty.h>
// <stdio.h>, and unqualified snprintf below, not <cstdio>/std::snprintf as the
// PS2 branch uses: ps3dev's newlib declares snprintf in the global namespace
// only, and this libstdc++'s <cstdio> does not re-export it into std, so the
// qualified spelling does not compile here at all.
#include <stdio.h>
#include <cstring>

namespace log_detail {
inline void TtyPuts(const char *msg) {
    u32 written = 0;
    sysTtyWrite(0, msg, static_cast<u32>(std::strlen(msg)), &written);
}
} // namespace log_detail

#define log(msg, ...)                                                 \
    do {                                                              \
        char _pnes_tty_buf[192];                                      \
        snprintf(_pnes_tty_buf, sizeof(_pnes_tty_buf),           \
                      "log: " msg "\n" __VA_OPT__(,) __VA_ARGS__);    \
        ::log_detail::TtyPuts(_pnes_tty_buf);                         \
    } while (0)

/// No interactive stdin to block on here (same as the PS2 branch) -- logs
/// that this was hit and carries straight on, rather than hanging forever.
#define pause()                                                       \
    ::log_detail::TtyPuts("log: pause() hit -- no stdin on PS3, continuing\n")

#elif defined(TARGET_CTR) && !defined(NDEBUG)

// The 3DS has nowhere for stdout to go, which is the same problem the PS2 and
// PS3 branches above exist to solve -- and, like them, the answer is whichever
// channel this console actually has. consoleInit() is not an option: it claims
// a whole screen's framebuffer, and both screens belong to citro2d here
// (src/3ds/video.cpp), so the generic <iostream> branch below would compile
// fine and write every message into the void.
//
// The SD card is what is left, and it is the better channel anyway for being
// the ONLY one that reads the same on hardware and under an emulator -- Azahar,
// Citra and Lime3DS all back the virtual SD with a real host directory, so
// `sdmc:/pnes.log` is a file on the developer's disk either way. (On hardware
// it is a file on the card, read after the fact rather than live; there is no
// terminal to tail on this platform, with or without this.)
//
// OPENED ONCE AND FLUSHED PER LINE, not reopened per message: these fire from
// the mode loops, and an open/close pair around each one is an FS round trip
// through a service call -- enough to change the timing of what is being
// diagnosed. Truncating ("w") rather than appending, so the file describes THIS
// run; a log that accumulates every run since the SD card was formatted is one
// nobody reads the top of.
//
// Formatting is snprintf-style, like the PS2/PS3 branches and unlike the
// generic one below: a caller passes ordinary printf specifiers matching its
// own argument types. Every log() call site in this project already does.
#include <cstdio>

namespace log_detail {
/// Lazy, so a run that never logs never creates the file. nullptr on failure
/// (no SD, or a read-only card), which every call site below tolerates.
inline FILE* CtrLogFile() {
    static FILE* f = std::fopen("sdmc:/pnes.log", "w");
    return f;
}
} // namespace log_detail

#define log(msg, ...)                                                 \
    do {                                                              \
        FILE* _pnes_ctr_log = ::log_detail::CtrLogFile();             \
        if (_pnes_ctr_log) {                                          \
            std::fprintf(_pnes_ctr_log,                               \
                         "log: " msg "\n" __VA_OPT__(,) __VA_ARGS__); \
            std::fflush(_pnes_ctr_log);                               \
        }                                                             \
    } while (0)

/// No interactive stdin on the 3DS to block on (same as the PS2/PS3 branches)
/// -- logs that this was hit and carries straight on, rather than hanging
/// forever. A real "press a button" wait would need hidScanInput on the render
/// thread, which is the thing a pause() is usually called to inspect.
#define pause()                                                       \
    log("pause() hit at %s:%d -- no stdin on 3DS, continuing", __FILE__, __LINE__)

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
/// ::std::underlying_type_t, but only ever INSTANTIATED for an enum.
///
/// NOT std::conditional_t<std::is_enum_v<T>, std::underlying_type_t<T>, T>,
/// which is what this replaced and which cannot work: conditional_t is an
/// ordinary template, so BOTH its branches are instantiated before the
/// condition picks one, and std::underlying_type<T> has no `type` member for a
/// non-enum T. The result was a hard error -- "no type named 'type' in
/// 'struct std::underlying_type<unsigned char>'", reported inside this header
/// rather than at the call site -- for any ::log argument that was a plain
/// byte. That went unnoticed because the only byte-sized values this project
/// had ever logged were scoped enums (eGameModes), and the generic-debug
/// branch this lives in is compiled out of a Release build (NDEBUG) -- so it
/// took a `log("%d", someU8)` in a Debug build off NES to surface it.
///
/// A partial specialisation defers the lookup: the primary template names no
/// underlying type at all, and only the `true` specialisation mentions it, so
/// nothing asks a non-enum for one.
template <typename T, bool = std::is_enum_v<T>>
struct UnderlyingOf { using type = T; };

template <typename T>
struct UnderlyingOf<T, true> { using type = std::underlying_type_t<T>; };

template <typename T>
constexpr auto LogPrintable(T &&value) {
    using Raw = std::remove_cvref_t<T>;
    using Underlying = typename UnderlyingOf<Raw>::type;
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
