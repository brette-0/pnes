/**
 * @file intsh.hpp
 * @brief PS2-only stand-in for the real `intsh` single-header dependency
 *        (FetchContent'd for every other platform).
 *
 * Every shared engine header (video.hpp, input.hpp, audio.hpp, technology.hpp,
 * types.hpp, ...) unconditionally does `#include <intsh>; using namespace
 * br0::intsh;`, unchanged on every platform including this one -- this file
 * is what makes that safe for PS2, not any `#ifdef` in those headers.
 *
 * ps2sdk's own tamtypes.h already typedefs u8/u16/u32/u64/s8/s16/s32/s64 as
 * ordinary global typedefs (every gsKit/libpad/audsrv signature is declared
 * in those exact names), and src/ps2/internal.hpp includes it before any
 * engine header. If the real intsh.hpp also aliased those same names via a
 * `using namespace` directive, both would be visible unqualified at once --
 * C++ treats that as ambiguous even though they denote the identical
 * underlying type, and not just at the point of use: tamtypes.h's OWN body
 * references e.g. `u32` unqualified internally, so the ambiguity breaks
 * ps2sdk's headers too, not just this project's.
 *
 * So this shim provides ONLY the names tamtypes.h does NOT already have --
 * intsh's signed i8/i16/i32/i64 (tamtypes.h's own signed names are spelled
 * s8/s16/s32/s64 instead; ::DeltaScroll in video.hpp / src/emu/ppu.cpp is the
 * one place the engine actually spells a real type with `i8`) -- and
 * deliberately leaves out u8/u16/u32/u64/s8/s16/s32/s64 and the fast/least
 * variants (if8.../uf8.../il8.../ul8...), none of which the engine uses
 * beyond what tamtypes.h already covers.
 *
 * Wired in by putting this directory on the include path AHEAD of the
 * fetched intsh's (target_include_directories(... BEFORE ...) in the ps2
 * branch of CMakeLists.txt) so `#include <intsh>` resolves here instead --
 * no FetchContent of the real dependency happens for TARGET_PLATFORM=ps2 at
 * all.
 */
#pragma once

namespace br0::intsh {

using i8  = signed char;
using i16 = short;
using i32 = int;
using i64 = long long;

}
