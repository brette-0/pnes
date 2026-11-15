/**
 * @file input.hpp
 * @brief Controller polling and button bit definitions.
 *
 * The NES exposes two standard controllers at MMIO ports \$4016 and
 * \$4017; this header defines the addresses as ::IO_PORT1 and
 * ::IO_PORT2 plus a portable polling helper that works on both
 * targets. Each poll returns a byte whose bits correspond to the
 * ::Buttons enumeration.
 *
 * The same poll is where a target's pointing device -- a touch panel or a
 * mouse -- lands: ::input::click is the frame's pointer state, read-only to
 * a game and written only by ::input::PollControllers. See ::input::Click
 * for the space it reports in and ::input::has_click for the targets that
 * have one at all.
 */
#pragma once

#include <intsh>
using namespace br0::intsh;

#include "types.hpp"
#include "technology.hpp"
#ifdef TARGET_NES
#ifndef IO_PORT1
/** @brief Memory-mapped register for controller 1 (NES \$4016). */
#define IO_PORT1 *(volatile u8*)0x4016
/** @brief Memory-mapped register for controller 2 (NES \$4017). */
#define IO_PORT2 *(volatile u8*)0x4017
#endif
#endif

// Targets with a pointing device the engine can read a screen position from:
// a touch panel (NDS/DSi, 3DS, Switch in handheld, Wii U GamePad) or a mouse
// (desktop). Every other target -- NES, GBA, GameCube/Wii, PSP, PS2, PS3 --
// has no way for a player to name a point on screen, and gets the empty
// ::input::Click stub below instead.
//
// Switch is listed on the strength of the handheld panel alone: docked there
// is no touch to read, which needs no condition here because it shows up the
// same way a finger that is not touching does -- no click this frame.
#if defined(TARGET_NDS) || defined(TARGET_CTR) || defined(TARGET_NX)   \
 || defined(TARGET_WIIU)                                               \
 || defined(TARGET_LINUX) || defined(TARGET_MACOS) || defined(TARGET_WINDOWS)
#define PNES_HAS_CLICK 1
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

/**
 * @brief Whether this target has a pointing device at all.
 *
 * `constexpr` rather than a macro test at the call site: a game writes
 * `if constexpr (input::has_click)` and the guarded code is dropped whole on
 * a target without one, with no `#ifdef` in the game's own source. ::click
 * exists either way (see ::Click), so the name always resolves and the
 * discarded branch still compiles.
 */
#ifdef PNES_HAS_CLICK
constexpr bool has_click = true;
#else
constexpr bool has_click = false;
#endif

#ifdef PNES_HAS_CLICK
/**
 * @brief The frame's pointer state, in the game's own internal resolution.
 *
 * Read-only to a game. The only writer is ::input::PollControllers -- this
 * class's one friend, declared above and defined once per backend -- so the
 * pointer arrives on the same tick as the buttons, through the same call, and
 * a game cannot write it at all (::Set is private).
 *
 * Every backend normalizes whatever its device reports into VIEWPORT PIXELS
 * before storing it: raw DS/3DS touch panel coordinates, the Switch's
 * 1280x720 touch, a desktop mouse in window space. So a game only ever sees
 * one space, on every target -- ::video::viewport_px() x
 * ::video::viewport_py() -- and never learns which device produced it. That
 * normalization is cheap everywhere it happens and free on the dual-screen
 * handhelds, whose panels are already 1:1 with viewport pixels: a DS touch
 * needs ::video::panel_rect(1)'s offset added and nothing more.
 *
 * Folding the panel offset in at the backend is deliberate, and why there is
 * no panel accessor here. ::video::panel_rect is already the one place the
 * dual-screen split lives, a viewport coordinate says which panel it is in
 * by construction, and a game that hit-tests UI wants the same arithmetic on
 * a 3DS as on a PC.
 *
 * Deliberately NOT `atomic`, unlike the UI state that crosses into the NMI
 * (ui::impl::ringLeft and friends, demo/ui/impl/canvas.hpp). Both ends of
 * this live in the same context: a mode calls ::PollControllers from its own
 * loop and reads the click a few lines later, exactly as it already does
 * with the button bytes that call fills. Nothing here is written from an
 * interrupt, so there is no race for `volatile` to order -- and paying for
 * one would mean every accessor below re-reading storage it was handed.
 */
class Click {
public:
    /**
     * @brief Where the pointer is, in viewport pixels.
     *
     * The LAST KNOWN position when nothing is touching, not a sentinel --
     * which is what makes ::Up readable (the point a finger left the panel
     * is the point that release belongs to) and what lets a mouse that moves
     * without a button down still be followed. ::Held says whether it is
     * live; a coordinate cannot, since (0,0) is a real pixel.
     *
     * Built member-wise rather than returned as a copy: ::vec2 has a
     * user-provided volatile assignment operator (pnes/types.hpp), which
     * deprecates its implicit copy constructor, and `return pos;` would use
     * exactly that. Same situation ::Rect spells out a volatile copy
     * constructor for, and the same two stores either way.
     */
    AI vec2<u16> Get() const noexcept { return { pos.x, pos.y }; }

    /** @brief Whether something is touching, or a mouse button is down, right now. */
    AI bool Held() const noexcept { return held; }

    /**
     * @brief True for the one frame the pointer went down.
     *
     * The click half of the `inputs & ~prevInputs` strobe a button press
     * already uses (see demo/src/modes/title.cpp). This is what a UI hit
     * test wants: a held finger must not re-fire a button every frame it
     * rests on it, exactly as a held A does not.
     */
    AI bool Down() const noexcept { return held && !prev; }

    /** @brief True for the one frame the pointer came up. ::Get is still the
     *         point it was released at. */
    AI bool Up() const noexcept { return !held && prev; }

private:
    /**
     * @brief The backend's one way in: a position already in viewport pixels,
     *        and whether anything is down.
     *
     * Rolls the edge itself, so ::Down and ::Up are each true for exactly one
     * poll and no backend has to remember last frame -- the same reason the
     * normalization contract is "viewport pixels" rather than "whatever your
     * device said": every per-target file ends up with one line.
     *
     * @p p is ignored while @p down is false. A device that reports no
     * position when idle (every touch panel here) would otherwise overwrite
     * the release point with a zero, and ::Up would name the top-left corner
     * of the screen instead of wherever the finger actually left it. A mouse,
     * which always has a position, calls this with `down` false and its real
     * coordinates, so hover still tracks -- see each backend's
     * ::PollControllers.
     */
    AI void Set(const vec2<u16> p, const bool down) noexcept {
        prev = held;
        held = down;
        if (down) pos = p;
    }

    vec2<u16> pos{0, 0};
    bool held = false;
    bool prev = false;

    /// The backend half of this header, and the only writer. Matches the
    /// declaration above exactly, so every target's own definition of it is
    /// the friend -- no internal header, and no public setter a game could
    /// reach for.
    friend void PollControllers(u8*, u8*);
};

/** @brief The frame's pointer state. One per program; backends fill it in
 *         ::PollControllers. */
inline Click click;

#else
/**
 * @brief The clickless stub, so a game can name ::input::click unconditionally.
 *
 * An empty class with static accessors that fold to constants: `click.Get()`
 * compiles on a NES build, `if constexpr (input::has_click)` drops whatever
 * it guards, and a hit test written once works on every target with no
 * `#ifdef` in sight. That is the whole reason this exists on a target that
 * cannot click -- conditional compilation removes the STORAGE and the
 * backend's poll, not the name.
 *
 * `constexpr` object of an empty class, so there is no bss and no
 * initializer -- nothing reaches the ROM unless something takes its address,
 * which nothing does.
 */
class Click {
public:
    /** @brief Always the origin. ::Held is always false, so it never names a
     *         real press. */
    static constexpr vec2<u16> Get() noexcept { return {0, 0}; }
    /** @brief Always false: there is no pointing device on this target. */
    static constexpr bool Held() noexcept { return false; }
    /** @brief Always false. A guarded hit test constant-folds away entirely. */
    static constexpr bool Down() noexcept { return false; }
    /** @brief Always false. */
    static constexpr bool Up() noexcept { return false; }
};

/** @brief The frame's pointer state: a compile-time nothing on a target with
 *         no pointing device. */
inline constexpr Click click;
#endif

} // namespace input