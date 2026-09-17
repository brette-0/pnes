/**
 * @file types.hpp
 * @brief Shared scalar and struct typedefs.
 *
 * Included first by ::platform-nes.h so later subsystem headers can rely
 * on a single point for common typedefs as the library grows.
 */
#pragma once

#include <intsh>
using namespace br0::intsh;

template <typename T>
struct buffer {
    T addr;
    u8 size;
};

template <typename t>
concept addable = requires (t a, t b) {
    a + b;
    a - b;
};

template <addable t>
struct vec2 {
    t x;
    t y;

    constexpr vec2 operator+(const vec2& rhs) const {
        return { static_cast<t>(x + rhs.x), static_cast<t>(y + rhs.y) };
    }

    constexpr vec2 operator-(const vec2& rhs) const {
        return { static_cast<t>(x - rhs.x), static_cast<t>(y - rhs.y) };
    }

    constexpr vec2& operator+=(const vec2& rhs) {
        x += rhs.x;
        y += rhs.y;
        return *this;
    }
    constexpr vec2& operator-=(const vec2& rhs) {
        x -= rhs.x;
        y -= rhs.y;
        return *this;
    }

    // Volatile-qualified overloads so `atomic vec2<t>` (volatile on NES, see
    // the `atomic` macro) can still be assigned/mutated in place -- the
    // implicit copy assignment operator the compiler would otherwise
    // generate is never volatile-qualified, so without these an `atomic
    // vec2<...>` object has no usable operator= at all.
    constexpr volatile vec2& operator=(const vec2& rhs) volatile {
        x = rhs.x;
        y = rhs.y;
        return *this;
    }
    constexpr volatile vec2& operator+=(const vec2& rhs) volatile {
        x += rhs.x;
        y += rhs.y;
        return *this;
    }
    constexpr volatile vec2& operator-=(const vec2& rhs) volatile {
        x -= rhs.x;
        y -= rhs.y;
        return *this;
    }

    // element-wise conversion to a vec2 of another (convertible) element type
    template <addable u>
    constexpr operator vec2<u>() const {
        return { static_cast<u>(x), static_cast<u>(y) };
    }
};