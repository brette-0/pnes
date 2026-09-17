#pragma once

#include <intsh>

#include <platform-nes/technology.hpp>
#include <platform-nes/types.hpp>
#include <platform-nes/video.hpp>

using namespace br0::intsh;

// text boxes

namespace ui::text {
    /*
     *  Same wrapping rule as Draw, but instead of writing rows to the
     *  nametable it records where each row would have started and how
     *  long it is.
     *
     *  Returns a heap-allocated array of box.y buffer<u8*> entries --
     *  one per row, caller owns it (delete[] once done). addr points
     *  into buff at that row's first byte, size is the row's length in
     *  bytes, splitter excluded. Rows left unused because the buffer
     *  ran out first are zeroed (addr == nullptr, size == 0).
     */
    NI buffer<u8*>* Make(const u8* buff, u8 sBuff, vec2<u8> box, u8 splitter);

    /*
     *  Draws a Make() result: writes chunks[row] to nametable row
     *  pos.y + row for row in [0, boxY), stopping early the first time
     *  it hits a row Make left zeroed (addr == nullptr) -- i.e. the
     *  buffer ran out before boxY rows were filled.
     *
     *  Does not take ownership of chunks -- caller allocated it via
     *  Make() and is responsible for delete[]ing it, whether or not
     *  this broke out early.
     *
     *  Pays (x,y)->address (a divide+modulo) exactly once, up front, then
     *  walks rows by a plain +32 add -- see the address overload below if
     *  even that one division doesn't belong on the caller's hot path.
     */
    AI void Draw(const buffer<u8*>* chunks, const vec2<u16> pos, const u8 boxY);

    /*
     *  Address overload of Draw(): @p address is row 0's nametable address
     *  (see ::ppu::CartesianToAddress), for a caller that already has it
     *  precomputed -- e.g. from inside an ISR, where the divide+modulo the
     *  vec2 overload above pays isn't affordable at all. Every later row is
     *  address + row*32 (one nametable tile-row), so this does zero
     *  division of its own.
     *
     *  Only correct within a single nametable page (address's row < 30) --
     *  callers whose box could cross that boundary need the vec2 overload,
     *  which still gets it right via CartesianToAddress.
     *
     *  Both overloads are defined here (not in text.cpp) -- see ::AI's own
     *  comment in technology.hpp for why that's safe despite being included
     *  by more than one .cpp: AI itself carries `inline`, not just
     *  `always_inline`.
     */
    AI void Draw(const buffer<u8*>* rows, const u16 address, const u8 height) {
        ppu::WriteFromBufferToNameTable(address, rows[0].addr, rows[0].size, 0);
        u16 rowAddr = address;
        for (u8 i = 1; i < height; i++) {
            rowAddr += video::viewport_tx();
            ppu::WriteFromBufferToNameTable(rowAddr, rows[i].addr, rows[i].size, i);
        }
    }

    AI void Draw(const buffer<u8*>* chunks, const vec2<u16> pos, const u8 boxY) {
        Draw(chunks, ppu::CartesianToAddress(pos), boxY);
    }
}
