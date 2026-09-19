#pragma once
#include <platform-nes/types.hpp>
#include <platform-nes/technology.hpp>   // AI/NI
#include <platform-nes/video.hpp>        // ppu::WriteSingleToNameTable
#include <intsh>

using namespace br0::intsh;

namespace ui::button {
    class TickBox {
    public:
        // Computes addr for pos (see ppu::CartesianToAddress) -- touches
        // nothing PPU-side, safe to run while rendering/NMI is live. Draw()
        // below does the actual nametable write, whenever that's safe.
        TickBox(vec2<u16> pos, bool defaultState);

        // Writes the tile for the current state at pos -- separate from
        // construction so it can run wherever it's actually safe to poke
        // the PPU, same Make/Draw split as text::Draw.
        // Defined here, not in tickbox.cpp: ::AI promises the body is
        // copied into every caller, which under GCC + LTO requires the body
        // to be visible at each call site -- see ::AI's own comment in
        // technology.hpp.
        AI auto Draw(const vec2<u16> pos) -> void {
            ppu::WriteSingleToNameTable(pos, enabled);
        }

        // Writes the toggled tile's op at *buf -- 3 bytes (addr-hi, addr-lo,
        // val) -- and advances buf past them, instead of touching the PPU
        // directly. buf must point into a caller-owned buffer with at least
        // 3 bytes left.
        auto Pass(u8 inputs, u8*& buf) -> void;

        bool enabled;
    private:
        // Precomputed VRAM address for pos (see ppu::CartesianToAddress) --
        // TickBox's tile never moves, so this is paid once at construction
        // instead of on every Toggle().
        static NI int Make(vec2<u16> pos);

        // ::AI, not ::UI_BANK: must stay reachable without a bank switch
        // from wherever Pass() calls it -- the two are mutually exclusive
        // (see MODULE_PLACEMENT's own doc comment).
        AI auto Toggle(u8*& buf) -> void {
            *buf++ = static_cast<u8>(addr >> 8);
            *buf++ = static_cast<u8>(addr & 0xFF);
            *buf++ = (enabled ^= true);
        }

        int addr;
    };
}
