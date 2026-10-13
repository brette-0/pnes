/**
 * @file internal.hpp
 * @brief Shared declarations for the Sony PlayStation 3 backend (PSL1GHT,
 *        RSX-mapped framebuffer).
 *
 * Like the PSP/Switch/Wii U backends, the PS3 presents the shared software PPU
 * core (src/emu) rather than driving native tile hardware -- the Cell PPU is
 * far more than enough for a 256x240 NES frame, so ::emu::GenerateFrame
 * rasterises into a staging buffer and the CPU integer-scale-blits that into
 * an RSX-mapped display buffer which gcmSetFlip() then presents. No part of
 * the RSX 3D pipeline is used: no vertex/fragment programs (so no cgcomp
 * step), no textured quad, no gcmSurface. See src/ps3/video.cpp for why a
 * first pass skips all of it -- the same call src/psp/video.cpp documents for
 * the GE and the Switch backend documents for deko3d.
 *
 * CMake sets TARGET_PS3 (the engine headers gate on it alongside TARGET_PS2 /
 * TARGET_PSP: emulated PPU, NES-native 32x30 viewport, no SDL headers). The
 * console's video out runs at a real 50/60Hz; frame pacing comes from waiting
 * on gcmGetFlipStatus() after requesting the flip.
 */
#ifndef PS3_INTERNAL_H
#define PS3_INTERNAL_H

// Must precede every engine header: ppu-types.h's u8..s64 typedefs have to be
// visible before the intsh shim's `using namespace br0::intsh` would otherwise
// introduce a second unqualified spelling of them. Same ordering requirement
// src/ps2/internal.hpp has for tamtypes.h.
#include <ppu-types.h>

#include <intsh>
using namespace br0::intsh;

/** @brief Application-defined NMI handler (the per-frame VBlank callback). */
extern void nmi_vector();
/** @brief Quit flag; defined in the shared core (src/emu/ppu.cpp). */
extern int quit;

/** @brief Registers the XMB exit/menu callback (called once from video init()). */
void ps3_setup_exit_callback();

/** @brief Pumps the registered sysutil callbacks; called once per presented frame. */
void ps3_poll_sysutil();

/** @brief Initialises PSL1GHT controller sampling (called once from video init()). */
void input_init();

/** @brief Releases the controller sampling (called from irq::post()). */
void input_exit();

/**
 * @brief Writes one line straight to lv2's TTY channel 0 (bring-up aid).
 *
 * Unconditional -- NOT gated on NDEBUG, unlike log() (logger.hpp) -- because
 * the thing it exists to diagnose is a release build that does not reach a
 * first frame. It is gated on PS3_BOOT_TRACE instead, which the ps3 branch of
 * CMakeLists.txt defines for both presets while this backend is being brought
 * up on real hardware/RPCS3; configure with -DPS3_BOOT_TRACE=0 to compile it
 * out entirely.
 *
 * sysTtyWrite (sys/tty.h) is a static-inline lv2 syscall -- no library, no
 * stdio, no stream objects, nothing that needs initialising first. That is the
 * whole point: this has to be usable BEFORE and INSTEAD of everything whose
 * initialisation it is reporting on, exactly as src/ps2/audio.cpp's sio_puts
 * path is on the EE. RPCS3 surfaces channel 0 in its TTY log; on hardware it
 * is what ps3load's console captures.
 */
#if defined(PS3_BOOT_TRACE) && PS3_BOOT_TRACE
#  include <sys/tty.h>
#  include <cstring>
inline void ps3_trace(const char *msg) {
    u32 written = 0;
    sysTtyWrite(0, "pnes: ", 6, &written);
    sysTtyWrite(0, msg, static_cast<u32>(std::strlen(msg)), &written);
    sysTtyWrite(0, "\n", 1, &written);
}
#else
inline void ps3_trace(const char *) {}
#endif

#endif // PS3_INTERNAL_H
