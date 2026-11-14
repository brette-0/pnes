#pragma once

#include <pnes/video.hpp>
#include STRCAT(../../gen/options/GEN_TARGET_DIR/options.hpp)
#include "../../chr/chr.h"
#include "../../src/graphics/windows.hpp"

namespace ui::impl {
    template <bool orient>
    inline void DrawSliderBody(const u16 pos, const u8 width) {
        if constexpr (!orient) return;
        constexpr u8 LeftEdgeGraphic  = windows::ui + blob::ui::slider_left;
        constexpr u8 RightEdgeGraphic = windows::ui + blob::ui::slider_right;
        constexpr u8 MiddleGraphic    = windows::ui + blob::ui::slider_middle;

        ppu::WriteSingleToNameTable(pos, LeftEdgeGraphic);
        ppu::WriteRepeatedToNameTable(pos, MiddleGraphic, width - 2, 0);
        ppu::WriteSingleToNameTable(pos + width, RightEdgeGraphic);
    }

    // Clear/Draw rather than one UpdateSlider(pos, amt, lastAmt): the hooks
    // below no longer hold both positions at once. `amount` is the slider's
    // only value byte, so moving a handle reads it, clears the cell it names,
    // overwrites it, and draws the cell it now names -- two one-cell writes
    // around a single store, each of which wants its own call.
    template <bool orient>
    inline void ClearSliderHandle(const u16 pos, const u8 amt) {
        if constexpr (!orient) return;
        constexpr u8 MiddleGraphic = windows::ui + blob::ui::slider_middle;
        ppu::WriteSingleToNameTable(pos + amt, MiddleGraphic);
    }

    template <bool orient>
    inline void DrawSliderHandle(const u16 pos, const u8 amt) {
        if constexpr (!orient) return;
        constexpr u8 ActiveGraphic = windows::ui + blob::ui::slider_cursor;
        ppu::WriteSingleToNameTable(pos + amt, ActiveGraphic);
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
            ui::impl::DrawSliderBody<!vertical>(pos + 1, r.size.x + 2);

            // Whatever `amount` already holds IS the starting position -- uitk
            // allocates that byte, so a caller wanting a non-zero default
            // writes it before the first Draw() instead of passing it in. The
            // guard is the same one DrawChange() applies: an out-of-range
            // value leaves the track drawn and the handle off it, rather than
            // writing a cell outside the slider's own footprint.
            const u8 amt = amount;
            if (amt > r.size.x) return;
            ui::impl::DrawSliderHandle<!vertical>(pos + 1, amt);
        }

        inline void DrawChange(const u8 amt) {
            const Rect r = rect;
            if (amt > r.size.x) return;    // caps underflow and overflow

            // Clear where the handle is, move the value, draw where it now is.
            // `amount` is atomic (volatile on NES), so it is loaded once into
            // `last` rather than read again for the second write.
            const u8 last = amount;
            ui::impl::ClearSliderHandle<!vertical>(pos + 1, last);
            amount = amt;
            ui::impl::DrawSliderHandle<!vertical>(pos + 1, amt);
        }
    }

    namespace SFXSlider {
        // See musicSlider above -- same prohibition, checked per slider,
        // because the axis is per slider.
        static_assert(!vertical, "vertical sliders are not implemented in this project -- "
                                  "untick Vertical on this slider in uitk, or implement the "
                                  "column-walking path in ui::impl");

        inline void Draw() {
            const Rect r = rect;
            SFXText::Draw();
            ui::impl::DrawSliderBody<!vertical>(pos + 1, r.size.x + 2);

            const u8 amt = amount;
            if (amt > r.size.x) return;
            ui::impl::DrawSliderHandle<!vertical>(pos + 1, amt);
        }

        inline void DrawChange(const u8 amt) {
            const Rect r = rect;
            if (amt > r.size.x) return;    // caps underflow and overflow

            const u8 last = amount;
            ui::impl::ClearSliderHandle<!vertical>(pos + 1, last);
            amount = amt;
            ui::impl::DrawSliderHandle<!vertical>(pos + 1, amt);
        }
    }
#endif
}
