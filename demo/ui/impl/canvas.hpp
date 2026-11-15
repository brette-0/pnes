#pragma once

#include <pnes/extras/ui/types.hpp>

#include STRCAT(../../gen/options/GEN_TARGET_DIR/options.hpp)
#include "../../chr/chr.h"
#include "../../src/main.hpp"
#include "../../src/graphics/windows.hpp"
// For ::kSelectorAttributes -- the corners below are the arrow's attributes by
// reference rather than by a second copy of the literal, so a palette change
// there moves both (see ::kCornerAttributes).
#include "singleChoice.hpp"
#include "sounds.hpp"

// hand written uitk canvas impls
namespace ui::impl {
    // Both provided by the mode that drives this scene -- see
    // demo/src/modes/title.cpp -- exactly as ::SelectorSlot and
    // ::SelectorScroll are, and for the same reason: the OAM layout is the
    // mode's, and so is the scroll the options page is read under. The four
    // corner sprites are consecutive from ::CornerSlot, in ::eCorner order.
    u8 CornerSlot();
    vec2<u16> CanvasScroll();

    // The focus ring's four sprites, in the order the mode reserves their OAM
    // slots (see title.cpp's ::kCornerSlot).
    enum eCorner : u8 {
        UpperLeft,
        UpperRight,
        LowerLeft,
        LowerRight,
        kCorners
    };

    /// The corners as the PPU sees them: the same box art DrawTextBox paints a
    /// button's border with (ui/impl/buttons.hpp), taken from the ui set banked
    /// a second time as sprites (windows::uiSprites, PPU $1800) -- which is
    /// exactly how the arrow reaches its own tile.
    constexpr u8 kCornerTiles[kCorners] = {
        static_cast<u8>(windows::uiSprites + blob::ui::box_ul),
        static_cast<u8>(windows::uiSprites + blob::ui::box_ur),
        static_cast<u8>(windows::uiSprites + blob::ui::box_bl),
        static_cast<u8>(windows::uiSprites + blob::ui::box_br),
    };

    /// The arrow's attributes, verbatim: sprite palette 1, unflipped, in front
    /// of the background. The corners are the same kind of thing the arrow is
    /// -- UI drawn over the scene in the UI's own colours -- so they want the
    /// palette title.cpp already loads for it (::kSelectorAttributes).
    constexpr u8 kCornerAttributes = kSelectorAttributes;

    /// The RESTING offset of each corner from the rect's own corner, along BOTH
    /// axes: a pixel under a full tile, so the art sits clear of whatever it
    /// brackets rather than over the component's own border, without reading as
    /// detached from it -- a full tile diagonally is 11px of daylight, which
    /// looked loose. A rect still wants a spare row and column around it (the
    /// corner bleeds one pixel into its own first row/column and no further),
    /// and the two Canvas rects in options.uis are sized for exactly that.
    constexpr i16 kSelectionGap = 7;

    // The hone: all four corners drift IN towards the rect they bracket and
    // back out again, each along both of its axes at once, so the ring
    // tightens and relaxes rather than sliding anywhere. The arrow's sway with
    // one more dimension (::kSwayOffsets, singleChoice.hpp) -- a table and not
    // a sine for the same reason: the phase is a mask and an index, so a frame
    // of it costs no multiply, no divide and no trig on a 6502.
    //
    // 2px of travel, so the ring lives between 5px and ::kSelectionGap's 7px
    // off the rect. Four was too much in both directions at once: it closed to
    // half a tile, where the corner art sat squarely over a button's own
    // border, and opened to a full one, where it read as four loose sprites
    // near a box rather than a ring around it. Phase 0 is the resting position,
    // fully retreated, exactly as the arrow's phase 0 is the position its
    // option's rect names -- so a ring placed cold is placed at rest and the
    // animation only ever closes it.
    //
    // Paired phases, which 2px across 8 of them leaves room for: a breath at
    // each extreme (two phases at rest across the wrap, two at full hone) and
    // single-pixel steps between, so the drift is slower as well as shorter.
    // The arrow's own table repeats its ends for the same reason -- a plain
    // ramp back looks mechanical.
    constexpr u8 kHoneOffsets[8] = {0, 1, 1, 2, 2, 1, 1, 0};

    /// Frames per hone step: 1 << 3 == 8, so a full cycle is 8 * 8 == 64
    /// frames. The arrow's cadence (::kSwayShift), deliberately -- the two
    /// indicators are on screen together and a second tempo reads as a fault.
    constexpr u8 kHoneShift = 3;

    /// The hone offset for a given frame. ::frameCounter wraps at a power of
    /// two and so does this cycle, so the wrapping frame steps by one phase
    /// like any other (see ::frameCounter, main.hpp).
    constexpr u8 HoneOffset(const u16 frame) {
        return kHoneOffsets[(frame >> kHoneShift) & 0x7];
    }

    /// Sprites are drawn one scanline BELOW the row OAM names -- the NES's own
    /// behaviour, which every pnes backend reproduces (src/emu/ppu.cpp and
    /// src/nds/video.cpp both render at `y + 1`). So a sprite wanted on a given
    /// pixel row is written one row above it. X has no such bias.
    constexpr i16 kSpriteYBias = 1;

    // The ring's four resting edges, in screen pixels -- what ::SelectComponent
    // worked out, before a hone is laid over it. Kept for the reason
    // ::selectorBaseX is: the hone has to be applied afresh every frame and
    // cannot read its own base back out of OAM, because what is there already
    // has a hone in it and honing that again would walk the ring into the
    // middle of the component.
    //
    // `atomic` for the reason everything else crossing this boundary is: these
    // are written wherever the selection moved (an input loop, or a generated
    // Pass() reached from one) and read in the NMI that lays the hone over
    // them.
    inline atomic u16 ringLeft   = 0;
    inline atomic u16 ringRight  = 0;
    inline atomic u16 ringTop    = 0;
    inline atomic u16 ringBottom = 0;

    // Lays one phase of the hone over the resting edges: each corner in along
    // both of its axes by `off`, which is what makes the near edges add it and
    // the far edges subtract it. Every `ring*` is read once into a local --
    // they are `atomic`, so every mention would otherwise be a fresh load, and
    // each is needed twice.
    inline void ApplyCornerHone(const u8 off) {
        const u16 left   = ringLeft;
        const u16 right  = ringRight;
        const u16 top    = ringTop;
        const u16 bottom = ringBottom;

        const u8 slot = CornerSlot();
        OAMBuffer[slot + UpperLeft].x  = static_cast<oam::oam_t>(left + off);
        OAMBuffer[slot + UpperLeft].y  = static_cast<oam::oam_t>(top + off);
        OAMBuffer[slot + UpperRight].x = static_cast<oam::oam_t>(right - off);
        OAMBuffer[slot + UpperRight].y = static_cast<oam::oam_t>(top + off);
        OAMBuffer[slot + LowerLeft].x  = static_cast<oam::oam_t>(left + off);
        OAMBuffer[slot + LowerLeft].y  = static_cast<oam::oam_t>(bottom - off);
        OAMBuffer[slot + LowerRight].x = static_cast<oam::oam_t>(right - off);
        OAMBuffer[slot + LowerRight].y = static_cast<oam::oam_t>(bottom - off);
    }

    // One frame of the hone. Called from the mode's handler for the state the
    // ring is on screen in, beside the ::oam::RefreshSprites that uploads what
    // it writes -- the same arrangement ::AnimateSelector has.
    //
    // Y is what says whether there is a ring to hone: ::SelectComponent parks
    // an off-screen one itself, so that value means "no ring on screen"
    // whatever put it there, and no second flag is needed to track it. Leaving
    // a parked sprite alone also means this cannot drag one back into view
    // between a park and the next placement.
    inline void AnimateCorners() {
        if (OAMBuffer[CornerSlot() + UpperLeft].y == ParkedSpriteY()) return;
        ApplyCornerHone(HoneOffset(frameCounter));
    }

    // Puts all four corners off every panel the target has, so the backend's
    // own clip drops them (::ParkedSpriteY). The same park ::ParkSelector
    // performs, and the same meaning: Y is what says whether a corner is on
    // screen, so nothing else has to track it.
    inline void ParkCorners() {
        const u8 slot = CornerSlot();
        for (u8 i = 0; i < kCorners; i++) {
            OAMBuffer[slot + i].y = ParkedSpriteY();
        }
    }

    // What a selected component looks like: the four corner sprites moved onto
    // the corners of its rect (see genCanvasDecl's DrawComponentSelection).
    //
    // THE four corner sprites -- the same OAM slots, holding the same tiles
    // with the same attributes, that mark the selected Canvas. One indicator
    // either way, moved rather than copied: there is only ever one selection on
    // screen, so a second set of slots would hold a parked sprite at best and a
    // contradiction at worst. It is the arrangement the arrow already has
    // across the two menus that share it (singleChoice.hpp).
    //
    // Plane pixels minus the page's scroll is the screen pixel -- the same
    // transform ::PlaceSelector applies to the arrow, under the scroll the mode
    // hands over in ::CanvasScroll -- then out by ::kSelectionGap and up by
    // ::kSpriteYBias.
    //
    // The far edges are `pos + size` rather than `pos + size - 1`: one past the
    // rect's last cell IS the cell a tile out from it, so the gap is already in
    // that term and only the near edges subtract it.
    //
    // Parked rather than clamped if the ring lands off the top or left, and all
    // four together: it is one indicator, and three corners of a box reads as a
    // glitch where none reads as "nothing selected". Same guard, and the same
    // reason for it, as ::PlaceSelector's. Note it is the RING that has to fit,
    // not the rect -- a rect flush against the top of the panel parks a ring
    // that would have sat above it.
    inline void SelectComponent(const Rect& current) {
        const vec2<u16> scroll = CanvasScroll();

        const i16 left   = static_cast<i16>(current.pos.x * 8 - scroll.x - kSelectionGap);
        const i16 right  = static_cast<i16>((current.pos.x + current.size.x) * 8 - scroll.x);
        const i16 top    = static_cast<i16>(current.pos.y * 8 - scroll.y - kSelectionGap - kSpriteYBias);
        const i16 bottom = static_cast<i16>((current.pos.y + current.size.y) * 8 - scroll.y - kSpriteYBias);

        if (left < 0 || top < 0) {
            ParkCorners();
            return;
        }

        ringLeft   = static_cast<u16>(left);
        ringRight  = static_cast<u16>(right);
        ringTop    = static_cast<u16>(top);
        ringBottom = static_cast<u16>(bottom);

        // The hone is applied here too, not left for the next
        // ::AnimateCorners -- a ring arriving on a new component mid-cycle
        // would otherwise show one frame fully retreated and then jump back to
        // the phase the cycle is actually on. Same reason ::PlaceSelector
        // applies the arrow's sway itself.
        ApplyCornerHone(HoneOffset(frameCounter));
    }

    // What a focus move looks like -- which here is nothing more than what the
    // arrival looks like. uitk hands over both rects because an indicator that
    // paints INTO the scene has to unpaint what it did to `last` first (see
    // genCanvasDecl's DrawUpdate); these four corners are sprites, so the move
    // is eight stores and the rect focus left does not come into it. Hence the
    // unnamed parameter: not an oversight, the whole saving.
    //
    // `inline` for the same reason every hook in buttons.hpp/sliders.hpp is:
    // this header is includable from more than one TU.
    inline void UpdateCanvas(const Rect&, const Rect& current) {
        // The sound of the ring MOVING, which is this hook and nothing else:
        // uitk calls it only when focus actually changed hands (a generated
        // Pass() early-outs at either end of the components table before it
        // gets here, and its Focus() early-outs when a click lands on the
        // component that already had focus), so a walk off the end of a
        // section is silent and a press that moves the ring is not.
        //
        // One effect for both directions, unlike a slider's two
        // (sliders.hpp): a slider's ends are a VALUE going up or down, where
        // this is a cursor arriving somewhere, and UP/DOWN through a section
        // reads as the same gesture either way.
        PostSfx(sfx::selectnext);
        SelectComponent(current);
    }

    // What STEPPING INTO a Canvas looks and sounds like: the ring placed on
    // the component that holds its focus, and the sound of a selection being
    // made.
    //
    // Separate from ::SelectComponent, which it otherwise only wraps, because
    // that one has a second caller with nothing to announce: title.cpp's
    // ::PlaceOptionsCorners brackets the COLUMN's own rect with the same four
    // sprites (see the note on these being one indicator, not two), and it is
    // reached when the page is first shown and whenever B backs out of a
    // section. Putting the effect in ::SelectComponent would sound a selection
    // on both -- including on the frame B had just sounded a deselection.
    inline void ConfirmComponent(const Rect& current) {
        PostSfx(sfx::select);
        SelectComponent(current);
    }
}

// impl for gen code
namespace gen::options {
    // One DrawUpdate()/DrawComponentSelection() pair per Canvas. uitk declares
    // both (and defines neither) for any Canvas with something focusable in it,
    // so both of the options scene's Canvases need both here even though
    // neither paints anything yet: DrawUpdate is what a generated Pass() calls
    // when focus moves, and DrawComponentSelection is the scene's own way in --
    // nothing generated calls that one at all.
    //
    // All four forward to the same two ui::impl hooks: the two sections are one
    // focus ring's worth of appearance, drawn the same way, and the moment one
    // wants its own look it gets a body of its own here rather than a flag
    // passed into a shared one.
    //
    // DrawComponentSelection goes to ::ConfirmComponent rather than straight
    // to ::SelectComponent, which is what makes the selection SOUND: this hook
    // is only ever reached by the page handing a Canvas the ring (title.cpp's
    // ::SelectFocusedComponent, on A or on a click that entered a section), so
    // every call to it is a player choosing a component. The column-level
    // placement that shares these sprites does not come through here at all.
    namespace videoCanvas {
        inline void DrawUpdate(const Rect& last, const Rect& current) {
            ui::impl::UpdateCanvas(last, current);
        }

        inline void DrawComponentSelection(const Rect& current) {
            ui::impl::ConfirmComponent(current);
        }
    }

    namespace audioCanvas {
        inline void DrawUpdate(const Rect& last, const Rect& current) {
            ui::impl::UpdateCanvas(last, current);
        }

        inline void DrawComponentSelection(const Rect& current) {
            ui::impl::ConfirmComponent(current);
        }
    }
}
