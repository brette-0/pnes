#pragma once

#include <intsh>
#include <limits>
#include <type_traits>

using namespace br0::intsh;

// FRAMERATE (CMakeLists.txt: 0 for 60Hz, 1 for 50Hz, set per TARGET_PLATFORM) -- same
// flag header.hpp derives the NES2.0 timing byte from. Kept here too since
// types.hpp may be included ahead of header.hpp.
#ifndef FRAMERATE
#define FRAMERATE 0
#endif

namespace demo {

// Framerated<int_type, number>: a compile-time constant that comes out as-is
// on the 60Hz systems (FRAMERATE == 0). At 50Hz, two conversions are needed
// depending on what the quantity represents:
//  - ScaleUp()   x1.2 (60/50): a per-frame rate (speed, acceleration, gravity)
//    needs to cover more ground each of 50Hz's fewer, slower frames to match
//    the same real-time distance per second.
//  - ScaleDown() x5/6 (50/60): a duration measured in frame-count (a timer)
//    needs fewer frames to span the same real-time span, since each 50Hz
//    frame lasts longer.
//
// `number` is a plain non-type template value (not required to already be
// int_type) -- it only has to weakly convert into int_type; that conversion
// and both scaled results are range-checked at compile time below.
template <typename int_type, auto number>
class Framerated {
    static_assert(std::is_arithmetic_v<int_type>,
        "Framerated: int_type must be an arithmetic type");
    static_assert(std::is_convertible_v<decltype(number), int_type>,
        "Framerated: number must be (weakly) convertible to int_type");

    static constexpr long double raw        = static_cast<long double>(number);
    static constexpr long double scaledUp   = raw * 1.2L;
    static constexpr long double scaledDown = raw * (5.0L / 6.0L);

    static_assert(raw >= static_cast<long double>(std::numeric_limits<int_type>::lowest()) &&
                  raw <= static_cast<long double>(std::numeric_limits<int_type>::max()),
        "Framerated: number does not fit in int_type");
    static_assert(scaledUp >= static_cast<long double>(std::numeric_limits<int_type>::lowest()) &&
                  scaledUp <= static_cast<long double>(std::numeric_limits<int_type>::max()),
        "Framerated: number * 1.2 (50Hz scale-up) overflows/underflows int_type");
    static_assert(scaledDown >= static_cast<long double>(std::numeric_limits<int_type>::lowest()) &&
                  scaledDown <= static_cast<long double>(std::numeric_limits<int_type>::max()),
        "Framerated: number * 5/6 (50Hz scale-down) overflows/underflows int_type");

public:
    int_type val;

    constexpr Framerated() : val(static_cast<int_type>(number)) {}

    constexpr int_type ScaleUp() const {
        if constexpr (FRAMERATE == 0) {
            return val;
        } else {
            return static_cast<int_type>(static_cast<long double>(val) * 1.2L);
        }
    }

    constexpr int_type ScaleDown() const {
        if constexpr (FRAMERATE == 0) {
            return val;
        } else {
            return static_cast<int_type>(static_cast<long double>(val) * (5.0L / 6.0L));
        }
    }
};

}
