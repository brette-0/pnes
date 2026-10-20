#pragma once
#include <pnes/video.hpp>
#include "../banks.hpp"   // LEVEL_GRAPHICS

LEVEL_GRAPHICS extern const oam::sprite_t msMary[0x02];
LEVEL_GRAPHICS extern const oam::sprite_t msMary2[0x02];  // player 2: same tiles, horizontally flipped

// Hardware-sprite count of the player metasprite (two hardware 8x16 columns),
// derived from msMary's array extent so the OAM populate counts can never
// drift from the metasprite's actual size. A compile-time constant: folds to
// a literal with zero runtime cost.
inline constexpr u8 kMarySprites = static_cast<u8>(sizeof(msMary) / sizeof(msMary[0]));