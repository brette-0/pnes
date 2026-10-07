/**
 * @file internal.hpp
 * @brief Shared declarations for the Sony PlayStation 2 backend (ps2sdk,
 *        gsKit).
 *
 * Like the Switch/Wii U/GameCube backends, the PS2 presents the shared
 * software PPU core (src/emu) through a real GPU rather than driving any
 * native tile hardware: ::emu::GenerateFrame rasterises into an ARGB8888
 * staging buffer in EE RAM, which gsKit DMAs into GS VRAM as a texture and
 * draws as a single scaled quad (see src/ps2/video.cpp for why that's a
 * better fit here than either GX's per-tile CI4 atlas or PSP's raw-VRAM
 * write). No GS primitive other than that one textured quad is used.
 *
 * CMake sets TARGET_PS2 and _EE (the ps2sdk/gsKit convention every EE-side
 * header requires). The engine headers gate on TARGET_PS2 alongside every
 * other non-NES target (emulated PPU, no SDL headers) -- see video.hpp's
 * TARGET_PS2 branch for the viewport geometry and its own file header
 * comment for why intsh's u8/u16/u32/u64 aliases are skipped here.
 *
 * This header includes <tamtypes.h> FIRST, ahead of any engine header: that
 * is what makes the TARGET_PS2 guard on every `using namespace br0::intsh;`
 * (video.hpp and friends) correct -- ps2sdk's own u8/u16/u32/u64/s8/s16/s32/
 * s64 typedefs need to already be the only ones in scope by the time those
 * headers are parsed, not fetched afterward.
 */
#ifndef PS2_INTERNAL_H
#define PS2_INTERNAL_H

#ifndef _EE
#define _EE
#endif
#include <tamtypes.h>

/** @brief Application-defined NMI handler (the per-frame VBlank callback). */
extern void nmi_vector();
/** @brief Quit flag; defined in the shared core (src/emu/ppu.cpp). */
extern int quit;

/** @brief Initialises PS2 controller sampling (called once from video init()). */
void input_init();

#endif // PS2_INTERNAL_H
