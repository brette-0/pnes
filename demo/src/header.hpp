#pragma once
#include <pnes/headers/nes2.hpp>

// FRAMERATE (CMakeLists.txt: 0 for 60Hz, 1 for 50Hz, set per TARGET_PLATFORM) drives the
// NES2.0 timing byte the same way famistudio_config.s derives its NTSC/PAL
// support flags from it -- one source of truth for which region a given
// TARGET_PLATFORM build targets, not a separate literal to remember to flip
// alongside it.
#ifndef FRAMERATE
#define FRAMERATE 0
#endif


// The $6000-$7FFF window is declared as PRG-NVRAM, not volatile PRG-RAM, and
// the battery bit is set for it -- the latter from the SAVE_FILE define rather
// than from an argument here (nes2.hpp's HEADER() comment). MMC3 makes this
// all-or-nothing: the board carries ONE 8 KiB chip at that window, so it is
// either battery-backed in its entirety or not at all, and there is no
// volatile sub-range to declare alongside it. demo/link.ld's own
// __program_ram_size/__program_nvram_size pair carries the same split into
// the layout, and HEADER() static_asserts that SAVE_FILE and the
// prg_nvram_bytes below cannot drift apart.
HEADER(
    Mirroring::Horizontal,
    262144, 16384,
    0, 8192,
    0, 0,
    FRAMERATE ? Timing::PAL : Timing::NTSC
);