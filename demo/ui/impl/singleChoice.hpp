#pragma once

#include <pnes/video.hpp>
#include STRCAT(../../gen/title/GEN_TARGET_DIR/title.hpp)
#include STRCAT(../../gen/gameOptions/GEN_TARGET_DIR/gameOptions.hpp)
#include "../../chr/chr.h"
#include "../../src/main.hpp"
#include "../../src/graphics/windows.hpp"

// Hand-written SingleChoice hooks -- the selection indicator uitk declares and
// refuses to draw (`void DrawChange(u8 from, u8 to);`, see genSingleChoiceDecl).
// uitk gives us where every option sits and which one moved; everything below
// is the part it cannot know: that this project marks a selection with a
// SPRITE, one tile to the left of the option's text.
//
// A sprite rather than a background tile, and that choice is most of this
// file. OAM is staged in ::OAMBuffer -- ordinary RAM -- and uploaded by
// ::oam::RefreshSprites once a frame, so moving the arrow is a few stores that
// are safe from anywhere, at any point in the frame. A nametable tile is not:
// it has to be written in vblank, which cost the menu a five-byte mailbox and
// a flush call in every one of the mode's NMI handlers. All of that is gone.
// Moving the selection now costs no PPU writes whatsoever.
//
// The arrow is also ONE sprite shared by every menu here, not one per menu:
// the title menu and the play-mode menu are alternative content for the same
// rows (only ever one of them is on screen), so a second slot would only ever
// hold a parked sprite.
namespace ui::impl {
    // Both provided by the mode that drives these menus -- see
    // demo/src/modes/title.cpp. Declared rather than defined for the same
    // reason uitk declares DrawChange() and leaves it to us: a sprite is
    // placed in SCREEN space while uitk's rects are in nametable PLANE space,
    // and the scroll standing between the two is the mode's, as is its OAM
    // layout. Neither is something this header could work out.
    u8 SelectorSlot();
    vec2<u16> SelectorScroll();

    /// The arrow as the PPU sees it: the ui art banked a second time as
    /// sprites (windows::uiSprites, PPU $1800 -- see title.cpp's R1 switch).
    /// This is what that window exists for.
    constexpr u8 kSelectorTile = windows::uiSprites + blob::ui::arrow;

    /// Sprite palette 1, unflipped, in front of the background. Palette 1
    /// because 0 is the characters' (::maryColors) and the arrow wants the
    /// UI's own colours -- title.cpp loads ::titleScreenColours into it, the
    /// same three the HUD's BG_3 holds, so the sprite arrow is the colour the
    /// background one was.
    constexpr u8 kSelectorAttributes = 1;

    /// One tile to the left of the option, with a tile of clear space between
    /// -- the two-cell offset the background arrow used, now in pixels.
    constexpr u16 kSelectorGap = 2 * 8;

    // The sway: the arrow drifts towards the option it points at and back,
    // which is the one thing a sprite indicator can do that the background
    // tile it replaced could not.
    //
    // A table rather than a sine, and a power-of-two one: the phase is a
    // mask and an index, so a frame of sway costs no multiply, no divide and
    // no trig on a 6502. Signed offsets are not needed either -- the arrow
    // sways one way only, towards the text, so every entry is positive and
    // phase 0 is the resting position the option's own rect names.
    //
    // 3px of travel at the far end, and a triangle rather than a square so
    // the movement reads as a drift instead of a flick. The ends repeat (3,3
    // and 0,0 across the wrap) which gives it a moment of rest at each
    // extreme -- a plain ramp back looks mechanical.
    constexpr u8 kSwayOffsets[8] = {0, 1, 2, 3, 3, 2, 1, 0};

    /// Frames per sway step: 1 << 3 == 8, so a full cycle is 8 * 8 == 64
    /// frames, a little over a second on NTSC. Slow enough to read as a drift
    /// and not a wobble.
    constexpr u8 kSwayShift = 3;

    /// The sway offset for a given frame. ::frameCounter wraps at a power of
    /// two and so does this cycle, so the wrapping frame steps by one phase
    /// like any other (see ::frameCounter, main.hpp).
    constexpr u8 SwayOffset(const u16 frame) {
        return kSwayOffsets[(frame >> kSwayShift) & 0x7];
    }

    // The arrow's unswayed X -- where ::PlaceSelector put it, before the sway
    // is added on. Kept because the sway has to be applied afresh every frame
    // and cannot read it back out of OAM: the value there already has a sway
    // in it, and adding to that would walk the arrow off the screen.
    //
    // `atomic` for the reason everything else crossing this boundary is: it is
    // written from wherever the menu was stepped (a mainline input loop) and
    // read in the NMI that lays the sway over it.
    inline atomic u16 selectorBaseX = 0;

    // Off every panel the target has, so the backend's own clip drops it (see
    // ::ParkedSpriteY). Used while no menu is on screen at all: the mode
    // erases the labels, and an arrow left pointing at rows that no longer
    // hold a menu is the one thing worse than no arrow.
    inline void ParkSelector() {
        OAMBuffer[SelectorSlot()].y = ParkedSpriteY();
    }

    // Places the arrow beside `option`, whose Rect is in plane tiles.
    //
    // Plane pixels minus the mode's scroll is the screen pixel, which is the
    // whole of the transform: the same arithmetic ::PlaceStandingSprites
    // applies to the characters, and the same scroll. It holds on a
    // single-screen target (where the menu is a scrolled-to nametable below a
    // raster split) and on a dual-screen one (where it is plane rows 30+ with
    // the plane bottom-anchored against the top panel) without either being a
    // case here, because both are already expressed in that one scroll.
    //
    // Signed, and parked rather than clamped if it lands above the screen: a
    // u16 would wrap an off-screen arrow to the bottom of the frame, which on
    // NES is a byte and would put it somewhere visible. Same guard, and the
    // same reason for it, as ::WindowedSpriteY in main.hpp.
    inline void PlaceSelector(const Rect& option) {
        const vec2<u16> scroll = SelectorScroll();
        const i16 x = static_cast<i16>(option.pos.x * 8 - kSelectorGap - scroll.x);
        const i16 y = static_cast<i16>(option.pos.y * 8 - scroll.y);

        const u8 slot = SelectorSlot();
        if (x < 0 || y < 0) {
            OAMBuffer[slot].y = ParkedSpriteY();
            return;
        }
        OAMBuffer[slot].tile       = kSelectorTile;
        OAMBuffer[slot].attributes = kSelectorAttributes;
        OAMBuffer[slot].y          = static_cast<oam::oam_t>(y);

        // The sway is applied here too, not left for the next ::AnimateSelector
        // -- the arrow moving to a new option mid-cycle would otherwise snap to
        // the unswayed position for one frame and then jump back.
        selectorBaseX     = static_cast<u16>(x);
        OAMBuffer[slot].x = static_cast<oam::oam_t>(x + SwayOffset(frameCounter));
    }

    // One frame of sway. Called from the mode's NMI handlers, beside the
    // ::oam::RefreshSprites that uploads what it writes.
    //
    // Y is what says whether there is an arrow to sway: ::PlaceSelector parks
    // an off-screen one at ::ParkedSpriteY itself, so that value means "no
    // arrow on screen" whatever put it there, and no second flag is needed to
    // track it. Leaving a parked sprite alone also means this cannot drag one
    // back into view between a park and the next placement.
    inline void AnimateSelector() {
        const u8 slot = SelectorSlot();
        if (OAMBuffer[slot].y == ParkedSpriteY()) return;

        const u16 base = selectorBaseX;
        OAMBuffer[slot].x = static_cast<oam::oam_t>(base + SwayOffset(frameCounter));
    }

    // The menus below are identical, and deliberately not one shared helper --
    // same arrangement buttons.hpp and sliders.hpp have for their own
    // identical hooks. Each menu owns its indicator, and the moment one wants
    // a different tile or offset the shared version would have to be unpicked
    // again. `inline`, because uitk declares these non-inline (`void
    // DrawChange(u8, u8);`) and that is what lets this header be included from
    // more than one TU.
    //
    // `from` is unused now, and its absence is the point: a sprite has one
    // position, so moving it is a store and not an unpaint-then-paint. uitk's
    // cold paint (`from == to`, see genSingleChoiceDecl) therefore needs no
    // special case either -- placing the arrow where the option is does the
    // right thing whether or not it was already somewhere.
    template <typename Options>
    inline void DrawSelectorChange(const Options& options, const u8 to) {
        const Rect r = options[to];
        PlaceSelector(r);
    }
}

namespace gen::title::TitleOptions {
    // Vertical only, and loudly so: the indicator offset is along X, to sit
    // beside a row of text, which is the wrong side of a menu whose options
    // run across the screen. The axis comes from the scene (uitk emits
    // `vertical` alongside `option`), so this fails at the next build after
    // someone unticks Vertical in uitk, not at runtime.
    static_assert(vertical, "horizontal SingleChoice menus are not implemented in this project -- "
                             "retick Vertical on this menu in uitk, or implement the above-the-option "
                             "indicator placement in ui::impl");

    inline void DrawChange(const u8, const u8 to) {
        ui::impl::DrawSelectorChange(options, to);
    }
}

namespace gen::gameOptions::gameOptions {
    // See TitleOptions above -- same prohibition, checked per menu, because
    // the axis is per menu.
    static_assert(vertical, "horizontal SingleChoice menus are not implemented in this project -- "
                             "retick Vertical on this menu in uitk, or implement the above-the-option "
                             "indicator placement in ui::impl");

    inline void DrawChange(const u8, const u8 to) {
        ui::impl::DrawSelectorChange(options, to);
    }
}
