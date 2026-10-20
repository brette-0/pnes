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
; TWO values are exceptions, both fed IN from CMake rather than owned here,
; and both for the same reason: they are not free choices of this file's,
; they must equal something decided elsewhere. The code segment must equal
; the bank pnes's audio module is compiled into (see below), and
; NTSC/PAL must equal the target being built. CMake's REGION variable (set from
; TARGET_PLATFORM, see CMakeLists.txt) is fed in below via -D, and this file
; derives FAMISTUDIO_CFG_NTSC_SUPPORT/PAL_SUPPORT from it rather than
; carrying its own separate literal -- CMakeLists.txt computes the identical
; values from that same REGION for its own compile definitions, so the two
; can't drift apart.

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
; The .ifdef is the same standalone-assemblability guard REGION uses below:
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

; REGION comes in from CMake (-D REGION=<0|1>, CMakeLists.txt's own REGION
; variable -- 0 for NTSC, 1 for PAL, set per TARGET_PLATFORM). The .ifndef
; fallback keeps this file directly assemblable standalone (ca65 on this
; file alone, no CMake in the loop) at its NTSC default, matching this
; file's own "real, project-owned config, not CMake-generated" charter above
; -- REGION only ever narrows which of these two FamiStudio itself still
; reads is 1, it never becomes the sole source of truth in place of this file.
.ifndef REGION
    REGION = 0
.endif

FAMISTUDIO_CFG_EXTERNAL     = 1
FAMISTUDIO_CFG_NTSC_SUPPORT = 1 - REGION
FAMISTUDIO_CFG_PAL_SUPPORT  = REGION
FAMISTUDIO_CFG_DPCM_SUPPORT = 1
FAMISTUDIO_CFG_SFX_SUPPORT  = 1
FAMISTUDIO_CFG_SFX_STREAMS  = 2
FAMISTUDIO_USE_VOLUME_TRACK = 1

.include "famistudio_ca65.s"
