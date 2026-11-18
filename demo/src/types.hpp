#pragma once

#include <intsh>
#include <limits>
#include <type_traits>

// For video::Is50Hz(), which is what selects between the two values Framerated
// below bakes into the build. See that class's own comment.
#include <pnes/video.hpp>

using namespace br0::intsh;

namespace demo {

// Framerated<int_type, number>: a tuning constant that has one value at 60Hz
// and another at 50Hz, with the choice made AT RUNTIME from the refresh the
// console actually came up at (video::Is50Hz(), pnes/video.hpp) rather than
// a build-time flag.
//
// WHY RUNTIME: the build does not know the answer. PAL60/EURGB60 is the case
// that settles it -- a European GameCube set to 60Hz hands src/ogc/video.cpp's
// irq::init() a 60Hz mode, so a PAL BUILD TICKS AT 60Hz and anything scaled
// for 50Hz at compile time runs ~20% fast. A PAL NES running an NTSC image, or
// an emulator defaulting to the wrong region, is the same mistake mirrored.
//
// NO RUNTIME ARITHMETIC. Both answers are computed by the compiler and stored
// in the build, and what happens at runtime is a select -- an indexed load, not
// a multiply. Nothing here puts long-double maths (or any multiply) in reach of
// a 6502; the cost is one extra int_type in ROM per constant, plus the index.
//
// The two conversions, which differ by what the quantity MEANS:
//  - ScaleUp()   x1.2 (60/50): a per-frame rate (speed, acceleration, gravity)
//    must cover more ground in each of 50Hz's fewer, slower frames to travel
//    the same real distance per second.
//  - ScaleDown() x5/6 (50/60): a duration measured in frames (a timer) needs
//    FEWER frames to span the same real time, since each frame lasts longer.
//
// `number` is a plain non-type template value (not required to already be
// int_type) -- it only has to weakly convert into int_type. That conversion and
// both scaled results are range-checked below, and those checks matter more
// than they used to: both results are now live in the same binary, so a build
// can no longer be "the one where the overflowing value never appears".
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

    // Indexed by video::Is50Hz(): [0] is the 60Hz value, [1] the 50Hz one.
    // Both entries are compile-time constants; only the subscript is runtime.
    static constexpr int_type up[2] = {
        static_cast<int_type>(raw), static_cast<int_type>(scaledUp)
    };
    static constexpr int_type down[2] = {
        static_cast<int_type>(raw), static_cast<int_type>(scaledDown)
    };

public:
    /// @brief This constant's value for a per-frame RATE at the detected refresh.
    static int_type ScaleUp()   { return up[::video::Is50Hz()]; }
    /// @brief This constant's value for a frame-count DURATION at the detected refresh.
    static int_type ScaleDown() { return down[::video::Is50Hz()]; }
};

}
