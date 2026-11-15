#include <pnes/audio.hpp>
#include <pnes/mappers/mmc3.hpp>
#include "banks.hpp"
#include <intsh>
using namespace br0::intsh;

#ifndef TARGET_NES
const audio::music_t tracks[] = {
    {.fp ="tracks/pc_audio.wav", .loop_start = 0}
};
const u8 nTracks = sizeof(tracks) / sizeof(audio::music_t);
#else

#ifdef __cplusplus
extern "C" {
#endif
extern const u8 _music_data_mega_man_2[];
#ifdef __cplusplus
}
#endif

// Two bytes -- this project's own choice to keep them beside the rest of its
// writable state in PRG-RAM, not a default audio.hpp picked for us. Constant
// initialiser, never assigned to: audio::Init passes the value to the engine
// as an inline-asm immediate, so it has to stay a link-time constant the
// optimiser can fold (see audio.hpp's ::tracks).
//
// There is no matching line for the effects. FamiStudio hardcodes that
// symbol, so src/nes/audio/famistudio.cpp names `_sounds` itself -- having
// the SFX export in the ROM is the whole of what this project has to do.
CARTMEM const u8* const tracks = _music_data_mega_man_2;
#endif