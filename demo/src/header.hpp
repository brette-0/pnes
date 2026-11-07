#pragma once
#include <pnes/headers/nes2.hpp>

// REGION (CMakeLists.txt: 0 NTSC, 1 PAL, set per TARGET_PLATFORM) drives the
// NES2.0 timing byte the same way famistudio_config.s derives its NTSC/PAL
// support flags from it -- one source of truth for which region a given
// TARGET_PLATFORM build targets, not a separate literal to remember to flip
// alongside it.
#ifndef REGION
#define REGION 0
#endif


HEADER(
    Mirroring::Horizontal, false,
    262144, 16384,
    0, 8192,
    0, 0,
    REGION ? Timing::PAL : Timing::NTSC
);