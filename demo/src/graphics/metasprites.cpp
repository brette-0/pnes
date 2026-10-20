#include <pnes/video.hpp>

#include "metasprites.hpp"
#include "../../chr/chr.h"

// Hardware 8x16 sprites: each OAM entry is one 8-wide, 16-tall column. Tile
// bit 0 selects the pattern-table half (top = tile & 0xFE, bottom =
// (tile & 0xFE) + 1, both fetched from that same half) -- so standing.chr is
// laid out [topleft, bottomleft, topright, bottomright] to match, and every
// tile id here is forced odd (|1) to select $1000, where the MMC3 CHR banks
// actually put this sprite data (see the chr2Control-chr5Control block in
// title.cpp/level.cpp, which banks normalSprites in there).
// player_standing lands on an even tile in the packed blob (see the entry
// order in demo/chr/config.toml) so `base & 0xFE == base`, keeping the |1
// from shifting the pair's top tile.
#define MS_SPLIT(base) { .tile = static_cast<oam::oam_t>((base) | 1) }, \
                       { .tile = static_cast<oam::oam_t>(((base) + 2) | 1) }

LEVEL_GRAPHICS const oam::sprite_t msMary[0x02] = {
    MS_SPLIT(blob::normalSprites::player_standing),  // small mary normal
};

LEVEL_GRAPHICS const oam::sprite_t msMary2[0x02] = {
    MS_SPLIT(blob::normalSprites::player_standing),  // small mary normal, unflipped
};
