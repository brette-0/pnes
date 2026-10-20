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

// PRG-ROM size (262144) MUST match demo/link.ld's own __prg_rom_size (256, in
// KiB) -- that script now accepts anything up to MMC3's 512 KiB maximum, and
// nothing can check the two against each other automatically: the NES2.0
// header is built here, at compile time, where linker symbols don't exist
// yet. Change one and change the other, or the ROM ships a header that lies
// about its own size. CHR-ROM (16384) has the same relationship with
// __chr_rom_size, and is in turn fixed by the [manifest] size in
// demo/chr/config.toml -- that is what blobman packs into demo/chr/chr.bin,
// which demo/src/graphics/chr_rom.cpp embeds as the whole CHR image.
HEADER(
    Mirroring::Horizontal, false,
    262144, 16384,
    8192, 0,
    0, 0,
    REGION ? Timing::PAL : Timing::NTSC
);