#include <platform-nes/extras/ui/singlechoice.hpp>

#include "platform-nes/input.hpp"
#include "platform-nes/video.hpp"
#include "platform-nes/technology.hpp"   // CREATE_SEGMENT_KEYWORD / MODULE_PLACEMENT

// WHERE THIS MODULE'S CODE GOES -- see PLATFORM_NES_UI_SECTION's own comment
// in CMakeLists.txt/local.cmake.example. Compulsory, like audio's: no default,
// so the placement decision is never a silent guess. Guarded the same way
// MODULE_PLACEMENT itself is (technology.hpp): this file lives under
// src/all/, compiled for every backend, not just NES-banked ones, so the
// requirement only applies where placement is even possible.
#if defined(TARGET_NES) && defined(NES_MAPPER_BANKSWITCHED)
#ifndef PLATFORM_NES_UI_SECTION
#error "PLATFORM_NES_UI_SECTION is not set. It names the ELF section \
src/all/extras/ui/*.cpp is placed into. Set it from CMake -- \
local.cmake.example has a worked example."
#endif
#endif
#define UI_BANK MODULE_PLACEMENT(PLATFORM_NES_UI_SECTION) MINSIZE

namespace ui::choice {
    namespace {
        NI u16 ComputeOptionAddr(const u16 arrowCol, const u16 y) {
            return ppu::CartesianToAddress({arrowCol, y});
        }
    }

    SingleChoice::~SingleChoice() {
        delete[] optionAddr;
    }

    UI_BANK SingleChoice::SingleChoice(
            const u8 nOptions, const u8 defaultOption
        ) : option(defaultOption), optionAddr(new u16[nOptions]),
            nOptions(nOptions) {
    }

    UI_BANK buffer<u8*>* SingleChoice::Make(
            const u8* buff, const u8 sBuff, const vec2<u16> pos, const vec2<u8> box,
            const u8 wordSplitter, const u8 optionSplitter,
            u16* const optionAddr, const u8 nOptions
        ) {
        const auto chunks = new buffer<u8*>[box.y]();

        // Arrow sits one tile left of the text with a blank tile of gap in
        // between -- same layout title.cpp's menu arrow uses.
        const u16 arrowCol = pos.x - 2;

        u8 cursor = 0;
        u8 row    = 0;

        for (u8 opt = 0; opt < nOptions && cursor < sBuff; opt++) {
            // Pay the (x,y)->address divide+modulo (ppu::CartesianToAddress,
            // internally the same cost ppu::WriteSingleToNameTable(vec2,u8)
            // would pay per call) once per option, here in Make() -- not on
            // every Next()/Previous() call, which runs inside the vblank
            // window.
            optionAddr[opt] = ComputeOptionAddr(arrowCol, static_cast<u16>(pos.y + row));

            // this option's chunk runs up to the next optionSplitter, or the
            // end of the buffer for the last option
            u8 chunkEnd = cursor;
            while (chunkEnd < sBuff && *(buff + chunkEnd) != optionSplitter) chunkEnd++;

            // word-wrap this option's chunk exactly like text::Make, except
            // the row cursor carries on across options instead of resetting
            u8 last = cursor;
            while (last < chunkEnd && row < box.y) {
                u8   lastWhite  = last;
                bool foundWhite = false;
                u8   c          = last;

                for (; c < chunkEnd && c - last < box.x; c++) {
                    if (*(buff + c) == wordSplitter) {
                        lastWhite  = c;
                        foundWhite = true;
                    }
                }

                const u8 end = (c == chunkEnd) ? c : (foundWhite ? lastWhite : c);

                chunks[row].addr = const_cast<u8*>(buff + last);
                chunks[row].size = end - last;
                row++;

                last = (end < chunkEnd) ? end + 1 : end;
            }

            // step past the optionSplitter itself, into the next option's text
            cursor = (chunkEnd < sBuff) ? chunkEnd + 1 : chunkEnd;
        }

        return chunks;
    }

    UI_BANK buffer<u8*>* SingleChoice::Make(
            const u8* buff, const u8 sBuff, const vec2<u16> pos, const vec2<u8> box,
            const u8 wordSplitter, const u8 optionSplitter
        ) {
        return Make(buff, sBuff, pos, box, wordSplitter, optionSplitter, optionAddr, nOptions);
    }
}
