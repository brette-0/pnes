; FamiStudio sound engine configuration -- a REAL, project-owned config
; file, not CMake-generated. Edit this directly for this project's own
; needs (a different game/song/SFX set would edit its own copy of this
; file, not a CMake cache variable).
;
; CMakeLists.txt reads most values back out of this file (regex-extracted)
; so audio.cpp's own #if FAMISTUDIO_CFG_* checks stay honest against
; whatever's actually configured here -- it never generates or owns this
; file's content. See BANKED_CALL_THEORY.txt's "RESOLVED: famistudio_config.s
; moves to the demo root as a real, directly-edited file" for the reasoning.
; ONE value is an exception, fed IN from CMake rather than owned here, and for
; a reason: it is not a free choice of this file's, it must equal something
; decided elsewhere -- the code segment must equal the bank pnes's audio module
; is compiled into (see below).
;
; NTSC/PAL USED TO BE A SECOND SUCH VALUE, derived from CMake's then-FRAMERATE
; variable (since removed). It no
; longer is: both note tables are enabled unconditionally and the engine selects
; between them at runtime from the measured refresh. See the block that sets
; them below. CMakeLists.txt still states the same pair for its own compile
; definitions, so the two cannot drift apart.

.define FAMISTUDIO_CA65_ZP_SEGMENT   ZEROPAGE
.define FAMISTUDIO_CA65_RAM_SEGMENT  BSS
; THE CODE SEGMENT IS NOT THIS FILE'S TO CHOOSE. The engine has to land in
; the same PRG bank as pnes's audio module, because that module
; reaches famistudio_update() with a PLAIN call and contains no bank
; switching at all -- put the two in different banks and you get a
; cross-bank call that executes whatever happens to be mapped, with no
; diagnostic. So the one value both sides share, local.cmake's
; PNES_AUDIO_SECTION (".prg_rom_audio"), is written here once and
; only once: CMakeLists.txt encodes it to the ca65 spelling and generates
; pnes_audio_segment.inc, which this file pulls in below.
;
; _pprg__rom__audio_fx is what ".prg_rom_audio" encodes to. ld.lld's XO65
; reader treats '_' in a ca65 segment name as an ESCAPE character, not a
; literal -- which is why a plain "FAMISTUDIO_CODE" is rejected outright
; ("unknown underscore escape"). The escapes, confirmed empirically against
; ld.lld itself: __ -> '_', _p -> '.', _d -> '$', _h -> '-', _xNN -> byte NN,
; _tp/_tn -> section type PROGBITS/NOBITS, _fw/_fx -> flags
; writable/executable. Note the DOUBLED underscores: every literal '_' has to
; be escaped as well, which is why this reads _pprg__rom__audio_fx and not
; _pprg_rom_audio_fx (that spelling is rejected -- '_r' is not an escape).
; Nobody hand-writes that any more; the encoder in CMakeLists.txt does.
;
; The .ifdef is a standalone-assemblability guard, the same shape as
; PNES_SFX_STREAMS's .ifndef further down:
; PNES_AUDIO_SEGMENT_INC comes in via -D from CMake (ca65's -D takes numeric
; symbols only, so it can select this branch but could never carry the
; segment NAME itself). Assembled by hand with no CMake in the loop, this
; file still works, at the literal that matches the demo's own link.ld.
;
; Naming the section after the BANK rather than after FamiStudio is
; deliberate: demo/link.ld's own .prg_rom_audio wildcard picks this up with
; no engine-specific rule, and a replacement audio engine written in C, C++
; or Rust targets the identical section with its own toolchain's mechanism
; (__attribute__((section(".prg_rom_audio"))) / #[link_section]). Nothing
; about the layout knows which engine is in the bank.
.ifdef PNES_AUDIO_SEGMENT_INC
    .include "pnes_audio_segment.inc"
.else
    .define PNES_AUDIO_SEGMENT _pprg__rom__audio_fx
.endif
.define FAMISTUDIO_CA65_CODE_SEGMENT PNES_AUDIO_SEGMENT

; BOTH NOTE TABLES, ALWAYS, AND THE ENGINE PICKS AT RUNTIME. These used to be
; 1-FRAMERATE / FRAMERATE -- mutually exclusive, so the build decided. It
; cannot: the console a ROM ends up in is not a build property (a PAL console
; running an NTSC image, a flash cart, an emulator defaulting to the wrong
; region), and the frame length is what the note tables have to match.
; src/nes/video.cpp measures it at boot into video::is50Hz, and
; src/nes/audio/famistudio.cpp hands that to famistudio_init through
; audio::Init's region argument -- which this engine has always taken.
;
; FRAMERATE is NOT consulted here any more, and no longer exists as a CMake
; variable at all -- CMake's REGION carries "which market is this build for",
; and the NES2.0 timing byte in demo/src/header.hpp says Timing::MULTI.
;
; COST IS THE SECOND NOTE TABLE IN ROM, and that is the whole price of the
; change on this target.
FAMISTUDIO_CFG_EXTERNAL     = 1
FAMISTUDIO_CFG_NTSC_SUPPORT = 1
FAMISTUDIO_CFG_PAL_SUPPORT  = 1
FAMISTUDIO_CFG_DPCM_SUPPORT = 1
FAMISTUDIO_CFG_SFX_SUPPORT  = 1
; PNES_SFX_STREAMS comes in from CMake (-D PNES_SFX_STREAMS=<n>), the SAME
; value it hands the C++ side as audio::sfxChannels -- the third of this
; file's CMake-fed values, and fed in for the same reason as the other two:
; it is not a free choice of this file's, because both sides have to agree on
; how many streams exist. Pick 3 here while the library still believes 2 and
; a project's third channel addresses a stream the engine never allocated,
; with nothing to say so. Standalone .ifndef fallback at the value this project
; actually uses, so this file still assembles with no CMake in the loop.
.ifndef PNES_SFX_STREAMS
    PNES_SFX_STREAMS = 2
.endif
FAMISTUDIO_CFG_SFX_STREAMS  = PNES_SFX_STREAMS
FAMISTUDIO_USE_VOLUME_TRACK = 1

.include "famistudio_ca65.s"
