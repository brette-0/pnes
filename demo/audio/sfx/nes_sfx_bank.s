; Places the FamiStudio sound-effect export into the audio DATA bank.
;
; Same seam, and the same reason, as demo/audio/tracks/nes_audio_bank.s:
; sfx.s is FamiStudio's own export (regenerated from demo/audio/sfx.fms on
; every build, see CMakeLists.txt) and carries no .segment of its own, so its
; data would land in ca65's default CODE segment and be merged into ordinary
; resident .text.
;
; THAT IS NOT MERELY UNTIDY, IT IS BROKEN: src/nes/audio.cpp reaches the
; engine through mmc3::CallPairedBlock, which maps the engine's bank over
; $8000-$9FFF and this data bank over $A000-$BFFF for the duration of the
; call. Everything ordinarily resident in $8000-$BFFF is therefore NOT
; mapped while the engine runs -- so SFX data left in .text would be read as
; whatever the music bank happens to hold at that offset. Caught exactly that
; way: `sounds` had landed at $A9B6, inside the window the music data
; displaces.
;
; UNLIKE the music export, it is not written beside this wrapper: it lands in
; the BUILD directory, which is why the .include below has no path and relies
; on ca65's -I. SFX data is region-baked -- the exported `sounds` table holds
; an NTSC pointer and a PAL one, and only the built region's is filled -- so
; the NTSC and PAL configurations generate different bytes. Sharing one path
; in the source tree would let whichever built last satisfy the other's
; up-to-date check. See CMakeLists.txt's NES_SFX_ASM for the full reasoning.
;
; _pprg__rom__sfx decodes to section ".prg_rom_sfx" (ld.lld XO65 escapes:
; _p -> '.', __ -> '_'); no _fx, this is data. Its own section rather than the
; engine's or the music's, so the linker script can place all three
; independently -- see nes_audio_bank.s's comment for why data is never forced
; to sit beside the engine.

.segment "_pprg__rom__sfx"
.include "sfx.s"
