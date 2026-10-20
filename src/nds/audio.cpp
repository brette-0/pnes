/**
 * @file audio.cpp
 * @brief Audio backend stub for Nintendo DS + DSi.
 *
 * The shared engine expects every backend to define the audio entry points
 * (see audio.hpp) plus the ::pcm_buffer / ::pcm_buffer_size globals the
 * non-NES build references. The DS has only 4 MB of main RAM, so -- unlike the
 * desktop/Switch builds -- there is no room to bake the demo's PCM tracks into
 * the ROM; per the project's DS plan the user supplies a synthesiser instead,
 * and the engine ships embedding only the tiny CHR art. These entry points are
 * therefore inert (the same approach as the GameCube/Wii backends) so the
 * library links and the demo runs silent. The signatures match the SDL backend
 * exactly, so swapping in a real libnds/maxmod mixer later touches only this
 * file.
 *
 * The DSi build (TARGET_DSI) has 16 MB and a faster clock and could later embed
 * crunched audio here; for now it shares this DS stub so both targets link and
 * behave identically.
 */
#include <pnes/audio.hpp>

/** @brief Shared mixing buffer (unused until a real DS mixer lands). */
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
