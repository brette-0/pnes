#pragma once
#include <platform-nes/types.hpp>
#include <platform-nes/technology.hpp>   // AI
#include <intsh>

using namespace br0::intsh;

// non boolean options in UI

namespace ui::slider {
    template <bool vertical>
    class TileSlider {
    public:
        // Stores layout/state -- touches nothing PPU-side, safe to run
        // while rendering/NMI is live. Draw() below does the actual
        // nametable write, whenever that's safe.
        TileSlider(
            vec2<u16> pos, u8 size,
            u8 defaultPosition,
            u8 unselectedGraphic, u8 selectedGraphic,
            u8 (*post)(u8 inputs)
        );

        // Writes the tile for the current selectedPosition -- separate
        // from construction so it can run wherever it's actually safe to
        // poke the PPU, same Make/Draw split as text::Draw. Slider body is
        // presumed already drawn, not the
        // slider piece itself -- does not edit attributes, as we presume
        // an attribute has 16x16 finity.
        AI auto Draw() -> void;

        // Writes the erase-old/draw-new ops at *buf -- 3 bytes each
        // (addr-hi, addr-lo, val) -- and advances buf past them, instead of
        // touching the PPU directly. buf must point into a caller-owned
        // buffer with enough room left (2 ops = 6 bytes).
        auto Pass(u8 inputs, u8*& buf) -> void;

    private:
        void Move(i8 amt, u8*& buf);

        vec2<u16> pos;
        u8 size;
        u8 unselectedGraphic;
        u8 selectedGraphic;
        u8 selectedPosition;
        u8 (*post)(u8 inputs);
    };
}