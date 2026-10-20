/**
 * @file audio.cpp
 * @brief Audio backend stub for the Game Boy Advance.
 *
 * The shared engine expects every backend to define the audio entry points
 * (see audio.hpp) plus the ::pcm_buffer / ::pcm_buffer_size globals the
 * non-NES build references. The GBA has only 256 KB of EWRAM (+ 32 KB IWRAM,
 * + 96 KB VRAM), so -- like the DS and the GameCube/Wii backends, and even more
 * so here -- there is no room to bake the demo's PCM tracks into the ROM; per
 * the project's handheld plan the user supplies a synthesiser instead, and the
 * engine ships embedding only the tiny CHR art. These entry points are therefore
 * inert so the library links and the demo runs silent. The signatures match the
 * SDL backend exactly, so swapping in a real GBA mixer later (the legacy PSG
 * channels map naturally onto the NES APU) touches only this file.
 */
#include <pnes/audio.hpp>

/** @brief Shared mixing buffer (unused until a real GBA mixer lands). */
float *audio::pcm_buffer = nullptr;
/** @brief Size of ::pcm_buffer, in samples. */
u32 audio::pcm_buffer_size = 0;

void audio::Init(u8) {}
void audio::Update() {}

void audio::TrackPlay(u8 /*index*/) {}
void audio::TrackPause(u8 /*pause*/) {}
void audio::TrackStop() {}

void audio::SfxPlay(u8 /*index*/, u8 /*channel*/) {}
void audio::SfxSamplePlay(u8 /*index*/) {}
