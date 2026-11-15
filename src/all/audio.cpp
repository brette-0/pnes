#include <pnes/audio.hpp>
#include <intsh>
using namespace br0::intsh;

/*
 * The registered SFX table, owned here rather than in any one backend.
 *
 * ONE DEFINITION FOR EVERY NON-NES TARGET. Every src/all source is compiled
 * into each platform's library (see CMakeLists.txt), so the eight backends that
 * read ::sfxs/::nSfx get them from here instead of each carrying its own copy
 * -- which is what they used to do, as a weak zero-entry array per file, to
 * keep the link working for applications that never invoked the old ::SFX
 * macro. Registration removes the need: nothing is weak, nothing is defined
 * per backend, and an application with no effects just never calls ::SetSfx.
 *
 * NOTHING HERE IS NES. The NES backend names FamiStudio's effect bank itself
 * (src/nes/audio/famistudio.cpp), because that symbol is fixed by the
 * exporter; there is no table and no registration. This file still compiles
 * on the NES, to nothing, because the glob that builds the library picks up
 * the src/all sources unconditionally.
 */
#ifndef TARGET_NES

const audio::sfx_t* sfxs = nullptr;
u8 nSfx = 0;

// Stored, NOT copied: the caller keeps ownership, which is why audio.hpp's
// contract says the table has to outlive every audio call. A backend that
// decodes up front does it in audio::Init, so this has to have run first --
// also audio.hpp's contract, and not something this function can check.
void audio::SetSfx(const sfx_t* const table, const u8 count) {
    sfxs = table;
    nSfx = table ? count : 0;
}

#endif
