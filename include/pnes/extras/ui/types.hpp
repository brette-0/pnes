#pragma once
#include "pnes/types.hpp"

/**
 * @brief A tile-space anchor and footprint as one value: where a UI element
 *        sits, and how much of the grid it covers.
 *
 * This is the shape uitk emits for every placeable node's geometry (see the
 * generated `<node>::rect`). Position is in tiles from the world origin, not
 * pixels and not a PPU address -- a generated node carries that separately as
 * `<node>::pos`, already resolved.
 *
 * Kept a plain two-member value rather than split back into a pos and a size:
 * a UI element's anchor and its extent are only ever meaningful together --
 * drawing a box, hit-testing a cursor, or placing a selection indicator all
 * need both -- and keeping them in one object is what lets a whole element's
 * geometry be copied out of `atomic` storage in a single statement.
 *
 * The constructors below exist for that last point. `atomic` is `volatile` on
 * every C++ build (technology.hpp), and a volatile object cannot use an
 * implicit copy constructor or assignment operator -- both take a plain
 * `const Rect&` and so are not viable on one. A target whose viewport is only
 * known at runtime stores its rects as `atomic Rect`, so without these a
 * generated `Make()` could not write one and a caller could not read one back.
 * Same reasoning, and same remedy, as ::ppu::CartesianToAddress's own
 * `const volatile vec2<u16>&` overload (video.hpp).
 */
struct Rect {
    vec2<u16> pos;
    vec2<u8>  size;

    Rect() = default;
    constexpr Rect(const vec2<u16>& pos, const vec2<u8>& size) noexcept : pos(pos), size(size) {}
    constexpr Rect(const Rect&) = default;
    constexpr Rect& operator=(const Rect&) = default;

    /// Copy out of `atomic Rect` storage -- member-wise, since the implicit
    /// copy constructor is not viable on a volatile source.
    Rect(const volatile Rect& other) noexcept
        : pos{other.pos.x, other.pos.y}, size{other.size.x, other.size.y} {}

    /// Store into `atomic Rect` storage. Returns nothing rather than a
    /// reference: handing back a `volatile Rect&` would invite a chained
    /// assignment that reads the value straight back out of volatile memory.
    void operator=(const Rect& other) volatile noexcept {
        pos.x  = other.pos.x;
        pos.y  = other.pos.y;
        size.x = other.size.x;
        size.y = other.size.y;
    }

    constexpr bool Contains(const vec2<u16> &target) const noexcept {
        if (target.x < pos.x) return false;
        if (target.y < pos.y) return false;
        if (target.x >= pos.x + size.x) return false;
        if (target.y >= pos.y + size.y) return false;
        return true;
    }
};
