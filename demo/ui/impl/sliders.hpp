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

    inline void UpdateSlider(const u16 pos, const u8 amt, const u8 lastAmt) {
        constexpr u8 MiddleGraphic = blob::ui::slider_middle;
        constexpr u8 ActiveGraphic = blob::ui::slider_cursor;
        ppu::WriteSingleToNameTable(pos + lastAmt, MiddleGraphic);
        ppu::WriteSingleToNameTable(pos + amt,     ActiveGraphic);
    }
}

namespace gen::options {
#ifndef TARGET_NES
    // ReSharper disable once CppNonInlineFunctionDefinitionInHeaderFile
    void Draw_musicSlider(const u8 defaultAmt) {
        ui::impl::DrawSliderBody(
            ppu::CartesianToAddress(musicSlider_pos) + 1, musicSlider_box.x + 2
        );

        DrawChange_musicSlider(defaultAmt);
    }

    // ReSharper disable once CppNonInlineFunctionDefinitionInHeaderFile
    void DrawChange_musicSlider(const u8 amt) {
        if (amt > musicSlider_box.x) return;    // caps underflow and overflow
        ui::impl::UpdateSlider(
            ppu::CartesianToAddress(musicSlider_pos) + 1, amt,musicSlider_amount
        );
        musicSlider_amount = amt;
    }
#endif
}