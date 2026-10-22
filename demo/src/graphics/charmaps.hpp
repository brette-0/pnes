#pragma once

#include <pnes/technology.hpp>
#include "../../chr/chr.h"
// Default character map: source character -> CHR tile byte.
//
// One rule per contiguous run of tiles, which is all the font is: a blank, then
// the digits, then the letters. Any TU that encodes a string through `generic`
// needs this object in scope, so it includes this header (strings.hpp pulls it
// in for that reason). Being `inline constexpr` -- and only ever
// constant-evaluated -- it can be included by any number of TUs with no
// ODR/LTO trouble and emits nothing of itself into the ROM.
//
// '\0' is mapped like any other character, because an encoded string carries
// its terminator and that terminator is a real tile (see Charmap::encode). It
// gets its OWN tile rather than sharing the space tile: ui::text::Make finds
// the end of a runtime buffer by scanning for it, which a terminator equal to
// the splitter would break at the first space in the text. The cost of that
// choice is that a terminator drawn to the nametable by mistake is visible
// rather than invisible -- SIZED_STR / nes_str::length exist so it isn't.
inline constexpr auto charmap_generic = ::tech::nes_str::charmap({
    { '\0',     blob::ui::empty },           // terminator; its own tile, scannable -- see above
    { ' ',      blob::ui::hud_whitespace },   // dedicated blank tile, not part of the font run
    { '0', '9', blob::ui::font + 0x00 },
    { 'A', 'Z', blob::ui::font + 0x0a },
});
