/**
 * @file interrupts.hpp
 * @brief IRQ registration and dispatch.
 *
 * On NES the application defines one hardware IRQ handler with ::IRQ, placed at
 * the vector like ::NMI, with no runtime arming. Off NES there is no vector, so
 * the renderer holds a function pointer in ::irqPending and drains it once per
 * frame.
 */
#pragma once
#include <intsh>
using namespace br0::intsh;
#include <cstddef>
#include "types.hpp"

/**
 * @brief Tags a function as an interrupt entry point.
 *
 * On NES: `extern "C"` keeps the symbol name stable for the linker script to
 * place at the vector, `used` stops LTO discarding a function only the vector
 * table references, and `interrupt_norecurse` emits the save/restore prologue
 * and epilogue ending in RTI. Off NES it is just `void`.
 *
 * Use in place of a return type. ::NMI and ::IRQ expand to the same set, pinned
 * to the two vector symbol names:
 *
 * @code
 *   interrupt MyHandler() {
 *     // full imaginary-register save/restore, ending in RTI
 *   }
 * @endcode
 */
#ifdef TARGET_NES
#define interrupt ASM_LINKAGE __attribute__((used, interrupt_norecurse)) void
#else
#define interrupt void
#endif

namespace irq {

#ifndef TARGET_NES

/** @brief Signature of an IRQ handler (no arguments, no return value). */
typedef void (*irq_handler_fn)();

/**
 * @brief Handler for the single pending IRQ event (desktop only).
 *
 * Callers write this slot directly to arm the next scanline IRQ --
 * overwriting it is a set, not an enqueue, so only one IRQ can be pending
 * at a time. Set alongside ::irqPosition and ::irqPendingValid. The
 * renderer fires it at ::irqPosition and clears ::irqPendingValid.
 */
extern irq_handler_fn irqHandler;
/** @brief Pixel coordinate at which ::irqHandler should fire (desktop only). */
extern vec2<u16>      irqPosition;
/** @brief Non-zero when ::irq::irqHandler / ::irq::irqPosition hold a valid event. */
extern bool           irqPendingValid;

#endif

} // namespace irq


#ifdef TARGET_NES

#ifdef NES_MAPPER_BANKSWITCHED
/**
 * @brief Declares the program's reset handler on bankswitched NES builds.
 *
 * Expands to `int main()`, invoked by crt0 at cold boot, pinned to
 * `.prg_rom_fixed`: main must stay mapped whatever any switchable window holds,
 * since everything including the bankswitching runs from it.
 */
#define RESET __attribute__((section(".prg_rom_fixed"))) int main()
#else
/**
 * @brief Declares the program's reset handler on NES builds.
 *
 * Expands to `int main()`, invoked by crt0 at cold boot. NROM has no banks, so
 * unlike the bankswitched variant main is not pinned to a section.
 */
#define RESET int main()
#endif

namespace irq {
inline void EnableInterrupts()  { __asm__ volatile ("cli"); }
inline void DisableInterrupts() { __asm__ volatile ("sei"); }

/**
 * @brief Set true by every ::NMI vector right after its body returns; cleared
 *        by ::video::WaitForPresent once it's done spinning on it.
 *
 * Lets ::video::WaitForPresent give NES the same "block until the next
 * VBlank has been presented" semantics it already has on desktop, without
 * every mode reimplementing its own done-flag and its own
 * `#ifdef TARGET_NES` spin-wait around a plain ::video::WaitForPresent call.
 */
extern volatile bool nmi_done;
} // namespace irq

/**
 * @brief Declares the NMI handler on NES builds.
 *
 * Sits at the hardware vector by raw symbol name: no indirection, no runtime
 * rearming, one handler chosen at compile time.
 *
 * The optional attribute-specifier-seq is spliced after the `extern "C"` this
 * expands to. That position is required -- a leading attribute is only valid at
 * the start of an ordinary declaration, and this is a linkage-specification.
 *
 * @code
 *   NMI()             { ... }              // falls through; RTI returns
 *   NMI(FIXED)        { ... }              // and pinned to an always-mapped bank
 *   NMI([[noreturn]]) { for (;;) { ... } } // never falls off the end
 * @endcode
 *
 * PLACEMENT MATTERS: an interrupt arrives when nothing controls what is mapped,
 * so a handler in a switchable window is correct only while nobody banks that
 * window. Which section is always mapped is a project's layout decision, hence
 * the composable argument. A project that banks nothing can omit it.
 *
 * `[[noreturn]]` is a lie unless the body truly never reaches its closing brace.
 *
 * @note Unverified whether llvm-mos drops the RTI epilogue for `[[noreturn]]`.
 *       For a guaranteed empty epilogue use ::NAKED_NMI / ::NAKED_IRQ.
 * @note The C++ function is `nmi_vector`; `asm("nmi")` restores the symbol.
 * @note The body supplied after this macro doesn't become `nmi_vector`
 *       itself -- it becomes a `static` function (`nmi_body`) that the real
 *       vector calls before unconditionally setting ::irq::nmi_done, so every
 *       project's NMI marks the flag for free (see ::irq::nmi_done). The
 *       same `__VA_ARGS__` placement attributes apply to both, so a body too
 *       large to inline into `nmi_vector` still lands in the guaranteed bank.
 */
#define NMI(...)                                                          \
ASM_LINKAGE __VA_ARGS__ void nmi_vector() asm("nmi");                     \
static __VA_ARGS__ void nmi_body();                                       \
ASM_LINKAGE __VA_ARGS__ __attribute__((used, interrupt_norecurse))        \
void nmi_vector() { nmi_body(); irq::nmi_done = true; }                   \
static __VA_ARGS__ void nmi_body()

/**
 * @brief Declares the IRQ handler on NES builds.
 *
 * Same shape as ::NMI, at the IRQ vector. Exactly one handler, chosen at compile
 * time: every enabled IRQ source vectors here, and the handler tells them apart
 * if more than one is active. Composes with attributes like ::NMI does.
 *
 * @note Named `irq_vector` because the `::irq` namespace already holds `irq` at
 *       global scope; `asm("irq")` restores the symbol the vector expects.
 */
#define IRQ(...)                                                          \
ASM_LINKAGE __VA_ARGS__ void irq_vector() asm("irq");                     \
ASM_LINKAGE __VA_ARGS__ __attribute__((used, interrupt_norecurse))        \
void irq_vector()

/**
 * @brief Declares the NMI handler on NES builds with zero compiler-generated
 *        interrupt bookkeeping.
 *
 * Same vector placement as ::NMI but `naked`: no prologue, epilogue or RTI, so
 * the body's asm is verbatim what lands at the vector. The price is that the
 * body may contain only `asm` (::JUMP / ::JUMP_INDIRECT qualify) and owns its
 * own housekeeping unless it tail-jumps somewhere that handles it.
 *
 * @code
 *   interrupt nmiHandler() { ... }   // real logic, own save/restore + RTI
 *   NAKED_NMI { JUMP(nmiHandler); }
 * @endcode
 *
 * Prefer plain ::NMI unless you need the guarantee of no generated code.
 */
#define NAKED_NMI                                            \
ASM_LINKAGE void nmi_vector() asm("nmi");                     \
ASM_LINKAGE __attribute__((naked, used))                      \
void nmi_vector()

/**
 * @brief Declares the IRQ handler on NES builds with zero compiler-generated
 *        interrupt bookkeeping.
 *
 * To ::IRQ what ::NAKED_NMI is to ::NMI: `naked`, pure-`asm` body, same
 * tail-jump-into-a-real-handler use.
 */
#define NAKED_IRQ                                            \
ASM_LINKAGE void irq_vector() asm("irq");                     \
ASM_LINKAGE __attribute__((naked, used))                      \
void irq_vector()

/**
 * @brief Direct unconditional jump to `target`, a function (a fixed,
 *        link-time-known entry point, e.g. another ::interrupt-tagged
 *        handler). Never falls through.
 *
 * A single `asm` statement, no wrapper -- so it is usable anywhere plain `asm`
 * is, including a `naked` body:
 *
 * @code
 *   NAKED_NMI { JUMP(nmiHandler); }
 * @endcode
 */
#define JUMP(target) __asm__ volatile ("jmp " #target)

/**
 * @brief Indirect unconditional jump through `target`, a `void(*)()`-typed
 *        variable holding a runtime-chosen entry point (e.g. ::irqTrampoline
 *        elsewhere). Never falls through.
 *
 * Same single-statement shape as ::JUMP, through the variable's contents rather
 * than a fixed address. Also usable in a `naked` body.
 */
#define JUMP_INDIRECT(target) __asm__ volatile ("jmp (" #target ")")

#else

#include "audio.hpp"
#include "console.hpp"

/**
 * @brief Library-side startup hook, called before user code on desktop builds.
 *
 * Invoked by the expansion of ::RESET. Initialises SDL subsystems,
 * opens the window, and prepares the audio mixer.
 */
namespace irq {
extern void init();

/**
 * @brief Library-side teardown hook, called after user code returns.
 *
 * Invoked by the expansion of ::RESET. Closes the window, releases
 * audio devices, and shuts down SDL.
 */
extern void post();
} // namespace irq

/**
 * @brief Declares the program's reset handler on desktop builds.
 *
 * Expands to a `main` that calls ::init, runs the user's body, then
 * calls ::post. Follow the macro with the handler body — it becomes
 * the inline `usr_main` function.
 *
 * ::tech::EnsureConsoleOnce() (console.hpp) runs first, unconditionally --
 * not lazily from inside log() itself. Tying it to the first log() call
 * meant a run that happened not to call log() at all never got a console,
 * even on a launch that genuinely needed one; calling it here instead means
 * every debug-build run gets one exactly once, independent of whether the
 * game ever actually logs anything during it.
 *
 * @code
 *   RESET {
 *     // one-shot setup, then the main loop
 *   }
 * @endcode
 */
#define RESET                       \
static void usr_main();         \
int main(){                     \
::tech::EnsureConsoleOnce();    \
irq::init();         \
usr_main();                 \
irq::post();         \
}                               \
inline static void usr_main ()

/**
 * @brief Declares the NMI handler on desktop builds.
 *
 * The SDL3 renderer calls this once per simulated VBlank, matching
 * the NES NMI cadence so the same source works on both targets.
 *
 * @note Named `nmi_vector` to stay in lock-step with the NES macro.
 * @note Takes and discards the attribute argument, so `NMI(...)` is valid call
 *       syntax on every target.
 */
#define NMI(...)                \
void nmi_vector()

/**
 * @brief Declares the IRQ handler on non-NES builds.
 *
 * Non-NES equivalent of ::NMI. Not called by the renderer directly: it is the
 * single fixed entry point library code arms as ::irq::irqHandler for any real
 * interrupt source, mirroring the NES vector, with only ::irq::irqPosition
 * varying per source. Convenience handlers that bypass a real source arm their
 * own callback and never touch this.
 *
 * @note Named `irq_vector` to avoid the `::irq` namespace; attribute argument
 *       taken and discarded, as with ::NMI above.
 */
#define IRQ(...)                \
void irq_vector()

/**
 * @brief Forward declaration of the application's ::IRQ handler (non-NES).
 *
 * Lets library code that schedules a real interrupt (e.g.
 * ::mmc3::ScheduleScanlineIRQ) arm ::irq::irqHandler with this fixed
 * entry point without depending on the application's translation unit.
 * Defined once by the application's own `IRQ() { ... }`, the same way
 * every backend's `extern void nmi_vector();` pairs with the
 * application's `NMI() { ... }`.
 */
extern void irq_vector();

/**
 * @brief Non-NES equivalent of ::NAKED_NMI.
 *
 * No vector and no naked-body restriction off NES, so this collapses to ::NMI.
 * Exists so a shared trampoline compiles unchanged on every target.
 */
#define NAKED_NMI               \
void nmi_vector()

/**
 * @brief Non-NES equivalent of ::NAKED_IRQ. See ::NAKED_NMI.
 */
#define NAKED_IRQ               \
void irq_vector()

/**
 * @brief Non-NES equivalent of ::JUMP: an ordinary tail call.
 *
 * Just `target();` -- there is no single-`asm` body restriction off NES.
 */
#define JUMP(target) target()

/**
 * @brief Non-NES equivalent of ::JUMP_INDIRECT: an ordinary call through
 *        a function-pointer variable.
 */
#define JUMP_INDIRECT(target) (target)()

/**
 * @brief Enables the CPU interrupt line — a no-op off NES.
 *
 * @note Off-NES backends dispatch their scanline IRQ synchronously from the
 * renderer, so there is no interrupt line to mask. Declared so a ::RESET body
 * written once compiles unchanged on every target.
 */
namespace irq {
inline void EnableInterrupts()  {}

/**
 * @brief Disables the CPU interrupt line — a no-op off NES.
 *
 * @note This function does nothing on non-NES targets; see ::irq::EnableInterrupts.
 */
inline void DisableInterrupts() {}
} // namespace irq

#endif

namespace irq {
/**
 * @brief Soft-resets the application.
 *
 * On NES re-enters through the hardware reset vector, the cold-boot entry
 * point. Off NES runs ::irq::post and exits. One call site, no `#ifdef`.
 */
void reset();
} // namespace irq