#pragma once

#include <pnes/audio.hpp>
#include <pnes/video.hpp>
#include STRCAT(../../gen/options/GEN_TARGET_DIR/options.hpp)
#include "../../chr/chr.h"
#include "../../src/graphics/windows.hpp"
#include "sounds.hpp"

namespace ui::impl {
    // @p width is the track's WHOLE footprint in cells, caps included, so it
    // lays out exactly like ui::impl::DrawTextBox's top row: a cap at @p pos, a
    // cap at `pos + width - 1`, and `width - 2` middles between them. The right
    // cap used to go to `pos + width`, which both ran a cell past the footprint
    // and left `pos + width - 1` never written -- a blank cell stranded between
    // the end of the middle run and a detached end cap.
    template <bool orient>
    inline void DrawSliderBody(const u16 pos, const u8 width) {
        if constexpr (!orient) return;
        constexpr u8 LeftEdgeGraphic  = windows::ui + blob::ui::slider_left;
        constexpr u8 RightEdgeGraphic = windows::ui + blob::ui::slider_right;
        constexpr u8 MiddleGraphic    = windows::ui + blob::ui::slider_middle;

        ppu::WriteSingleToNameTable(pos, LeftEdgeGraphic);
        ppu::WriteRepeatedToNameTable(pos + 1, MiddleGraphic, width - 2, 0);
        ppu::WriteSingleToNameTable(pos + width - 1, RightEdgeGraphic);
    }

    // How far a handle may travel: the track's middle cells, and only those.
    // The caps are NOT handle positions -- ::ClearSliderHandle has exactly one
    // tile to put back and it is the middle one, so a handle allowed onto a cap
    // would replace that cap with a middle the moment it moved off again and
    // the track would lose its end for the rest of the scene. @p width is
    // ::DrawSliderBody's, i.e. the whole footprint.
    constexpr u8 SliderTravel(const u8 width) {
        return static_cast<u8>(width - 2);
    }

    // Which handle position a CLICKED CELL means. @p cell is uitk's, counted
    // from the rect's own anchor along the track (see a generated slider's
    // DrawClick), and @p width is ::DrawSliderBody's whole footprint.
    //
    // The -1 is the cap: cell 0 is the left end graphic, so the first position
    // the handle can occupy is cell 1, and an `amount` is therefore always one
    // less than the cell it is drawn in (::DrawSliderHandle's own `pos + 1 +
    // amt`). Without it every click would land the handle one cell to the
    // RIGHT of the pointer -- visible on a short track, and worst exactly where
    // a player aims most precisely, at the ends.
    //
    // Both caps resolve to the position nearest them rather than to nothing.
    // A cap is not somewhere the handle may sit (::SliderTravel), but it is
    // unambiguously a direction -- clicking the left end graphic can only mean
    // "as far left as this goes" -- and travelling there is what a player who
    // clicked it is asking for. Rejecting the click instead would make the two
    // most obvious targets on the whole widget the only dead ones.
    //
    // This is also why a click needs its own mapping rather than sharing the
    // one a nudge uses: ::DrawChange DROPS an out-of-range amount, which is
    // right for a press walking off the end and wrong for a pointer aimed at
    // it. The clamp lives here, so DrawChange keeps its guard and its meaning.
    //
    // A track with no travel at all (a 2-cell footprint: two caps and nothing
    // between) has no position to name, so it answers 0 and ::DrawChange's
    // guard rejects it -- the handle stays off a track that cannot hold one.
    constexpr u8 SliderAmountForCell(const u8 width, const u8 cell) {
        const u8 travel = SliderTravel(width);
        if (!travel) return 0;
        if (cell == 0) return 0;                                  // left cap
        const u8 amt = static_cast<u8>(cell - 1);
        return amt >= travel ? static_cast<u8>(travel - 1) : amt;  // right cap clamps
    }

    // The two directions between a handle position and the proportion it
    // stands for: 0 at the left end, 100 at the right, linear in between. @p
    // width is ::DrawSliderBody's, i.e. the whole footprint, so the division is
    // by the LAST position rather than by the count -- the right-hand end has
    // to read as exactly 100, not as travel-1 over travel.
    //
    // A degenerate one-position track reads as full rather than as silent: a
    // slider too narrow to move is not a slider turned down, and answering 0
    // there would mute a game whose panel happened to be too small.
    constexpr u8 SliderPercent(const u8 width, const u8 amt) {
        const u8 travel = SliderTravel(width);
        if (travel <= 1) return 100;
        const u8 last = static_cast<u8>(travel - 1);
        if (amt >= last) return 100;
        return static_cast<u8>(static_cast<u16>(amt) * 100 / last);
    }

    // The same proportion ::SliderPercent reports, scaled to the whole byte the
    // mixing backends read as a level (0 silent, 255 unity -- see
    // include/pnes/audio.hpp's ::SfxPlay, and note the API itself assigns the
    // byte no meaning; that range is the backends' own).
    //
    // Derived from ::SliderPercent rather than from `amt` directly, so there is
    // one definition of what a handle position is worth and this cannot drift
    // from what the music slider pushes into ::TrackVolume. The rounding is the
    // same +50/100 trick ::SliderAmountFor uses, scaled: `* 255 + 50) / 100`
    // lands 100 on exactly 255 and 0 on exactly 0, which are the two values
    // that have to be exact.
    constexpr u8 SliderCoefficient(const u8 width, const u8 amt) {
        return static_cast<u8>((static_cast<u16>(SliderPercent(width, amt)) * 255 + 50) / 100);
    }

    // The inverse, rounded to the nearest cell -- for restoring a proportion
    // onto a track whose width has CHANGED (a runtime viewport, see
    // title.cpp's ApplyWidescreenChange), where the stored position means a
    // different fraction than it did before.
    constexpr u8 SliderAmountFor(const u8 width, const u8 percent) {
        const u8 travel = SliderTravel(width);
        if (travel <= 1) return 0;
        const u8 last = static_cast<u8>(travel - 1);
        const u8 pct  = percent > 100 ? 100 : percent;
        return static_cast<u8>((static_cast<u16>(pct) * last + 50) / 100);
    }

    // Clear/Draw rather than one UpdateSlider(pos, amt, lastAmt): the hooks
    // below no longer hold both positions at once. `amount` is the slider's
    // only value byte, so moving a handle reads it, clears the cell it names,
    // overwrites it, and draws the cell it now names -- two one-cell writes
    // around a single store, each of which wants its own call.
    // @p pos is the track's own anchor, the same one ::DrawSliderBody takes,
    // and @p amt counts from the FIRST MIDDLE cell -- hence the +1 here rather
    // than at each of the four call sites, which is where the old `pos + 1`
    // anchor came from and what shifted the whole track a cell right of its
    // rect.
    template <bool orient>
    inline void ClearSliderHandle(const u16 pos, const u8 amt) {
        if constexpr (!orient) return;
        constexpr u8 MiddleGraphic = windows::ui + blob::ui::slider_middle;
        ppu::WriteSingleToNameTable(pos + 1 + amt, MiddleGraphic);
    }

    template <bool orient>
    inline void DrawSliderHandle(const u16 pos, const u8 amt) {
        if constexpr (!orient) return;
        constexpr u8 ActiveGraphic = windows::ui + blob::ui::slider_cursor;
        ppu::WriteSingleToNameTable(pos + 1 + amt, ActiveGraphic);
    }
}

namespace gen::options::audioCanvas {
// NES only has no sliders, and that is the scene's decision, not this file's:
// options.uis hides musicSlider and SFXSlider (and the MUSIC/SFX labels that
// now hang off them) on it, because the mix there is a pair of on/off buttons
// instead -- disableMusic/disableSFX, which are in turn hidden on every
// target BUT NES.
// So the generated NES header has no slider namespace, and hooks for one
// would not compile there.
//
// Every other target has both sliders, including the dual-screen ones: DS,
// DSi and 3DS build options_ds.uis rather than options.uis, and that scene
// carries the same two sliders anchored to PANEL1_* so they land on the
// bottom screen of either console. Guarded once around the whole block rather
// than per namespace, since the two sliders always come and go together;
// buttons.hpp does the same with #if TARGET_GC around its GC-only button.
#ifndef TARGET_NES
    // `pos` is already the PPU address of the slider's anchor -- uitk resolves
    // it in Make(), so there is no CartesianToAddress call left to pay here.
    // `rect` is copied out whole before its size is read: on a runtime-viewport
    // target it is `atomic Rect`, i.e. volatile, so reading it goes through
    // Rect's volatile copy constructor once rather than per field.
    //
    // `inline`, like every hook in buttons.hpp: uitk declares these non-inline
    // (`void Draw();`), and adding inline at the definition is what lets this
    // header be included from more than one TU without a duplicate symbol.
    //
    // The two namespaces below are deliberately identical rather than one
    // shared helper -- same arrangement buttons.hpp has for its own identical
    // button hooks. Each slider owns its own graphics decision, and the moment
    // one wants a different track or handle tile the shared version would have
    // to be unpicked again.
    namespace musicSlider {
        // Horizontal only, and loudly so. Every ui::impl helper is templated on
        // the orientation and compiles to nothing for the axis it doesn't
        // handle, so a slider re-axised in uitk would otherwise draw an empty
        // track and read as a graphics bug rather than an unimplemented
        // layout. The axis comes from the scene (uitk emits `vertical`
        // alongside `amount`), so this fails at the next build after someone
        // ticks Vertical, not at runtime.
        static_assert(!vertical, "vertical sliders are not implemented in this project -- "
                                  "untick Vertical on this slider in uitk, or implement the "
                                  "column-walking path in ui::impl");

        inline void Draw() {
            const Rect r = rect;
            musicText::Draw();
            // The track IS the rect: `pos` is its own anchor and `r.size.x` its
            // whole width, so it lands inside the footprint the focus ring is
            // drawn around -- ui::impl::DrawTextBox treats a button's rect
            // exactly the same way. It used to be drawn at `pos + 1`, two cells
            // wider than the rect, which put it a cell right of its own left
            // edge and three cells past its right one.
            ui::impl::DrawSliderBody<!vertical>(pos, r.size.x);

            // Whatever `amount` already holds IS the starting position -- uitk
            // allocates that byte, so a caller wanting a non-zero default
            // writes it before the first Draw() instead of passing it in. The
            // guard is the same one DrawChange() applies: an out-of-range
            // value leaves the track drawn and the handle off it, rather than
            // writing a cell outside the slider's own footprint. "In range" is
            // ::SliderTravel, not the rect width: the caps are not positions.
            const u8 amt = amount;
            if (amt >= ui::impl::SliderTravel(r.size.x)) return;
            ui::impl::DrawSliderHandle<!vertical>(pos, amt);
        }

        inline void DrawChange(const u8 amt) {
            const Rect r = rect;
            // Caps overflow AND underflow: uitk's Pass() hands us `cur - 1`
            // unclamped, so stepping left off the end arrives here as 255.
            if (amt >= ui::impl::SliderTravel(r.size.x)) return;

            // Clear where the handle is, move the value, draw where it now is.
            // `amount` is atomic (volatile on NES), so it is loaded once into
            // `last` rather than read again for the second write.
            const u8 last = amount;

            // Which WAY the handle went, which is the one place in this
            // project an effect is chosen by direction. A slider is a value,
            // not a cursor, so the two directions are not the same gesture --
            // where a Canvas's focus move uses one effect for both
            // (ui/impl/canvas.hpp's UpdateCanvas).
            //
            // After the range guard above, so walking off either end of the
            // track is silent -- the same frame that draws nothing should
            // sound nothing. And `!=` rather than `<`/`>` alone: a click that
            // resolves to the cell the handle is already in
            // (::SliderAmountForCell, including either cap once the handle has
            // reached it) is not a move and gets no effect.
            if (amt != last) {
                ui::impl::PostSfx(amt > last ? sfx::selectnext : sfx::selectprev);
            }

            ui::impl::ClearSliderHandle<!vertical>(pos, last);
            amount = amt;
            ui::impl::DrawSliderHandle<!vertical>(pos, amt);

            // The engine side of the slider, pushed from the one place the
            // value changes rather than polled per frame. The track's leftmost
            // position is silence and its rightmost full output, which is
            // exactly audio::TrackVolume's 0-100 contract, so the mapping is
            // ::SliderPercent and nothing else -- no curve, no clamp of its own.
            //
            // A backend with no music to scale (the 3DS/GBA/DS stubs) or no
            // master level (NES, which has no slider either) defines this as a
            // no-op, so this call needs no guard of its own.
            audio::TrackVolume(ui::impl::SliderPercent(r.size.x, amt));
        }

        // The pointer's way in, from the generated Click(). All the layout
        // knowledge is in ::SliderAmountForCell -- which cell is a cap, and
        // which handle position a cell stands for -- so this is the same
        // DrawChange() a press makes, reached with the position the click
        // named. Which means a click and a nudge share one repaint path, one
        // guard and one volume push; only the way the amount was arrived at
        // differs.
        inline void DrawClick(const u8 cell) {
            const Rect r = rect;
            DrawChange(ui::impl::SliderAmountForCell(r.size.x, cell));
        }
    }

    namespace SFXSlider {
        // See musicSlider above -- same prohibition, checked per slider,
        // because the axis is per slider.
        //
        // STILL PUSHES NOTHING FROM HERE, and now for a better reason than
        // before. musicSlider calls ::TrackVolume because music has a master
        // level to set; effects do not -- audio::SfxPlay takes its level PER
        // CALL instead (see include/pnes/audio.hpp on why that is the better
        // primitive). So there is nothing for this hook to push: the value is
        // read where an effect is actually played, by title.cpp's ::DriveAudio,
        // through ::SliderCoefficient above.
        //
        // Which means `amount` IS the setting, not a copy of it -- the handle
        // is the single source of truth and no backend holds a second one that
        // could disagree with where this slider is sitting.
        static_assert(!vertical, "vertical sliders are not implemented in this project -- "
                                  "untick Vertical on this slider in uitk, or implement the "
                                  "column-walking path in ui::impl");

        inline void Draw() {
            const Rect r = rect;
            SFXText::Draw();
            // The track IS the rect: `pos` is its own anchor and `r.size.x` its
            // whole width, so it lands inside the footprint the focus ring is
            // drawn around -- ui::impl::DrawTextBox treats a button's rect
            // exactly the same way. It used to be drawn at `pos + 1`, two cells
            // wider than the rect, which put it a cell right of its own left
            // edge and three cells past its right one.
            ui::impl::DrawSliderBody<!vertical>(pos, r.size.x);

            const u8 amt = amount;
            if (amt >= ui::impl::SliderTravel(r.size.x)) return;
            ui::impl::DrawSliderHandle<!vertical>(pos, amt);
        }

        inline void DrawChange(const u8 amt) {
            const Rect r = rect;
            // Caps overflow AND underflow: uitk's Pass() hands us `cur - 1`
            // unclamped, so stepping left off the end arrives here as 255.
            if (amt >= ui::impl::SliderTravel(r.size.x)) return;

            const u8 last = amount;

            // See musicSlider::DrawChange -- same direction test, duplicated
            // for the same reason every other hook in this file is.
            if (amt != last) {
                ui::impl::PostSfx(amt > last ? sfx::selectnext : sfx::selectprev);
            }

            ui::impl::ClearSliderHandle<!vertical>(pos, last);
            amount = amt;
            ui::impl::DrawSliderHandle<!vertical>(pos, amt);
        }

        // See musicSlider::DrawClick -- same mapping, same reason it is per
        // slider rather than shared: the hooks are duplicated in this file by
        // design and only the layout MATHS is common.
        inline void DrawClick(const u8 cell) {
            const Rect r = rect;
            DrawChange(ui::impl::SliderAmountForCell(r.size.x, cell));
        }
    }
#endif
}
