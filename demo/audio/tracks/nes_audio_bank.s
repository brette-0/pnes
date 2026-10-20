; Places the exported FamiStudio song data into its own PRG-ROM bank.
;
; demo/audio/tracks/nes_audio.s is a GENERATED artifact (re-exported from
; demo/nes.fms on every build, see CMakeLists.txt) and carries no .segment
; directive of its own, so its data would otherwise land in ca65's default
; CODE segment and be merged into ordinary .text. This wrapper is the seam:
; it opens the target segment and pulls the generated file in, leaving the
; generator's output untouched and re-exportable.
;
; _pprg__rom__music decodes to the ELF section ".prg_rom_music" -- ld.lld's
; XO65 reader reads '_' as an escape ('_p' -> '.', '__' -> a literal '_'); see
; demo/famistudio_config.s for the full escape table. No _fx: this is data.
;
; ITS OWN SECTION, deliberately NOT the engine's. The engine and pnes's
; audio module must share a bank -- that adjacency is what lets audio.cpp call
; the engine directly. Song data has no such requirement and must not inherit
; one: welding it to the engine would cap every song this cart can ever hold at
; whatever is left of that one bank. Where it actually lands is demo/link.ld's
; decision, and it only has to be in a DIFFERENT window from the engine, since
; the engine walks this data while it executes and both are mapped at once.
;
; CMakeLists.txt excludes nes_audio.s itself from the assembly glob so it is
; assembled ONLY through this file -- the same treatment famistudio_config.s
; already gets. Assembling both would define every symbol twice.

.segment "_pprg__rom__music"
.include "nes_audio.s"
