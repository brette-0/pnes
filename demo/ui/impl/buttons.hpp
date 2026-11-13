#pragma once

#include <pnes/types.hpp>
#include <intsh>

#include "../../chr/chr.h"
#include "../../src/graphics/windows.hpp"
#include STRCAT(../../gen/options/GEN_TARGET_DIR/options.hpp)

// hand written uitk button impls
namespace ui::impl {
    enum eButtonState : u8 {
        Enabled,
        Disabled
    };

    constexpr void DrawTextBox(const u16 pos, const vec2<u8>& box) {
        // TODO: create actual graphics and use it normally
        constexpr u8 ULCornerGraphic   = windows::ui + blob::ui::box_ul;
        constexpr u8 URCornerGraphic   = windows::ui + blob::ui::box_ur;
        constexpr u8 BLCornerGraphic   = windows::ui + blob::ui::box_bl;
        constexpr u8 BRCornerGraphic   = windows::ui + blob::ui::box_br;
        constexpr u8 LeftEdgeGraphic   = windows::ui + blob::ui::box_vertical;
        constexpr u8 RightEdgeGraphic  = windows::ui + blob::ui::box_vertical;
        constexpr u8 TopEdgeGraphic    = windows::ui + blob::ui::box_horizontal;
        constexpr u8 BottomEdgeGraphic = windows::ui + blob::ui::box_horizontal;

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

    }
}

// impl for gen code
namespace gen::options {
    // `pos` is already the button's PPU address -- uitk resolves it in Make(),
    // so there is no CartesianToAddress call left to pay here. `rect` is copied
    // out whole before its size is read: on a runtime-viewport target it is
    // `atomic Rect`, i.e. volatile, so reading it goes through Rect's volatile
    // copy constructor once rather than per field.
    //
    // One Draw() per button, dispatching on the generated Enabled() -- uitk no
    // longer emits a DrawEnabled()/DrawDisabled() pair with a generated branch
    // over it, because a button's two states are two appearances of one widget
    // and the branch belongs where the appearance is decided (see
    // genButtonDecl). `inline`, so this header stays includable from more than
    // one TU; add AI to a definition that should be force-inlined at its call
    // sites (::AI in technology.hpp).
    namespace reduceFlashesButton {
        inline void Draw() {
            const Rect r = rect;
            ui::impl::DrawTextBox(pos, r.size);
            ui::impl::UpdateTextBox(Enabled() ? ui::impl::Enabled : ui::impl::Disabled, pos, r.size);
        }
    }

#if TARGET_GC
    namespace enableWidescreenButton {
        inline void Draw() {
            const Rect r = rect;
            ui::impl::DrawTextBox(pos, r.size);
            ui::impl::UpdateTextBox(Enabled() ? ui::impl::Enabled : ui::impl::Disabled, pos, r.size);
        }
    }
#endif
}
