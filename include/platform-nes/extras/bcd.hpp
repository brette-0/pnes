#pragma once

#include <platform-nes/extras/math.hpp>
#include <intsh>
#include <type_traits>
#include <consh>

#include "platform-nes/video.hpp"
using namespace br0::intsh;
using namespace br0::consh;

template <nx T>
class bcd {
public:
    bcd() {
        if constexpr (std::is_signed_v<T>) {
            buff = new u8[(sizeof(T) << 3) + 1];
            return;
        }

        buff = new u8[sizeof(T) << 3];
    }

    template <typename CharMap>
    MINSIZE constexpr void Make(T num) {
        if constexpr (std::is_signed_v<T>) {
            if (num < 0) {
                buff[0] = CharMap('-');
                num = abs(num);
            }
        }

        std::make_unsigned_t<T> threshold = 1;
        u8 nextChar;
        for (; threshold < num; threshold *= 10) { }
        while (num) {
            u8 index;
            for (index = num < 0, nextChar = CharMap('0'); threshold <= num; ++nextChar) { }
            buff[index] = nextChar;
            if (threshold == 1) break;
            threshold /= 10;
        }
    }

    MINSIZE constexpr void Draw(const u16 addr) const {
        ppu::WriteFromBufferToNameTable(addr, buff, (sizeof(T) << 3) + std::is_signed_v<T>, 0);
    }

    ~bcd() {
        delete[] buff;
    }

private:
    u8* buff;
};