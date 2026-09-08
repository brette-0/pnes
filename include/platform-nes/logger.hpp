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
 * tools/logger.lua then prints the message in Mesen whenever the CPU
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

namespace log_detail {
/**
 * @brief One log() call site's compiler-generated metadata.
 *
 * Never read at runtime -- the host-side `logger` tool is the only reader,
 * and only after the build, straight out of the linked .elf's .pnes_log
 * section. `file` and `message` point at sibling .pnes_log data, never at
 * .rodata: see ::PNES_LOG_TEXT for why that distinction matters.
 */
struct Entry {
    const char *file;
    u32 line;
    const char *message;
};
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

/**
 * @brief Zero-footprint log call site. See this file's own header comment.
 *
 * __COUNTER__ is captured once via the id parameter (expanded as an
 * argument, then pasted three times) so all three declarations below name
 * the same call site consistently -- referencing __COUNTER__ directly three
 * times would yield three different values.
 */
#define log(msg) PNES_LOG_IMPL(msg, __COUNTER__)
#define PNES_LOG_IMPL(msg, id)                                               \
    do {                                                                     \
        PNES_LOG_TEXT(PNES_LOG_CAT(_pnes_log_file_, id), __FILE__);          \
        PNES_LOG_TEXT(PNES_LOG_CAT(_pnes_log_msg_, id), msg);                \
        static const ::log_detail::Entry PNES_LOG_CAT(_pnes_log_entry_, id)  \
            __attribute__((section(".pnes_log"), used)) = {                  \
                PNES_LOG_CAT(_pnes_log_file_, id),                           \
                __LINE__,                                                    \
                PNES_LOG_CAT(_pnes_log_msg_, id)                             \
            };                                                               \
    } while (0)

#elif !defined(NDEBUG)

#include <iostream>

/// Off NES, debug builds only -- log() is gated behind DEBUG entirely here,
/// not just the console-creation fallback: a release build must never pay
/// for (or emit) this, matching log()'s NES side always costing nothing.
/// No EnsureConsoleOnce() call here: that runs once, unconditionally, from
/// ::RESET's own expansion (interrupts.hpp) at actual program startup --
/// not lazily from the first log() call, so a console is guaranteed to
/// exist by the time ANY log() call runs, even a run that only calls it
/// once, deep into the game.
/// "log: " matches tools/logger.lua's own prefix for a log() message on the
/// NES/Mesen side -- there's no "app: " counterpart here, since there's no
/// separate loader/tool step on this path to have diagnostics of its own.
#define log(msg) (std::cout << "log: " << (msg) << '\n')

#else

/// Off NES, release build: log() costs nothing and prints nothing, same as
/// the NES side always does.
#define log(msg) ((void)0)

#endif
