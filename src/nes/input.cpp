#include <intsh>
using namespace br0::intsh;
#include <pnes/input.hpp>
#include <pnes/technology.hpp>

// NO PLACEMENT KNOB, deliberately. This module is one ~40-byte function, so
// there is nothing worth moving out of a bank, and out-of-lining it would cost
// argument marshalling plus jsr/rts on a call a game makes every frame. ::AI
// below pins it into its callers instead, which also keeps it reachable from
// whatever bank the caller happens to be in.

// Scratch bytes for the shift-register read below. Named with C linkage so the
// inline asm can address them directly by symbol instead of through a runtime
// pointer (ROR has no (zp),Y addressing mode, so the running accumulator can't
// live at *port1/*port2 directly -- those are only reachable indirectly).
// `used` is load-bearing: the asm block below has an empty operand list, so
// as far as LLVM's IR is concerned nothing ever references these globals --
// the only "use" is the literal symbol name inside opaque asm text, which
// ordinary use-analysis can't see. Without `used`, LTO's dead-global
// elimination strips both of these before they even make it into
// libpnes.a, and the final link fails with "undefined symbol"
// (verified: they're absent from the .a's symbol table without this
// attribute). Same fix this codebase already relies on for the NMI/IRQ vector
// functions in interrupts.hpp, which are likewise only reachable by a path
// (the raw hardware vector table) invisible to the compiler.
extern "C" {
__attribute__((used)) u8 pollScratch1;
__attribute__((used)) u8 pollScratch2;
}

// Classic NESdev shift-register read: each of the 8 clocks moves the new bit
// into the top of the byte and everything else down one, so after exactly 8
// rotations the scratch byte is fully replaced by the 8 new bits -- no need to
// zero it first, and no separate shift/mask/OR-into-memory per bit. This
// replaces a fully-unrolled loop that did a load+shift+and+load+or+store
// (through *port1/*port2 each iteration) with a tight 8-cycle loop body that
// does the accumulation with a single RMW rotate per controller.
AI void input::PollControllers(u8* port1, u8* port2) {
    IO_PORT1 = 1;
    IO_PORT1 = 0;

    __asm__ volatile (
        "ldx #8\n"
        "1:\n"
        "lda $4016\n"
        "lsr a\n"
        "ror pollScratch1\n"
        "lda $4017\n"
        "lsr a\n"
        "ror pollScratch2\n"
        "dex\n"
        "bne 1b\n"
        :
        :
        : "a", "x", "c", "memory"
    );

    *port1 = pollScratch1;
    *port2 = pollScratch2;
}
