#pragma once

#include <pnes/video.hpp>
#include STRCAT(../../gen/options/GEN_TARGET_DIR/options.hpp)
#include "../../chr/chr.h"

namespace ui::impl {
    inline void DrawSliderBody(const u16 pos, const u8 width) {
        constexpr u8 LeftEdgeGraphic  = blob::ui::slider_left;
        constexpr u8 RightEdgeGraphic = blob::ui::slider_right;
        constexpr u8 MiddleGraphic    = blob::ui::slider_middle;

        ppu::WriteSingleToNameTable(pos, LeftEdgeGraphic);
        ppu::WriteRepeatedToNameTable(pos, MiddleGraphic, width - 2, 0);
        ppu::WriteSingleToNameTable(pos + width, RightEdgeGraphic);
    }

    // Clear/Draw rather than one UpdateSlider(pos, amt, lastAmt): the hooks
    // below no longer hold both positions at once. `amount` is the slider's
    // only value byte, so moving a handle reads it, clears the cell it names,
    // overwrites it, and draws the cell it now names -- two one-cell writes
    // around a single store, each of which wants its own call.
    inline void ClearSliderHandle(const u16 pos, const u8 amt) {
        constexpr u8 MiddleGraphic = blob::ui::slider_middle;
        ppu::WriteSingleToNameTable(pos + amt, MiddleGraphic);
    }

    inline void DrawSliderHandle(const u16 pos, const u8 amt) {
        constexpr u8 ActiveGraphic = blob::ui::slider_cursor;
        ppu::WriteSingleToNameTable(pos + amt, ActiveGraphic);
    }
}

namespace gen::options {
#ifndef TARGET_NES
    // `pos` is already the PPU address of the slider's anchor -- uitk resolves
    // it in Make(), so there is no CartesianToAddress call left to pay here.
    // `rect` is copied out whole before its size is read: on a runtime-viewport
    // target it is `atomic Rect`, i.e. volatile, so reading it goes through
    // Rect's volatile copy constructor once rather than per field.
    namespace musicSlider {
        // ReSharper disable once CppNonInlineFunctionDefinitionInHeaderFile
        void Draw() {
            const Rect r = rect;
            ui::impl::DrawSliderBody(pos + 1, r.size.x + 2);

            // Whatever `amount` already holds IS the starting position -- uitk
            // allocates that byte, so a caller wanting a non-zero default
            // writes it before the first Draw() instead of passing it in. The
            // guard is the same one DrawChange() applies: an out-of-range
            // value leaves the track drawn and the handle off it, rather than
            // writing a cell outside the slider's own footprint.
            const u8 amt = amount;
            if (amt > r.size.x) return;
            ui::impl::DrawSliderHandle(pos + 1, amt);
        }

        // ReSharper disable once CppNonInlineFunctionDefinitionInHeaderFile
        void DrawChange(const u8 amt) {
            const Rect r = rect;
            if (amt > r.size.x) return;    // caps underflow and overflow

            // Clear where the handle is, move the value, draw where it now is.
            // `amount` is atomic (volatile on NES), so it is loaded once into
            // `last` rather than read again for the second write.
            const u8 last = amount;
            ui::impl::ClearSliderHandle(pos + 1, last);
            amount = amt;
            ui::impl::DrawSliderHandle(pos + 1, amt);
        }
    }
#endif
}
