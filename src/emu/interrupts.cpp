#include <pnes/interrupts.hpp>
#include <cstdlib>

namespace irq {

irq_handler_fn irqHandler;
vec2<u16>      irqPosition;
bool           irqPendingValid;

} // namespace irq

/**
 * @brief Weak default definition of the application's ::IRQ entry point.
 *
 * Library code that schedules a real interrupt (::mmc3::ScheduleScanlineIRQ)
 * references ::irq_vector unconditionally, so any non-NES app that links
 * that code needs the symbol to resolve even if it never defines its own
 * `IRQ() { ... }` handler (e.g. a game that doesn't use MMC3's scanline
 * IRQ). This no-op stub is silently overridden by the application's own
 * strong definition whenever one exists.
 */
__attribute__((weak)) void irq_vector() {}

namespace irq {

void reset() {
    post();
    exit(0);
}

} // namespace irq
