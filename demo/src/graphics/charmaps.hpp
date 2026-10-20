#pragma once

#include <pnes/technology.hpp>
#include "../../chr/chr.h"
// Default character map: source token -> CHR tile byte.
//
// CHARMAP expands to a constexpr function `charmap_generic(char)`. Any TU that
// maps a string through `generic` (via STRING) needs this function in scope, so
// it includes this header (strings.hpp pulls it in for that reason). Being
// constexpr (hence inline), the charmap can be included by any number of TUs
// with no ODR/LTO trouble.
CHARMAP(generic,                            // default char map
    if (_c == ' ') return (u8)(blob::ui::hud_whitespace);      // dedicated blank tile
    CM(0, blob::ui::font + 0x00)
    CM(1, blob::ui::font + 0x01)
    CM(2, blob::ui::font + 0x02)
    CM(3, blob::ui::font + 0x03)
    CM(4, blob::ui::font + 0x04)
    CM(5, blob::ui::font + 0x05)
    CM(6, blob::ui::font + 0x06)
    CM(7, blob::ui::font + 0x07)
    CM(8, blob::ui::font + 0x08)
    CM(9, blob::ui::font + 0x09)
    CM(A, blob::ui::font + 0x0a)
    CM(B, blob::ui::font + 0x0b)
    CM(C, blob::ui::font + 0x0c)
    CM(D, blob::ui::font + 0x0d)
    CM(E, blob::ui::font + 0x0e)
    CM(F, blob::ui::font + 0x0f)
    CM(G, blob::ui::font + 0x10)
    CM(H, blob::ui::font + 0x11)
    CM(I, blob::ui::font + 0x12)
    CM(J, blob::ui::font + 0x13)
    CM(K, blob::ui::font + 0x14)
    CM(L, blob::ui::font + 0x15)
    CM(M, blob::ui::font + 0x16)
    CM(N, blob::ui::font + 0x17)
    CM(O, blob::ui::font + 0x18)
    CM(P, blob::ui::font + 0x19)
    CM(Q, blob::ui::font + 0x1a)
    CM(R, blob::ui::font + 0x1b)
    CM(S, blob::ui::font + 0x1c)
    CM(T, blob::ui::font + 0x1d)
    CM(U, blob::ui::font + 0x1e)
    CM(V, blob::ui::font + 0x1f)
    CM(W, blob::ui::font + 0x20)
    CM(X, blob::ui::font + 0x21)
    CM(Y, blob::ui::font + 0x22)
    CM(Z, blob::ui::font + 0x23)
);
