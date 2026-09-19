#pragma once

#include <platform-nes/technology.hpp>
#include "charmaps.hpp"   // charmap_generic must be in scope to map the string
#include "../banks.hpp"   // LEVEL_GRAPHICS

// Strings mapped through the `generic` charmap (see charmaps.hpp). Each
// character is mapped at compile time and the result is folded by the linker
// to a single std::array in rodata. Consumers just include this header; there
// is no separate definition TU. `sizeof(name)` / SIZED_OBJ(name) give the
// (unterminated) character count.
//
// LEVEL_GRAPHICS, not the plain ::STRING macro expansion -- level's own text,
// same bank as its metatiles/palette. See ::level_graphics_tag.
LEVEL_GRAPHICS inline constexpr auto msg_mary = ::tech::nes_str::encode<charmap_generic>("MARY");


TITLE_DATA inline constexpr auto msg_title  = ::tech::nes_str::encode<charmap_generic>("SUPER MARY SISTERS");

// Title menu, as one single-choice-menu buffer: '\n' (mapped to 0 in
// charmap_generic, see charmaps.hpp) marks the boundary between options.
// PC targets get a fourth, Quit option; consoles have no OS to quit back to.
#if defined(TARGET_MACOS) || defined(TARGET_WINDOWS) || defined(TARGET_LINUX)
TITLE_DATA inline constexpr auto msg_menu = ::tech::nes_str::encode<charmap_generic>("NEW GAME\nCONTINUE\nOPTIONS\nQUIT");
#else
TITLE_DATA inline constexpr auto msg_menu = ::tech::nes_str::encode<charmap_generic>("NEW GAME\nCONTINUE\nOPTIONS");
#endif

// Play-mode choice, same '\n'-as-boundary convention as msg_menu. NES has no
// network stack to drive a LAN option -- other targets get it, plus the
// distinction between local and networked multiplayer.
#if defined(TARGET_NES)
TITLE_DATA inline constexpr auto msg_playMode = ::tech::nes_str::encode<charmap_generic>("SINGLEPLAYER\nMULTIPLAYER");
#else
TITLE_DATA inline constexpr auto msg_playMode = ::tech::nes_str::encode<charmap_generic>("SINGLEPLAYER\nLOCAL MULTIPLAYER\nLAN MULTIPLAYER");
#endif
