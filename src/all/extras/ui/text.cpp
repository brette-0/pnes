#include <pnes/extras/ui/text.hpp>

#include "pnes/video.hpp"
#include "pnes/technology.hpp"   // CREATE_SEGMENT_KEYWORD / MODULE_PLACEMENT

// WHERE THIS MODULE'S CODE GOES -- see PNES_UI_SECTION's own comment
// in CMakeLists.txt/local.cmake.example. Compulsory, like audio's: no default,
// so the placement decision is never a silent guess. Guarded the same way
// MODULE_PLACEMENT itself is (technology.hpp): this file lives under
// src/all/, compiled for every backend, not just NES-banked ones, so the
// requirement only applies where placement is even possible.
#if defined(TARGET_NES) && defined(NES_MAPPER_BANKSWITCHED)
#ifndef PNES_UI_SECTION
#error "PNES_UI_SECTION is not set. It names the ELF section \
src/all/extras/ui/*.cpp is placed into. Set it from CMake -- \
local.cmake.example has a worked example."
#endif
#endif
#define UI_BANK MODULE_PLACEMENT(PNES_UI_SECTION) MINSIZE

namespace ui::text::detail {
    UI_BANK textBuffer* Make(const u8* buff, const vec2<u8> box, const u8 splitter, const u8 terminator) {
        const auto rows = new textBuffer[box.y]();

        // The length the caller used to pass, measured here instead. Both
        // bytes are tiles (see ui::text::Make in text.hpp), so this
        // compares against the encoded terminator, not against 0x00 -- and
        // the charmap has to give the terminator a tile of its own, or the
        // first splitter in the text would end the string here.
        //
        // Capped at 960 -- 32*30, the most tiles any nametable box can show,
        // so no reachable text is cut short (the old 255 cap truncated a box
        // taller than 32x8, and a full-screen one at barely a quarter of its
        // capacity). The cap is a constant rather than box.x*box.y because
        // the 6502 has no multiply: the product form cost 78 bytes more than
        // this one, measured.
        //
        // The guard still has to be tight, not merely present: an unterminated
        // buffer scanning on through $2000-$401f would not just read garbage
        // but CHANGE PPU/APU state -- reading $2002 clears the vblank flag,
        // $2007 steps the VRAM address -- so a 0xffff guard would be no guard
        // at all on this target, however much u16 sBuff can address.
        //
        // sBuff is u16, so every position compared against it is too: a u8
        // cursor would wrap to 0 at 255 and spin forever on text longer than
        // that, which is the whole case this widening exists to serve. Only
        // textBuffer::size stays u8, bounded by box.x a row at a time.
        u16 sBuff = 0;
        while (sBuff != 960 && *(buff + sBuff) != terminator) sBuff++;

        u16 cursor = 0;
        u16 last   = 0;
        u8  row    = 0;

        while (cursor < sBuff && row < box.y) {
            u16  lastWhite  = last;
            bool foundWhite = false;

            for (; cursor < sBuff && cursor - last < box.x; cursor++) {
                if (*(buff + cursor) == splitter) {
                    lastWhite  = cursor;
                    foundWhite = true;
                }
            }

            const u16 end = (cursor == sBuff) ? cursor : (foundWhite ? lastWhite : cursor);

            rows[row].addr = const_cast<u8*>(buff + last);
            rows[row].size = static_cast<u8>(end - last);
            row++;

            last   = (end < sBuff) ? end + 1 : end;
            cursor = last;
        }

        return rows;
    }
}

namespace ui::text {
    // rowAddr steps by one nametable row per iteration. One nametable is this
    // target's own viewport width, floored at the NES-native 32 minimum
    // (matching ::emu::ComputeNtGeometry, src/emu/emu.hpp, and
    // ::CartesianToAddress's own internal xy_to_nt_addr, src/emu/ppu.cpp --
    // NOT a bare video::viewport_tx() call, since a target whose viewport is
    // narrower than 32 crops a still-32-wide background rather than shrinking
    // it (GBA/NDS/DSi -- see TARGET_GBA's own viewport_tx() doc comment,
    // video.hpp), so the row stride there still has to be 32, not less).
    // Stepping by the wrong width jumps the flat address past the row
    // boundary xy_to_nt_addr expects, landing on the wrong row *and* column
    // once decoded back through it.
    UI_BANK void Draw(const textBuffer* rows, const u16 address, const vec2<u8> box, const Alignment align) {
        const u16 rowWidth = video::viewport_tx() < 32 ? 32 : video::viewport_tx();
        u16 rowAddr = address;
        for (u8 i = 0; i < box.y; i++, rowAddr += rowWidth) {
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

    // See Draw's own comment above: same row stride, same reason.
    UI_BANK void Clear(
        const textBuffer* rows, const u16 address, const vec2<u8> box, const Alignment align, const u8 clear
    ) {
        const u16 rowWidth = video::viewport_tx() < 32 ? 32 : video::viewport_tx();
        u16 rowAddr = address;
        for (u8 i = 0; i < box.y; i++, rowAddr += rowWidth) {
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
