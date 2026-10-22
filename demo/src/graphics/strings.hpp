#pragma once

#include <pnes/technology.hpp>
#include "charmaps.hpp"   // charmap_generic must be in scope to map the string
#include "../banks.hpp"   // LEVEL_GRAPHICS

// Strings mapped through the `generic` charmap (see charmaps.hpp). Each
// character is mapped at compile time and the result is folded by the linker
// to a single std::array in rodata. Consumers just include this header; there
// is no separate definition TU.
//
// Each string is terminated, and that terminator is itself a mapped tile, so
// `sizeof(name)` is one MORE than the character count: SIZED_STR(name) /
// ::tech::nes_str::length(name) give the text, SIZED_OBJ(name) the whole
// buffer.
//
// LEVEL_GRAPHICS, not a plain `inline constexpr` -- level's own text, same
// bank as its metatiles/palette. See ::level_graphics_tag.
LEVEL_GRAPHICS inline constexpr auto msg_mary = charmap_generic.encode("MARY");


TITLE_DATA inline constexpr auto msg_title  = charmap_generic.encode("SUPER MARY SISTERS");

// The title menu and the play-mode menu are generated from demo/ui/title.uis
// and demo/ui/gameOptions.uis (demo/gen/), not hand-written here.
