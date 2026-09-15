#pragma once

#include <intsh>

#include "text.hpp"
#include "platform-nes/types.hpp"
#include "platform-nes/technology.hpp"   // atomic
#include "platform-nes/video.hpp"        // ppu::CartesianToAddress / WriteFromBufferToNameTable
#include "platform-nes/input.hpp"        // input::UP / input::DOWN

using namespace br0::intsh;

namespace ui::choice {
    class SingleChoice {
    public:
        // Allocates optionAddr and stores the callbacks -- touches nothing
        // PPU-side, so this is safe to run while rendering/NMI is live.
        // Layout and the actual nametable writes happen in Draw(), called
        // separately whenever it's actually safe to poke the PPU.
        SingleChoice(u8 nOptions, u8 defaultOption);

        ~SingleChoice();

        // Word-wraps every option's text (same rule as text::Draw, except
        // the row cursor carries on across options instead of resetting)
        // and computes each option's indicator address into optionAddr --
        // touches nothing PPU-side, safe to run while rendering/NMI is
        // live. Static so it can run before -- or entirely without -- a
        // SingleChoice instance, as long as the caller hands it storage
        // for optionAddr (>= nOptions entries).
        //
        // Returns a heap-allocated array of box.y buffer<u8*> entries --
        // one per row, caller owns it (delete[] once done) and hands it
        // to Draw() below. Same layout as text::Make's result.
        static NI buffer<u8*>* Make(
            const u8* buff, u8 sBuff, vec2<u16> pos, vec2<u8> box,
            u8 wordSplitter, u8 optionSplitter,
            u16* optionAddr, u8 nOptions
        );

        // Convenience wrapper over the static Make() using this instance's
        // own optionAddr/nOptions.
        NI auto Make(
            const u8* buff, u8 sBuff, vec2<u16> pos, vec2<u8> box,
            u8 wordSplitter, u8 optionSplitter
        ) -> buffer<u8*>*;

        // Writes a Make() result's chunks to the nametable -- the box's
        // shape only (same early-break-on-nullptr rule as text::Draw). Does
        // NOT touch the selection indicator -- that's the caller's job, via
        // SelectedAddr() below and whatever mechanism (e.g. a scratchpad
        // handed to the NMI) draws the arrow. Does not take ownership of
        // chunks -- caller allocated it via Make() and is responsible for
        // delete[]ing it.
        //
        // Pays (x,y)->address (a divide+modulo) exactly once, up front, then
        // walks rows by a plain +32 add -- see the address overload below if
        // even that one division doesn't belong on the caller's hot path.
        // Defined here, not in singlechoice.cpp: ::AI promises the body is
        // copied into every caller, which under GCC + LTO requires the body
        // to be visible at each call site -- see ::AI's own comment in
        // technology.hpp.
        AI auto Draw(
            const buffer<u8*>* const chunks, const vec2<u16> pos, const u8 boxY
        ) -> void {
            Draw(chunks, ppu::CartesianToAddress(pos), boxY);
        }

        // Address overload of Draw(): @p address is row 0's nametable
        // address (see ::ppu::CartesianToAddress), for a caller that
        // already has it precomputed -- e.g. from inside an ISR, where the
        // divide+modulo the vec2 overload above pays isn't affordable at
        // all. Every later row is address + row*32 (one nametable
        // tile-row), so this does zero division of its own.
        //
        // Only correct within a single nametable page (address's row < 30)
        // -- a caller whose box could cross that boundary needs the vec2
        // overload, which still gets it right via CartesianToAddress.
        AI auto Draw(
            const buffer<u8*>* const chunks, const u16 address, const u8 boxY
        ) -> void {
            u16 rowAddr = address;
            for (u8 row = 0; row < boxY; row++) {
                if (chunks[row].addr == nullptr) {
                    break;
                }

                ppu::WriteFromBufferToNameTable(rowAddr, chunks[row].addr, chunks[row].size, 0);
                rowAddr = static_cast<u16>(rowAddr + 32);
            }
        }

        // Nametable address of the currently-selected option's indicator
        // slot -- what a caller queues (as both clear and write address, on
        // first draw) into whatever mechanism actually pokes the PPU. See
        // Draw()'s own comment: SingleChoice decides nothing about how or
        // when the indicator itself gets drawn.
        NI u16 SelectedAddr() const {
            return optionAddr[option];
        }

        // Template so vertical/horizontal each specialize on option +=/-=
        // without a runtime branch -- defined here, not in singlechoice.cpp:
        // a template member needs its definition visible at every
        // instantiation point, same requirement ::AI states for itself
        // above.
        template <bool vertical>
        AI auto Pass(const u8 inputs) -> void {
            if constexpr (vertical) {
                if      (inputs & input::UP)   { if (option != 0)            option -= 1; }
                else if (inputs & input::DOWN) { if (option != nOptions - 1) option += 1; }
                return;
            }

            if      (inputs & input::UP)   { if (option != 0)            option -= 1; }
            else if (inputs & input::DOWN) { if (option != nOptions - 1) option += 1; }
        }
        atomic u8 option;
    private:
        u16* optionAddr;
        const u8 nOptions;
    };
}