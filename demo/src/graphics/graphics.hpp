#pragma once

// Single source of truth for the demo's CHR tiles.
//
// Each blob is pulled in with #embed, so its size -- and therefore its base
// CHR-tile index `<name>_tile` -- is a compile-time constant usable in
// constexpr tables (see metatiles.cpp / MT_SPLIT). Tile ids are never
// hand-assigned: each chains from the blob before it.
//
//   * Just #include this wherever you use tile ids. The FINAL blob's
//     CHARACTER_ROM_END_FINAL emits the padded 8 KB CHR image as an `inline`
//     object, so the linker folds it to exactly one copy no matter how many
//     TUs include this header
//
// #embed paths are relative to THIS header (demo/src/graphics/).

#include <platform-nes/video.hpp>

// player sprite graphics
CHARACTER_ROM_BEGIN(chrPlayerStanding)
#embed "../../chr/sprites/player/standing.chr"
CHARACTER_ROM_END(chrPlayerStanding, CHR_ORIGIN);

// power sprite graphics
CHARACTER_ROM_BEGIN(chrBerries)
#embed "../../chr/sprites/berries.chr"
CHARACTER_ROM_END(chrBerries, chrPlayerStanding);

CHARACTER_ROM_BEGIN(chrWand)
#embed "../../chr/sprites/wand.chr"
CHARACTER_ROM_END(chrWand, chrBerries);

// enemy sprite graphics
CHARACTER_ROM_BEGIN(chrMushletStanding)
#embed "../../chr/sprites/enemies/mushlet/standing.chr"
CHARACTER_ROM_END_PAD_TO(chrMushletStanding, chrWand, CHR_TILES_PER_TABLE);

// world static tiles
CHARACTER_ROM_BEGIN(chrBush)
#embed "../../chr/tiles/static/bush.chr"
CHARACTER_ROM_END(chrBush, chrMushletStanding);

CHARACTER_ROM_BEGIN(chrLiquid)
#embed "../../chr/tiles/static/liquid.chr"
CHARACTER_ROM_END(chrLiquid, chrBush);

CHARACTER_ROM_BEGIN(chrPipe)
#embed "../../chr/tiles/static/pipe.chr"
CHARACTER_ROM_END(chrPipe, chrLiquid);

CHARACTER_ROM_BEGIN(chrTerrain)
#embed "../../chr/tiles/static/terrain.chr"
CHARACTER_ROM_END(chrTerrain, chrPipe);

CHARACTER_ROM_BEGIN(chrAir)
#embed "../../chr/tiles/static/air.chr"
CHARACTER_ROM_END(chrAir, chrTerrain);

// ui static tiles
CHARACTER_ROM_BEGIN(chrFont)
#embed "../../chr/tiles/static/ui/font.chr"
CHARACTER_ROM_END(chrFont, chrAir);

CHARACTER_ROM_BEGIN(chrHUDCoin)
#embed "../../chr/tiles/static/ui/hud_coin.chr"
CHARACTER_ROM_END(chrHUDCoin, chrFont);

CHARACTER_ROM_BEGIN(chrHUDWhitespace)
#embed "../../chr/tiles/static/ui/hud_whitespace.chr"
CHARACTER_ROM_END(chrHUDWhitespace, chrHUDCoin);

// button toggle-state dot (checked / unchecked)
CHARACTER_ROM_BEGIN(chrSelected)
#embed "../../chr/tiles/static/ui/selected.chr"
CHARACTER_ROM_END(chrSelected, chrHUDWhitespace);

CHARACTER_ROM_BEGIN(chrUnselected)
#embed "../../chr/tiles/static/ui/unselected.chr"
CHARACTER_ROM_END(chrUnselected, chrSelected);

// dedicated blank tile -- the official ' ' glyph (see charmaps.hpp). Kept
// separate from chrFont so it isn't at the mercy of font.chr's own layout.
CHARACTER_ROM_BEGIN(chrEmpty)
#embed "../../chr/tiles/static/ui/empty.chr"
CHARACTER_ROM_END(chrEmpty, chrUnselected);

// menu selection cursor (title screen)
CHARACTER_ROM_BEGIN(chrArrow)
#embed "../../chr/tiles/static/ui/arrow.chr"
CHARACTER_ROM_END(chrArrow, chrEmpty);

// button box border pieces
CHARACTER_ROM_BEGIN(chrButtonBoxUL)
#embed "../../chr/tiles/static/ui/button/box_ul.chr"
CHARACTER_ROM_END(chrButtonBoxUL, chrArrow);

CHARACTER_ROM_BEGIN(chrButtonBoxUR)
#embed "../../chr/tiles/static/ui/button/box_ur.chr"
CHARACTER_ROM_END(chrButtonBoxUR, chrButtonBoxUL);

CHARACTER_ROM_BEGIN(chrButtonBoxBR)
#embed "../../chr/tiles/static/ui/button/box_br.chr"
CHARACTER_ROM_END(chrButtonBoxBR, chrButtonBoxUR);

CHARACTER_ROM_BEGIN(chrButtonBoxBL)
#embed "../../chr/tiles/static/ui/button/box_bl.chr"
CHARACTER_ROM_END(chrButtonBoxBL, chrButtonBoxBR);

CHARACTER_ROM_BEGIN(chrButtonBoxHorizontal)
#embed "../../chr/tiles/static/ui/button/box_horizontal.chr"
CHARACTER_ROM_END(chrButtonBoxHorizontal, chrButtonBoxBL);

CHARACTER_ROM_BEGIN(chrButtonBoxVertical)
#embed "../../chr/tiles/static/ui/button/box_vertical.chr"
CHARACTER_ROM_END(chrButtonBoxVertical, chrButtonBoxHorizontal);

// world dynamic tiles -- FINAL blob. Closing it with _FINAL also emits the
// whole cartridge's CHR ROM image: 0x2000 (8 KB) is the entire CHR ROM here,
// which (being a single page) the PPU maps directly. A banked cartridge would
// pass its full CHR ROM size instead and let the mapper window it in.
CHARACTER_ROM_BEGIN(chrCoin)
#embed "../../chr/tiles/dynamic/coin.chr"
CHARACTER_ROM_END_FINAL(chrCoin, chrButtonBoxVertical, 0x2000);
