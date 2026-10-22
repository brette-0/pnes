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

    // Finds the end of buff itself by scanning for `terminator`, so there is no
    // length to pass: a charmap-encoded string carries its terminator (see
    // Charmap::encode), and both bytes here are tiles, not ASCII. The scan
    // stops at 255 bytes, the most the old explicit length could express
    // anyway, so an unterminated buffer truncates instead of running away.
    NI textBuffer* Make(const u8* buff, vec2<u8> box, u8 splitter, u8 terminator);

    // Charmap overload: takes the charmap itself and lifts the two bytes above
    // out of it, so a call site names neither. @p Map is a template parameter,
    // not an argument, for two reasons -- a charmap is only ever
    // constant-evaluated, so nothing of it reaches the ROM, and its
    // operator()'s fall-through to the deliberately undefined
    // nes_str::unmapped must never become a real call. Both bytes are bound to
    // constexpr locals for that second reason: an unoptimised build is allowed
    // not to fold a call in a plain argument position.
    template <auto& Map, char Splitter = ' '>
    AI inline textBuffer* Make(const u8* buff, const vec2<u8> box) {
        constexpr u8 splitter   = Map(Splitter);
        constexpr u8 terminator = Map('\0');
        return Make(buff, box, splitter, terminator);
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
