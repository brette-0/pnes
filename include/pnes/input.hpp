/**
 * @file input.hpp
 * @brief Controller polling and button bit definitions.
 *
 * The NES exposes two standard controllers at MMIO ports \$4016 and
 * \$4017; this header defines the addresses as ::IO_PORT1 and
 * ::IO_PORT2 plus a portable polling helper that works on both
 * targets. Each poll returns a byte whose bits correspond to the
 * ::Buttons enumeration.
 */
#pragma once

#include <intsh>
using namespace br0::intsh;
#ifdef TARGET_NES
#ifndef IO_PORT1
/** @brief Memory-mapped register for controller 1 (NES \$4016). */
#define IO_PORT1 *(volatile u8*)0x4016
/** @brief Memory-mapped register for controller 2 (NES \$4017). */
#define IO_PORT2 *(volatile u8*)0x4017
#endif
#endif

namespace input {

/**
 * @brief Bit masks for the eight standard NES controller buttons.
 *
 * The values are chosen to match the NES hardware shift order, so they
 * can be OR'd directly into a port byte and tested against the
 * results of ::input::PollControllers.
 */
enum Buttons {
    A       = 0x01, /**< A button. */
    B       = 0x02, /**< B button. */
    SELECT  = 0x04, /**< Select button. */
    START   = 0x08, /**< Start button. */
    UP      = 0x10, /**< D-pad up. */
    DOWN    = 0x20, /**< D-pad down. */
    LEFT    = 0x40, /**< D-pad left. */
    RIGHT   = 0x80, /**< D-pad right. */
};

/**
 * @brief Reads the current state of both controllers.
 *
 * Strobes \$4016 and serially clocks out eight bits per controller,
 * writing the packed results to @p port1 and @p port2. On desktop
 * builds the same function returns the mapped keyboard / gamepad
 * state in the same bit layout.
 *
 * @param[out] port1 Receives the button mask for controller 1.
 * @param[out] port2 Receives the button mask for controller 2.
 */
void PollControllers(u8* port1, u8* port2);

} // namespace input