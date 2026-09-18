#include <platform-nes/extras/ui/text.hpp>

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

namespace ui::text {
    UI_BANK textBuffer* Make(const u8* buff, const u8 sBuff, const vec2<u8> box, const u8 splitter) {
        const auto rows = new textBuffer[box.y]();

        u8 cursor = 0;
        u8 last   = 0;
        u8 row    = 0;

        while (cursor < sBuff && row < box.y) {
            u8   lastWhite  = last;
            bool foundWhite = false;

            for (; cursor < sBuff && cursor - last < box.x; cursor++) {
                if (*(buff + cursor) == splitter) {
                    lastWhite  = cursor;
                    foundWhite = true;
                }
            }

            const u8 end = (cursor == sBuff) ? cursor : (foundWhite ? lastWhite : cursor);

            rows[row].addr = const_cast<u8*>(buff + last);
            rows[row].size = end - last;
            row++;

            last   = (end < sBuff) ? end + 1 : end;
            cursor = last;
        }

        return rows;
    }

    UI_BANK void Draw(const textBuffer* rows, const u16 address, const vec2<u8> box, const Alignment align) {
        u16 rowAddr = address;
        for (u8 i = 0; i < box.y; i++, rowAddr += video::viewport_tx()) {
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

    UI_BANK void Clear(
        const textBuffer* rows, const u16 address, const vec2<u8> box, const Alignment align, const u8 clear
    ) {
        u16 rowAddr = address;
        for (u8 i = 0; i < box.y; i++, rowAddr += video::viewport_tx()) {
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

            ppu::WriteRepeatedToNameTable(useAddr, clear, rows[i].size, 0);
        }
    }
}
