/**
 * @file intsh.hpp
 * @brief PS3-only stand-in for the real `intsh` single-header dependency
 *        (FetchContent'd for every other platform bar PS2).
 *
 * Exactly the same problem, and the same fix, as src/ps2/intsh_shim/intsh.hpp
 * -- read that file for the full reasoning. The short version: every shared
 * engine header unconditionally does `#include <intsh>; using namespace
 * br0::intsh;`, and PSL1GHT's ppu-types.h already typedefs u8/u16/u32/u64 and
 * s8/s16/s32/s64 as ordinary global typedefs (verified -- every rsx/libaudio/
 * libio signature is declared in those exact names, and ppu-types.h's own body
 * references them unqualified internally). Two unqualified spellings of the
 * same name are ambiguous to C++ even when they denote the identical type, and
 * the ambiguity would break PSL1GHT's own headers, not just this project's.
 *
 * So this shim provides ONLY what ppu-types.h lacks: intsh's signed i8/i16/
 * i32/i64 (PSL1GHT spells its signed names s8/s16/s32/s64 instead; ::DeltaScroll
 * in video.hpp / src/emu/ppu.cpp is the one place the engine writes a real
 * type as `i8`). u8..u64/s8..s64 and the fast/least variants are deliberately
 * left out.
 *
 * Note PSL1GHT also typedefs f32/f64 and the volatile v* family, which intsh
 * does not declare at all -- so those need nothing here either way.
 *
 * Wired in by exposing this directory as the br0::intsh INTERFACE target in
 * the ps3 branch of CMakeLists.txt, so `#include <intsh>` resolves here. There
 * is nothing to order it against: FetchContent_MakeAvailable is called for
 * `tuple` only, so the real intsh is never fetched for TARGET_PLATFORM=ps3.
 */
#pragma once

namespace br0::intsh {

using i8  = signed char;
using i16 = short;
using i32 = int;
using i64 = long long;

}
