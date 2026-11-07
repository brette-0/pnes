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

/**
 * @brief One entry in a generated Canvas's component table: where a component
 *        is, and the single function that hands it a frame's input.
 *
 * A Canvas (see uitk's genCanvasDecl) has no geometry or appearance of its own
 * -- it is a focus ring over the interactable nodes nested inside it. To move
 * that focus it needs both halves of each component by *index*, which is only
 * known at runtime, so this is the indexable view over nodes that are
 * otherwise reachable by name alone.
 *
 * Both members are pointers into the component's own storage rather than
 * copies of it, which is what keeps the generated table `constexpr` on every
 * target. A fixed-viewport node's `rect` is itself `constexpr`, but a
 * runtime-viewport one's is mutable storage its `Make()` fills, so a table of
 * Rect *values* could not be built ahead of time there -- while the ADDRESS of
 * that storage is a compile-time constant everywhere. It also means an entry
 * can never disagree with the node it describes: there is only ever one rect.
 *
 * `rect` is `const volatile` -- which is what `atomic Rect` is on every C++
 * build (technology.hpp) -- so one pointer type covers both: a `constexpr
 * Rect`'s address converts by merely adding the qualifiers. Reading one copies
 * it out through ::Rect's volatile copy constructor, the same way a generated
 * `Make()` already copies a member's rect into a SingleChoice's option table.
 *
 * `func` is the component's `Pass(u8)`, and in generated code it is never
 * anything else: a Canvas only ever hands a component input. Note that this is
 * the one place a generated `Pass` is called indirectly -- taking its address
 * forces an out-of-line copy of a function uitk marks always-inline, so the
 * component bodies a Canvas reaches cost a call where a direct one would not.
 * That is the price of indexing by a runtime value, and it is paid once per
 * press.
 */
struct InvokeableComponant {
    const volatile Rect* rect;  ///< The component's own `rect` storage.
    void (*func)(u8);           ///< The component's own `Pass`.
};