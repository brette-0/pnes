#pragma once


namespace ui::impl {
    inline void DrawSliderBody(const u16 pos, const u8 width) {
        constexpr u8 LeftEdgeGraphic  = 0x00;
        constexpr u8 RightEdgeGraphic = 0x00;
        constexpr u8 MiddleGraphic    = 0x00;

        ppu::WriteSingleToNameTable(pos, LeftEdgeGraphic);
        ppu::WriteRepeatedToNameTable(pos, MiddleGraphic, width - 2, 0);
        ppu::WriteSingleToNameTable(pos + width, RightEdgeGraphic);
    }

    inline void UpdateSlider(const u16 pos, const u8 amt, const u8 lastAmt) {
        constexpr u8 MiddleGraphic = 0x00;
        constexpr u8 ActiveGraphic = 0x00;
        ppu::WriteSingleToNameTable(pos + lastAmt, MiddleGraphic);
        ppu::WriteSingleToNameTable(pos + amt,     ActiveGraphic);
    }
}