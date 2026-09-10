#include "colours.hpp"
#include "../banks.hpp"   // LEVEL_GRAPHICS

LEVEL_GRAPHICS const u8 BGColours[16] = {
    0x20, 0x10, 0x00, 0x0f,
    0x20, 0x17, 0x27, 0x37,
    0x20, 0x06, 0x0a, 0x0b,
    0x20, 0x07, 0x0c, 0x0e,
};

TITLE const u8 titleScreenColours[3] = {
    0x0f, 0x20, 0x10
};

// Same colours as titleScreenColours, duplicated into LEVEL_GRAPHICS: the
// level's HUD (palette 3) needs them too, but title.cpp's copy lives in the
// TITLE bank, which isn't mapped during level mode.
LEVEL_GRAPHICS const u8 hudColours[3] = {
    0x0f, 0x20, 0x10
};

LEVEL_GRAPHICS const u8 maryColors[3] = {0x07, 0x26, 0x04};
