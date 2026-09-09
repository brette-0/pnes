#pragma once
#include <platform-nes/platform-nes.hpp>

#ifdef TARGET_NES
// See logger.hpp for what this configures. Both attributes are required, not
// just `used`: nothing on NES ever reads this, so a real link's --gc-sections
// pass drops it anyway unless it's KEEP()'d -- reusing .pnes_log (see
// src/nes/mappers/debug-log.ld) gets that for free, matching logger.cpp's own
// default definition's placement.
//
// NOT constexpr, despite the constant initializer (IDEs will suggest it):
// lua_log_builder reads this byte back out of the linked .elf's storage --
// constexpr tells the compiler no runtime object is ever needed, which is
// exactly the reasoning gc-sections/LTO would use to discard it again, used
// and section() notwithstanding.
//
// TARGET_NES-only: logger.hpp's own extern declaration (what makes this an
// override rather than a stray unrelated global) only exists under
// TARGET_NES, and .pnes_log/debug-log.ld's KEEP() rule are NES-specific --
// on another platform this would just be a pointless global sitting in a
// section name that means nothing to that platform's linker.
inline const float silentHeapAmount  __attribute__((used, section(".pnes_log"))) = 0.0f;
inline const float silentStackAmount __attribute__((used, section(".pnes_log"))) = 0.0f;

#endif

enum class eGameModes : u8 {
    Title,  // title screen
    World,  // world map, level select
    Level,  // gameplay, platforming in level
};

/** @brief Viewport width in metatiles (tiles / 2). */
constexpr u8 viewport_mx() { return video::viewport_tx() >> 1; }
/** @brief Viewport height in metatiles (tiles / 2). */
constexpr u8 viewport_my() { return video::viewport_ty() >> 1; }

extern atomic eGameModes gameMode;

// Mode-dispatched NMI/IRQ entry points: main.cpp's nmiTrampoline/irqTrampoline
// (the only functions actually pinned to the hardware vectors) call through
// these once per interrupt. The active mode's setup path points them at its
// own handlers before enabling interrupts. Plain function pointers, called
// with ordinary C++ call syntax from the trampolines -- never reached by raw
// asm symbol text -- so, unlike the trampolines themselves, neither these nor
// the handlers they point at need C linkage or any special attribute. Left
// uninitialized (zero) so both land in BSS rather than .data.
extern void (*pNMI)();
extern void (*pIRQ)();

extern u8 scratchpad[];

// Shared OAM staging buffer -- every mode (title, level, ...) refreshes its
// own sprites into the same 64-sprite table rather than each owning one, so
// it lives here instead of any one mode's header.
extern oam::sprite_t OAMBuffer[64];
