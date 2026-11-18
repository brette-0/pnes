#pragma once
#include <pnes/headers/nes2.hpp>

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
    // Timing::MULTI (NES2.0 byte 12 = 2), not NTSC or PAL, and no longer tied
    // to any build flag: this ROM genuinely runs on both. src/nes/video.cpp measures
    // the console's frame length at boot into video::is50Hz, demo::Framerated
    // selects its 60Hz or 50Hz value off that, both FamiStudio note tables and
    // both SFX tables are linked in (demo/famistudio_config.s), and the engine
    // picks by the same measured value. Declaring one region would be a claim
    // the image no longer makes -- and declaring the WRONG one is what an
    // auto-region emulator or flashcart would then act on.
    Timing::MULTI
);