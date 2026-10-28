#pragma once
#include <consh>

#include "pnes/video.hpp"

using namespace br0::consh;

namespace debug {
    auto constexpr SetColour(const u8 colour) -> void {
#ifdef TARGET_NES
        ppu::SetColorPriority(colour);
#endif
    }
}