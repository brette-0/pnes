#pragma once

#include <intsh>

#include <platform-nes/technology.hpp>
#include <platform-nes/types.hpp>
#include <platform-nes/video.hpp>

using namespace br0::intsh;

// text boxes

namespace ui::text {
    enum Alignment : u8 {
        Left,
        Centre,
        Right
    };


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
    AI void Draw(const buffer<u8*>* chunks, vec2<u16> pos, vec2<u8> box, Alignment align);

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
     *  Both overloads are defined here (not in text.cpp), and both take an
     *  explicit `inline` alongside ::AI: as free functions (not class
     *  members, which are implicitly inline when defined in-class) included
     *  by more than one .cpp, they'd otherwise be an ODR violation -- each
     *  including TU would emit its own external-linkage definition. `inline`
     *  gives them vague (COMDAT) linkage instead, which is also what GCC's
     *  always_inline needs to accept a body under LTO -- see ::AI's own
     *  comment in technology.hpp.
     */
    AI void Draw(const buffer<u8*>* rows, u16 address, vec2<u8> box, Alignment align);


    AI inline void Draw(const buffer<u8*>* rows, const u16 address, const vec2<u8> box, const Alignment align) {
        ppu::WriteFromBufferToNameTable(address, rows[0].addr, rows[0].size, 0);
        u16 rowAddr = address;
        for (u8 i = 1; i < box.y; i++) {
            rowAddr += video::viewport_tx();

            u16 useAddr;
            switch (align) {
                case Left:
                default:
                    useAddr = rowAddr;
                    break;

                case Right:
                    useAddr = rowAddr + (box.x - rows[i].size);
                    break;

                case Centre:
                    useAddr = rowAddr + (box.x - rows[i].size) / 2;
                    break;
            }

            ppu::WriteFromBufferToNameTable(useAddr, rows[i].addr, rows[i].size, 0);
        }
    }

    AI inline void Draw(const buffer<u8*>* chunks, const vec2<u16> pos, const vec2<u8> box, const Alignment align) {
        Draw(chunks, ppu::CartesianToAddress(pos), box, align);
    }
}
