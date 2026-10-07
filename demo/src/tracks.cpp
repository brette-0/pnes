#include <platform-nes/audio.hpp>
#include <platform-nes/mappers/mmc3.hpp>
#include "banks.hpp"
#include <intsh>
using namespace br0::intsh;

#ifndef TARGET_NES
TRACKS(
    {.fp ="tracks/pc_audio.wav", .loop_start = 0}
);
#else

#ifdef __cplusplus
extern "C" {
#endif
extern const u8 _music_data_mega_man_2[];
extern const u8 _sounds[];
#ifdef __cplusplus
}
#endif

// Two bytes each -- this project's own choice to keep them beside the rest
// of its writable state in PRG-RAM, not a default audio.hpp picked for us.
CARTMEM TRACKS(_music_data_mega_man_2);
CARTMEM SFX(_sounds);
#endif