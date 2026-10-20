#include <pnes/audio.hpp>
#include <pnes/technology.hpp>   // CREATE_SEGMENT_KEYWORD

/**
 * @brief Placement keyword for this module's own code -- the section is the
 *        CONSUMING PROJECT'S choice, supplied as PNES_AUDIO_SECTION.
 *
 * THE WHOLE POINT: the audio backend must land in the SAME PRG-ROM bank as
 * the engine it drives. A project already controls where the engine goes
 * (its FamiStudio config's FAMISTUDIO_CA65_CODE_SEGMENT, and the ca65 wrappers
 * around any exported data); this is the matching knob for this module, so the
 * two can be put side by side.
 *
 * Once they are neighbours, THIS MODULE CONTAINS NO BANK-SWITCHING AT ALL.
 * A caller long-calls audio::Update(); inside it, famistudio_update() is an
 * ordinary call, because the engine is already mapped -- the caller's own
 * switch brought both in together. That is why nothing here includes a
 * mapper header, names a bank, or knows which mapper the board even has.
 *
 * DELIBERATELY NOT NAMED AFTER FAMISTUDIO. A replacement engine written in
 * C, C++, Rust or hand-written assembly binds to the same arrangement by
 * placing itself in the same section, with no change to audio.hpp -- its
 * functions are plain prototypes, satisfied by whichever backend source
 * file the project compiles in.
 *
 * COMPULSORY, not defaulted. There is no "unbanked" fallback value: a
 * default would silently place this module wherever the library guessed,
 * which is exactly the wrong-bank-with-no-diagnostic failure the rest of
 * this design works to make impossible. A project that banks nothing still
 * names the section its ordinary code lands in.
 */
#ifndef PNES_AUDIO_SECTION
#error "PNES_AUDIO_SECTION is not set. It names the ELF section this \
module's code is placed in, and MUST be the same bank the audio engine is \
assembled into (see the project's FamiStudio config). Set it from CMake -- \
local.cmake.example has a worked example."
#endif
// MODULE_PLACEMENT supplies the section and the noinline that makes the section
// mean anything (see its comment). No `used`/`retain`: a farcall opens a bank
// switch around an otherwise ordinary call, so LTO can see every reference, and
// pinning only keeps uncalled API alive.
#define AUDIO_BANK MODULE_PLACEMENT(PNES_AUDIO_SECTION)

#include <intsh>
using namespace br0::intsh;

#if !FAMISTUDIO_CFG_NTSC_SUPPORT && !FAMISTUDIO_CFG_PAL_SUPPORT
    #error "FamiStudio: neither NTSC nor PAL support enabled"
#endif

#ifdef __mos__
    #define FASTCALL __attribute__((cc65_fastcall)) void
#else
    #define FASTCALL void
#endif

/*
 * FamiStudio is assembled by a genuine standalone ca65 binary
 * (CMakeLists.txt), and its own config places it into whichever PRG-ROM bank
 * the project chose. EVERY FUNCTION IN THIS FILE IS TAGGED
 * ::AUDIO_BANK so it lands in that same bank -- which is the only reason the
 * calls below can be plain calls.
 *
 * THIS FILE CONTAINS NO BANK SWITCHING, deliberately, and includes no mapper
 * header. The caller long-calls audio::Update(); by the time control arrives
 * here the engine is already mapped, because it came in with this module. If
 * the engine reads data from a further bank (an exported song, say), mapping
 * that is likewise the caller's business, decided where the layout is decided
 * -- not hardcoded in the backend adapter.
 *
 * That keeps this file portable in the way that matters: swapping FamiStudio
 * for a different engine, or this board for one with no banking at all, is a
 * change to placement and to the caller, never to the code here.
 */
extern "C" FASTCALL famistudio_music_play(u8 song_index);
extern "C" FASTCALL famistudio_music_pause(u8 pause);
extern "C" FASTCALL famistudio_music_stop(void);
#if FAMISTUDIO_CFG_SFX_SUPPORT
extern "C" FASTCALL famistudio_sfx_play(u8 sfx_index, u8 channel);
#endif
extern "C" FASTCALL famistudio_update(void);

// famistudio_init wants its region byte in A: zero for PAL, non-zero for
// NTSC (famistudio_ca65.s's FAMISTUDIO_INIT doc comment) -- the inverse of
// this project's own Init(region) convention (1 = PAL, 0 = NTSC). `used`
// makes it addressable by symbol from the raw asm text below, the same
// pattern src/nes/input.cpp relies on for its poll scratch bytes (LLVM can't
// see the reference inside opaque asm, so without `used` LTO strips it).
extern "C" {
__attribute__((used)) u8 famistudio_region_scratch;
}

AUDIO_BANK void audio::Init(const u8 region) {
    famistudio_region_scratch = region ? 0 : 1;

    __asm__ volatile (
        "ldx #<%0\n"
        "ldy #>%0\n"
        "lda famistudio_region_scratch\n"
        "jsr famistudio_init\n"
        :
        : "i"(tracks)
        : "memory", "a", "x", "y", "c", "v"
    );

#if FAMISTUDIO_CFG_SFX_SUPPORT
    __asm__ volatile (
        "ldx #<%0\n"
        "ldy #>%0\n"
        "jsr famistudio_sfx_init\n"
        :
        : "i"(sfx)
        : "memory", "a", "x", "y", "c", "v"
    );
#endif
}

AUDIO_BANK void audio::TrackPlay(const u8 index) {
    famistudio_music_play(index);
}

AUDIO_BANK void audio::TrackPause(const u8 pause) {
    famistudio_music_pause(pause);
}

AUDIO_BANK void audio::TrackStop() {
    famistudio_music_stop();
}

AUDIO_BANK void audio::Update() {
    famistudio_update();
}

AUDIO_BANK void audio::SfxPlay(const u8 index, const u8 channel) {
#if FAMISTUDIO_CFG_SFX_SUPPORT
    famistudio_sfx_play(index, channel);
#else
    (void)index; (void)channel;
#endif
}

AUDIO_BANK void audio::SfxSamplePlay(const u8 index) {
#if FAMISTUDIO_CFG_SFX_SUPPORT
    famistudio_sfx_play(index, 1);
#else
    (void)index;
#endif
}
