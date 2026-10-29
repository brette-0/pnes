#pragma once
#include <pnes/pnes.hpp>

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

/**
 * @brief The sprite Y that parks a sprite off-screen on this target.
 *
 * Sprite Y is a literal output-space pixel row on every backend, so a row past
 * ::video::viewport_py() is off every panel the target has and the backend's own
 * clip drops it. On NES, OAM y is a byte and the frame is 240 lines, so 0xF0 is
 * the hardware's own unfetchable row -- the idiom this is the portable spelling
 * of.
 */
constexpr oam::oam_t ParkedSpriteY() {
#ifdef TARGET_NES
    return 0xf0;
#else
    return video::viewport_py();
#endif
}

/**
 * @brief Whether this target's gameplay panel is a window onto the game's frame.
 *
 * The one place the target set is named. ::VerticalWindow and ::WindowedSpriteY
 * only exist under it, and level.cpp gates its background half (ApplyHudSplit's
 * scroll) and the ::rasterPlayerY latch that feeds both on the same macro, so
 * there is no second list of targets to keep in step with this one.
 */
#if defined(TARGET_GBA) || defined(TARGET_NDS)
#  define PNES_WINDOWED_PANEL
#endif

#ifdef PNES_WINDOWED_PANEL

/**
 * @brief Rows of frame the level's logic places things in.
 *
 * NOT ::video::viewport_py(): that is how tall this target's OUTPUT is, which on
 * a windowed panel is the thing the window is smaller than. This is how tall the
 * region the window slides over is -- the 240 rows the level draws and positions
 * actors within, which does not vary by target. ::VerticalWindow clamps against
 * it so the window never runs off the bottom of the level.
 */
constexpr i16 kFrameRows = 240;

/**
 * @brief Vertical offset of the output window into the game's 240-row frame.
 *
 * Exists only under ::PNES_WINDOWED_PANEL, where the game's ::kFrameRows do not
 * fit the output: the GBA's panel is 160 rows and the DS's gameplay panel is
 * 192, both short of the 240 the game draws into, so the panel shows a WINDOW
 * onto that frame and a follow camera chooses which rows -- the player is
 * centred in the panel, clamped so the window never leaves the frame. Targets
 * whose output already covers the frame need no conversion at all, so rather
 * than calling this for a guaranteed 0 they do not see it.
 *
 * This is the ONE definition of that camera. ::ApplyHudSplit scrolls the
 * background by it (level.cpp) and ::WindowedSpriteY shifts sprites by it, which
 * is what keeps the two glued together: both derive it from the same player Y,
 * so there is no frame of lag between them and no second copy of the clamp.
 *
 * @param playerFrameY The camera subject's frame-space Y (P1's sprite row).
 */
constexpr i16 VerticalWindow(const oam::oam_t playerFrameY) {
    constexpr i16 panelH = static_cast<i16>(video::panel_rect(0).h);
    constexpr i16 mid    = static_cast<i16>(panelH >> 1);
    constexpr i16 anchor = static_cast<i16>(kFrameRows - panelH);
    const i16 raw = static_cast<i16>(playerFrameY) - mid;
    return raw < 0 ? 0 : (raw > anchor ? anchor : raw);
}

/**
 * @brief A frame-space sprite Y converted to this target's output space.
 *
 * The backends place sprite Y literally -- the Y written is the pixel row drawn
 * -- so on a target whose panel is a window onto the frame (::VerticalWindow)
 * the game owns the conversion, exactly as the title screen already owns its
 * preview's. A row above the window is off the panel, so it parks.
 */
constexpr oam::oam_t WindowedSpriteY(const oam::oam_t frameY, const oam::oam_t playerFrameY) {
    const i16 y = static_cast<i16>(frameY) - VerticalWindow(playerFrameY);
    return y < 0 ? ParkedSpriteY() : static_cast<oam::oam_t>(y);
}

#endif   // PNES_WINDOWED_PANEL

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
