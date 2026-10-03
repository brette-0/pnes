#pragma once

#include <platform-nes/types.hpp>
#include <intsh>

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
        // TODO: create actual graphics and use it normally
        constexpr u8 DisabledGraphic = 0x00;
        ppu::WriteSingleToNameTable(pos, DisabledGraphic);
    }

    constexpr void DrawDotBoxEnabled(const u16 pos) {
        // TODO: create actual graphics and use it normally
        constexpr u8 EnabledGraphic = 0x00;
        ppu::WriteSingleToNameTable(pos, EnabledGraphic);
    }

    constexpr void DrawTextBox(const u16 pos, const vec2<u8>& box) {
        // TODO: create actual graphics and use it normally
        constexpr u8 ULCornerGraphic   = 0x00;
        constexpr u8 URCornerGraphic   = 0x00;
        constexpr u8 BLCornerGraphic   = 0x00;
        constexpr u8 BRCornerGraphic   = 0x00;
        constexpr u8 LeftEdgeGraphic   = 0x00;
        constexpr u8 RightEdgeGraphic  = 0x00;
        constexpr u8 TopEdgeGraphic    = 0x00;
        constexpr u8 BottomEdgeGraphic = 0x00;

        ppu::WriteSingleToNameTable(pos, ULCornerGraphic);
        ppu::WriteRepeatedToNameTable(pos + 1, TopEdgeGraphic, box.x - 2, 0);
        ppu::WriteSingleToNameTable(pos + box.x, URCornerGraphic);
        ppu::WriteRepeatedToNameTable(
            pos + video::viewport_tx() * box.y, BottomEdgeGraphic,
            box.x - 2, 0
        );
        ppu::WriteSingleToNameTable(pos + video::viewport_tx() * box.x, BLCornerGraphic);
        ppu::WriteRepeatedToNameTable(
            pos + video::viewport_tx(), LeftEdgeGraphic,
            box.y - 2, 1
        );
        ppu::WriteRepeatedToNameTable(
            pos + video::viewport_tx() + box.x, RightEdgeGraphic,
            box.y - 2, 1
        );
        ppu::WriteSingleToNameTable(pos + box.x + video::viewport_tx() * box.x, BRCornerGraphic);
    }

    constexpr void UpdateTextBox(const eButtonState state, const u16 pos, const vec2<u8> &box) {
        // use attribute tables to change colours for state
    }
}

// impl for gen code
namespace gen::options {
    inline void DrawDisabled_reduceFlashesButton() {
        ui::impl::UpdateTextBox(ui::impl::DisabledUnselected, ppu::CartesianToAddress(reduceFlashesButton_pos), reduceFlashesButton_box);
    }

#if TARGET_GC
    inline void DrawDisabled_enableWidescreenButton() {
        ui::impl::UpdateTextBox(ui::impl::DisabledUnselected, ppu::CartesianToAddress(enableWidescreenButton_pos), enableWidescreenButton_box);
    }
#endif
}