#include "palette.hpp"

#include <algorithm>

namespace pal {

std::uint32_t rgbOf(const std::uint8_t index) {
    return kNesPalette[std::min<std::size_t>(index, kNesColours - 1)];
}

std::array<std::uint32_t, kColoursPerPalette> rgbOf(const Palette& palette) {
    return {rgbOf(kTransparencyColour),
            rgbOf(palette.colours[0]),
            rgbOf(palette.colours[1]),
            rgbOf(palette.colours[2])};
}

std::array<std::uint32_t, kColoursPerPalette> placeholderRgb() {
    return {0x000000u, 0xFF0000u, 0x00FF00u, 0x0000FFu};
}

} // namespace pal
