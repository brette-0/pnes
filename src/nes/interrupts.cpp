#include <pnes/interrupts.hpp>
#include <pnes/technology.hpp>

namespace irq {

volatile bool nmi_done;

void reset() {
    /* TODO: banked ROM needs far-call reset */
    __asm__ ("jmp ($fffc)");
}

} // namespace irq
