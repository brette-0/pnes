#pragma once

#include <platform-nes/technology.hpp>
#include "../graphics/graphics.hpp"
// Default character map: source token -> CHR tile byte.
//
// CHARMAP expands to a constexpr function `charmap_generic(char)`. Any TU that
// maps a string through `generic` (via STRING) needs this function in scope, so
// it includes this header (strings.hpp pulls it in for that reason). Being
// constexpr (hence inline), the charmap can be included by any number of TUs
// with no ODR/LTO trouble.
CHARMAP(generic,                            // default char map
    if (_c == ' ') return (u8)(chrHUDWhitespace_tile);      // dedicated blank tile
    CM(0, chrFont_tile + 0x00)
    CM(1, chrFont_tile + 0x01)
    CM(2, chrFont_tile + 0x02)
    CM(3, chrFont_tile + 0x03)
    CM(4, chrFont_tile + 0x04)
    CM(5, chrFont_tile + 0x05)
    CM(6, chrFont_tile + 0x06)
    CM(7, chrFont_tile + 0x07)
    CM(8, chrFont_tile + 0x08)
    CM(9, chrFont_tile + 0x09)
    CM(A, chrFont_tile + 0x0a)
    CM(B, chrFont_tile + 0x0b)
    CM(C, chrFont_tile + 0x0c)
    CM(D, chrFont_tile + 0x0d)
    CM(E, chrFont_tile + 0x0e)
    CM(F, chrFont_tile + 0x0f)
    CM(G, chrFont_tile + 0x10)
    CM(H, chrFont_tile + 0x11)
    CM(I, chrFont_tile + 0x12)
    CM(J, chrFont_tile + 0x13)
    CM(K, chrFont_tile + 0x14)
    CM(L, chrFont_tile + 0x15)
    CM(M, chrFont_tile + 0x16)
    CM(N, chrFont_tile + 0x17)
    CM(O, chrFont_tile + 0x18)
    CM(P, chrFont_tile + 0x19)
    CM(Q, chrFont_tile + 0x1a)
    CM(R, chrFont_tile + 0x1b)
    CM(S, chrFont_tile + 0x1c)
    CM(T, chrFont_tile + 0x1d)
    CM(U, chrFont_tile + 0x1e)
    CM(V, chrFont_tile + 0x1f)
    CM(W, chrFont_tile + 0x20)
    CM(X, chrFont_tile + 0x21)
    CM(Y, chrFont_tile + 0x22)
    CM(Z, chrFont_tile + 0x23)
);
