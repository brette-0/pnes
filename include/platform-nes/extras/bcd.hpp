#pragma once

#include <platform-nes/extras/math.hpp>
#include <intsh>
#include <type_traits>
#include <limits>

#include "platform-nes/technology.hpp"
#include "platform-nes/video.hpp"
using namespace br0::intsh;

namespace bcd {
    template <typename T, typename CharMap>
    static void DrawUnsigned(const u16 addr, T num, const u8* numberMap) {

    }


    template <typename T, typename CharMap>
    void Draw(u16 addr, T num, const u8* numberMap) {
        if constexpr (std::is_signed_v<T>) {
            ppu::WriteSingleToNameTable(addr, CharMap('-'));
            DrawUnsigned(addr + 1, abs(num), numberMap);
        }
        else DrawUnsigned(addr, num, numberMap);
    }
}