#pragma once

#include <platform-nes/types.hpp>
#include <intsh>

#include "../../src/graphics/graphics.hpp"
#include STRCAT(../../gen/options/GEN_TARGET_DIR/options.hpp)

// hand written uitk button impls
namespace ui::impl {
    enum eButtonState {
        DisabledUnselected,
        DisabledSelected,
        EnabledUnselected,
        EnabledSelected
    };

    constexpr void DrawDotBoxDisabled(const u16 pos) {
        ppu::WriteSingleToNameTable(pos, chrUnselected_tile);
    }

    constexpr void DrawDotBoxEnabled(const u16 pos) {
        ppu::WriteSingleToNameTable(pos, chrSelected_tile);
    }

    constexpr void DrawTextBox(const u16 pos, const vec2<u8>& box) {
        // TODO: create actual graphics and use it normally
        constexpr u8 ULCornerGraphic   = charmap_generic('X');
        constexpr u8 URCornerGraphic   = charmap_generic('X');
        constexpr u8 BLCornerGraphic   = charmap_generic('X');
        constexpr u8 BRCornerGraphic   = charmap_generic('X');
        constexpr u8 LeftEdgeGraphic   = charmap_generic('O');
        constexpr u8 RightEdgeGraphic  = charmap_generic('O');
        constexpr u8 TopEdgeGraphic    = charmap_generic('O');
        constexpr u8 BottomEdgeGraphic = charmap_generic('O');

        // UL
        ppu::WriteSingleToNameTable(pos, ULCornerGraphic);

        // UR
        ppu::WriteSingleToNameTable(pos + box.x - 1, URCornerGraphic);

        // BL
        ppu::WriteSingleToNameTable(pos + video::viewport_tx() * (box.y - 1), BLCornerGraphic);

        // BR
        ppu::WriteSingleToNameTable(pos + box.x + video::viewport_tx() * (box.y - 1) - 1, BRCornerGraphic);

        // top row
        ppu::WriteRepeatedToNameTable(pos + 1, TopEdgeGraphic, box.x - 2, 0);

        // bottom row
        ppu::WriteRepeatedToNameTable(
            pos + video::viewport_tx() * (box.y - 1) + 1, BottomEdgeGraphic,
            box.x - 2, 0
        );

        // left edge
        ppu::WriteRepeatedToNameTable(
            pos + video::viewport_tx(), LeftEdgeGraphic,
            box.y - 2, 1
        );

        // right edge
        ppu::WriteRepeatedToNameTable(
            pos + video::viewport_tx() + box.x - 1, RightEdgeGraphic,
            box.y - 2, 1
        );
    }

    constexpr void UpdateTextBox(const eButtonState state, const u16 pos, const vec2<u8> &box) {
        // use attribute tables to change colours for state
    }
}

// impl for gen code
namespace gen::options {
    inline void DrawEnabled_reduceFlashesButton() {
        const u16 addr = ppu::CartesianToAddress(reduceFlashesButton_pos);
        ui::impl::DrawTextBox(addr, reduceFlashesButton_box);
        ui::impl::UpdateTextBox(ui::impl::EnabledUnselected, addr, reduceFlashesButton_box);
    }

    inline void DrawDisabled_reduceFlashesButton() {
        const u16 addr = ppu::CartesianToAddress(reduceFlashesButton_pos);
        ui::impl::DrawTextBox(addr, reduceFlashesButton_box);
        ui::impl::UpdateTextBox(ui::impl::DisabledUnselected, addr, reduceFlashesButton_box);
    }

#if TARGET_GC
    inline void DrawEnabled_enableWidescreenButton() {
        const u16 addr = ppu::CartesianToAddress(enableWidescreenButton_pos);
        ui::impl::DrawTextBox(addr, enableWidescreenButton_box);
        ui::impl::UpdateTextBox(ui::impl::EnabledUnselected, addr, enableWidescreenButton_box);
    }

    inline void DrawDisabled_enableWidescreenButton() {
        const u16 addr = ppu::CartesianToAddress(enableWidescreenButton_pos);
        ui::impl::DrawTextBox(addr, enableWidescreenButton_box);
        ui::impl::UpdateTextBox(ui::impl::DisabledUnselected, addr, enableWidescreenButton_box);
    }
#endif
}