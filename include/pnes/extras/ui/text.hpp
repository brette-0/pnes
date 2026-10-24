#pragma once

#include <intsh>

#include <pnes/technology.hpp>
#include <pnes/types.hpp>
#include <pnes/video.hpp>

using namespace br0::intsh;

// move charmap code here

// text boxes

namespace ui::text {
    enum Alignment : u8 {
        Left,
        Centre,
        Right
    };

    // One word-wrapped row: addr points into the source buffer at the
    // row's first byte, size is the row's length in bytes, splitter
    // excluded.
    struct textBuffer {
        u8* addr;
        u8 size;
    };

    // The word-wrapping worker. Not the API: both bytes it takes are tiles,
    // not characters, which only a charmap can produce -- call ::Make below,
    // which is where they come from. Out of line (and so not a template)
    // because this is the one part of the module with a body worth banking,
    // see UI_BANK in text.cpp.
    namespace detail {
        NI textBuffer* Make(const u8* buff, vec2<u8> box, u8 splitter, u8 terminator);
    }

    // Identity charmap, for text that is never translated: every ASCII
    // character maps to its own byte, so `Make<ascii>` wraps a plain C string
    // -- splitter 0x20, terminator 0x00 -- exactly as the pre-charmap
    // primitive did. Characters past 0x7f are deliberately unmapped: a `char`
    // literal cannot name them portably, and a buffer holding them is not
    // ASCII and wants a charmap of its own.
    inline constexpr auto ascii = ::tech::nes_str::charmap({ { '\0', '\x7f', 0 } });

    // Finds the end of buff itself by scanning for the charmap's terminator
    // tile, so there is no length to pass: a charmap-encoded string carries
    // its terminator (see Charmap::encode). The scan stops at 255 bytes, the
    // most the old explicit length could express anyway, so an unterminated
    // buffer truncates instead of running away.
    //
    // Both the terminator and the splitter -- the character rows are broken on,
    // the space by default -- are lifted out of the charmap, so a call site
    // names neither tile. @p Map is a template parameter, not an argument, for
    // two reasons: a charmap is only ever constant-evaluated, so nothing of it
    // reaches the ROM, and its operator()'s fall-through to the deliberately
    // undefined nes_str::unmapped must never become a real call. @p Splitter
    // travels as a `char` NTTP for that same second reason, and both bytes are
    // bound to constexpr locals for it too: an unoptimised build is allowed not
    // to fold a call in a plain argument position.
    template <auto& Map, char Splitter = ' '>
    AI inline textBuffer* Make(const u8* buff, const vec2<u8> box) {
        constexpr u8 splitter   = Map(Splitter);
        constexpr u8 terminator = Map('\0');
        return detail::Make(buff, box, splitter, terminator);
    }

    AI void Draw(const textBuffer* chunks, vec2<u16> pos, vec2<u8> box, Alignment align);

    NI void Draw(const textBuffer* rows, u16 address, vec2<u8> box, Alignment align);

    NI void Clear(
        const textBuffer* rows, u16 address, vec2<u8> box, Alignment align, u8 clear
    );

    AI inline void Draw(const textBuffer* chunks, const vec2<u16> pos, const vec2<u8> box, const Alignment align) {
        Draw(chunks, ppu::CartesianToAddress(pos), box, align);
    }
}
