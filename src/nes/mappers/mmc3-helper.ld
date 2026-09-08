/*
 * NES-MMC3 (mapper 4) linker helper script -- LIBRARY-OWNED.
 *
 * Real MMC3 hardware addressing (PRG mode 0, CHR mode 0 -- the only
 * combination mmc3.hpp/mmc3.cpp support; see mmc3.hpp's file comment):
 *   $8000-$9FFF  8 KiB PRG-ROM, switchable  (R6, via $8000/$8001)
 *   $A000-$BFFF  8 KiB PRG-ROM, switchable  (R7, via $8000/$8001)
 *   $C000-$DFFF  8 KiB PRG-ROM, FIXED to the second-to-last physical bank
 *   $E000-$FFFF  8 KiB PRG-ROM, FIXED to the last physical bank
 *   $6000-$7FFF  up to 8 KiB PRG-RAM ("cartridge memory", see mmc3.hpp's
 *                ::cartmem)
 *
 * Mirrors vrc1-helper.ld's own split as closely as MMC3's hardware allows:
 * this file owns only genuinely mechanical/hardware MMC3 facts (window
 * geometry, the two fixed banks, the reset vector, PRG-RAM's address). A
 * consuming project's own top-level script still owns __prg_rom_size/
 * __chr_rom_size and any of its own MMC3_BANKED()-tagged domains -- see
 * vrc1-helper.ld's own file comment and BANKED_CALL_THEORY.txt's "THE FINAL
 * LINKER SCRIPT MUST BE USER-MADE" section for the reasoning this follows.
 * (See demo/link.ld for a worked example of that split for VRC1; an MMC3
 * project's own top-level script would follow the same shape, `INCLUDE
 * mmc3-helper.ld` in place of `INCLUDE vrc1-helper.ld`.)
 *
 * UNLIKE VRC1's three independently-selectable PRG windows, MMC3 in PRG
 * mode 0 only exposes TWO registers a project can point anywhere ($8000 via
 * R6, $A000 via R7). $C000-$DFFF is hardware-fixed to the second-to-last
 * bank regardless of any register write -- not a third switchable window.
 *
 * PRG-ROM SIZE: anything from 32 KiB up to 512 KiB, MMC3's own hardware
 * maximum -- R6/R7 are 6-bit bank numbers on real TxROM silicon (bits 0-5;
 * bits 6-7 are ignored), so 64 x 8 KiB is all the chip can address, and the
 * ASSERT below rejects more rather than letting a project link a ROM whose
 * top banks no register can ever select. The consuming project still owns
 * __prg_rom_size itself (see this file's own split rationale above); it just
 * has to be PROVIDE()d BEFORE `INCLUDE mmc3-helper.ld`, since the layout
 * below is computed from it.
 *
 * HOW SIZE-INDEPENDENCE IS ACHIEVED (this is the part that used to be a
 * hardcoded 4-bank assumption, in this file AND in the first consumer's own
 * script): the always-resident image is placed at the TOP of the ROM file,
 * not the bottom. File order is
 *
 *   [ consumer's own MMC3_BANKED() domains, banks 0..R-1 ]
 *   [ prg_rom_spare -- zero padding, whatever is left over ]
 *   [ prg_rom_switchable -- __mmc3_resident_windows banks  ]
 *   [ prg_rom_fixed      -- exactly 1 bank, the last one   ]
 *
 * so the resident windows always occupy the LAST (1 + __mmc3_resident_
 * windows) physical banks no matter how big the ROM is. Because a real
 * bank's number is file_offset/0x2000 (see vrc1-helper.ld's MULTI-BANK
 * ENCODING note), that makes the two facts MMC3 hardwires -- "$C000 shows
 * the second-to-last bank" and "$E000 shows the last bank" -- line up with
 * linked content by construction:
 *
 *   - prg_rom_fixed IS the last bank            -> $E000-$FFFF, always.
 *   - at __mmc3_resident_windows == 3, prg_rom_switchable's third 8 KiB
 *     chunk IS the second-to-last bank          -> $C000-$DFFF, always.
 *   - its first two chunks are banks (N-1-windows) and (N-windows), which
 *     is exactly what ::_reset pokes into R6/R7 -- it reads those bank
 *     numbers from __mmc3_boot_bank_window1/2, defined below, rather than
 *     the literal 0/1 it used to assume.
 *
 * The earlier revision of this file did the opposite (resident image at the
 * bottom, banks 0..3) and therefore had to forbid __mmc3_resident_windows
 * == 3 for any ROM that wasn't EXACTLY 4 banks: "second-to-last" only means
 * "the third 8 KiB chunk in file order" when there are exactly 4 chunks, so
 * the moment a 5th bank appeared, whatever ordinary code the linker had put
 * in that third chunk stopped being what $C000 actually showed and became
 * silently unreachable. Anchoring to the top of the file instead makes 3
 * windows correct at EVERY size, which is why it is now the default here
 * and why no consumer needs to commit to an exact bank count any more.
 *
 * A project that would rather keep $C000-$DFFF for something of its own
 * (its own fixed-address carve-out placed in the second-to-last bank, e.g.)
 * sets __mmc3_resident_windows = 2, declares its own MEMORY region at
 * ORIGIN 0xc000 (no bank-encoding needed -- it is never reached via a
 * register), and FULL()s it immediately BEFORE prg_rom_switchable in its own
 * OUTPUT_FORMAT so it lands in the second-to-last bank. This file does not
 * do that on a project's behalf, the same reasoning vrc1-helper.ld's own
 * file comment gives for not making project-specific domain decisions here.
 *
 * $6000-$7FFF (PRG-RAM, "cartridge memory") is genuinely optional on real
 * MMC3 boards, but this file always reserves it (size driven by
 * __program_ram_size/__program_nvram_size, PROVIDE()'d below with a
 * working default) so ::cartmem has real backing memory with no extra
 * consumer configuration required -- override either before this INCLUDE
 * (or to 0) to change that.
 *
 * DELIBERATELY NOT __prg_ram_size/__prg_nvram_size (the SDK's own canonical
 * names, used by e.g. nes-mmc3/nes-mmc1's own common.ld and by
 * ines-header.ld): confirmed the generic "nes" platform's own crt0.o
 * already contains a WEAK object-file definition of those two names (both
 * 0; `llvm-nm crt0.o` shows `00000000 W __prg_ram_size`), and a weak symbol
 * still counts as "defined" as far as PROVIDE() is concerned -- it silently
 * lost to crt0.o's weak 0 in a real link, collapsing prg_ram's own LENGTH
 * below to 0. Rather than fight that with a plain (non-PROVIDE) assignment,
 * this file uses its own distinct names instead: nothing else defines
 * __program_ram_size, so PROVIDE() behaves exactly as advertised. The only
 * real cost is that the canonical __prg_ram_size stays whatever crt0.o
 * defaults it to (0) -- harmless here, since this project doesn't use
 * ines-header.ld at all (see vrc1-helper.ld's own NOTE ON __mapper comment)
 * and nes.ld's only use of it is a power-of-2 ASSERT that 0 trivially
 * satisfies regardless of this file's real PRG-RAM size.
 *
 * c_writeable/__stack are NOT set here at all -- not even as an inlined
 * default the way an earlier revision of this file did (replacing
 * `INCLUDE c-in-ram.ld` with its own two equivalent lines). Confirmed
 * directly against this project's own build that a REGION_ALIAS, unlike a
 * plain assignment, cannot be overridden by a consuming project afterward
 * (ld.lld rejects a second alias for the same name outright) -- so baking
 * ANY default in here would permanently commit every consumer to whichever
 * region this file picked, with no escape short of forking it. See the
 * comment right above the (now absent) REGION_ALIAS/__stack lines, further
 * down this file, for what a consuming project should write instead.
 *
 * This file INCLUDEs nes-no-pow2.ld, not the SDK's own nes.ld -- MMC3 needs
 * the same relief VRC1 does. __mmc3_reserved_banks/__mmc3_resident_windows
 * make a project's real total bank count (__mmc3_prg_rom_banks) an arbitrary
 * sum, not a power of 2 by construction -- e.g. 3 resident + 5 reserved + 1
 * fixed = 9 banks (72 KiB), which stock nes.ld's ASSERT would reject outright
 * even though nothing about MMC3's hardware (R6/R7 are plain 6-bit bank
 * numbers, same as VRC1's 4-bit ones) or this project's own NES2.0 header
 * builder requires it. See nes-no-pow2.ld's own file comment for the full
 * investigation of why the stock ASSERT is stricter than actually needed.
 */

INCLUDE nes-no-pow2.ld

PROVIDE(__program_ram_size = 8);
PROVIDE(__program_nvram_size = 0);

ASSERT(__program_ram_size + __program_nvram_size <= 8,
       "MMC3 only supports up to 8 KiB of PRG-(NV)RAM.")

/* How many 8 KiB windows starting at $8000 count as "ordinary resident
 * space" (prg_rom_switchable's own LENGTH, below) -- 3 by default, i.e. the
 * full flat $8000-$DFFF span: R6's window, R7's window, and the hardware-
 * fixed second-to-last-bank window at $C000. All three are correct at every
 * PRG-ROM size now that the resident image is anchored to the TOP of the ROM
 * file (see this file's header comment) -- this used to default to 2, back
 * when the third window was only safe on an exactly-4-bank ROM and every
 * consumer wanting it had to commit to that bank count itself.
 *
 * A project that wants $C000-$DFFF for a carve-out of its own instead sets
 * this to 2 by PROVIDE()-ing it BEFORE `INCLUDE mmc3-helper.ld` -- PROVIDE()
 * only takes its first-encountered value, so before-the-INCLUDE is the only
 * ordering that wins. Never 1 or 0: R6's own window ($8000-$9FFF) is where
 * ordinary untagged code lands, so it is always resident space here
 * regardless.
 *
 * MUST stay above the ASSERTs and layout arithmetic below, which read it. */
PROVIDE(__mmc3_resident_windows = 3);

/* 1 IS NOW LEGAL, and means something the "never 1 or 0" note above did not
 * anticipate: the resident image is ONLY $C000-$DFFF, leaving BOTH switchable
 * windows free for a single 16 KiB project domain at $8000-$BFFF. That is the
 * only way to express a banked domain larger than one window -- a farcall
 * cannot enter one (CallInSection switches a single register), so such a
 * domain is switched once on mode entry instead, and everything outside it
 * must live in the hardwired $C000 window or the fixed bank.
 *
 * The resident image therefore ANCHORS AT $DFFF AND GROWS DOWNWARD, which is
 * what makes the freed windows contiguous with $8000. 3 still yields the
 * historic $8000-$DFFF exactly; 2 yields $A000-$DFFF (NOT the $8000-$BFFF the
 * old comment describes -- a resident span that left a hole at $C000 could
 * never have been reconstructed by R6/R7 anyway, since $C000 is hardwired). */
ASSERT(__mmc3_resident_windows >= 1 && __mmc3_resident_windows <= 3,
       "__mmc3_resident_windows must be 1 ($C000-$DFFF), 2 ($A000-$DFFF) or 3 ($8000-$DFFF) -- MMC3 exposes no other resident PRG span.")

/* Banks at the BOTTOM of the ROM file a consuming project is claiming for
 * its own MMC3_BANKED()-tagged domains (0 by default -- this library has no
 * opinion on whether a project needs any, it just leaves the hook open, the
 * same way vrc1-helper.ld's own __prg_rom_switchable_reserved does). Those
 * domains' own MEMORY regions/SECTIONS rules belong in the consuming
 * project's script; all this number does is shorten prg_rom_spare below by
 * the same amount, so the total ROM image still comes out at exactly
 * __prg_rom_size. Set it BEFORE `INCLUDE mmc3-helper.ld`, and FULL() those
 * domains FIRST in the project's own OUTPUT_FORMAT (before prg_rom_spare) --
 * that is what makes their physical bank numbers 0..N-1, stable regardless
 * of total ROM size. */
PROVIDE(__mmc3_reserved_banks = 0);

/* MMC3's own PRG ceiling: R6/R7 are 6-bit bank numbers (bits 0-5 of the
 * value written to $8001; bits 6-7 are ignored by the chip), so 64 banks x
 * 8 KiB = 512 KiB is the most any register can reach. nes.ld's own
 * power-of-2 ASSERT is gone (nes-no-pow2.ld, INCLUDEd above -- see this
 * file's header comment), so these two add the mapper-specific floor and
 * ceiling in its place. The floor is not a fixed 32 KiB: it is whatever the
 * resident image itself needs (the windows below, plus the fixed bank), so a
 * project that drops to 2 resident windows can legally build an exact 24 KiB
 * ROM now -- it no longer gets rounded up to 32. */
ASSERT(__prg_rom_size <= 512,
       "MMC3 addresses at most 512 KiB of PRG-ROM (R6/R7 are 6-bit bank numbers) -- __prg_rom_size is larger, so the top banks would be unreachable by any register write.")
ASSERT(__prg_rom_size >= (__mmc3_resident_windows + __mmc3_reserved_banks + 1) * 8,
       "__prg_rom_size is smaller than what is already spoken for: __mmc3_resident_windows * 8 KiB + __mmc3_reserved_banks * 8 KiB + the 8 KiB fixed bank.")

/* Total 8 KiB PRG banks, and the physical bank numbers ::_reset must poke
 * into R6/R7 at boot to reconstruct the flat resident image. The resident
 * windows sit at the TOP of the ROM file (see this file's header comment
 * for the full layout and why), so window 1's bank is NOT 0 for any ROM
 * bigger than the resident image itself -- mmc3.cpp reads both of these as
 * linker-defined symbols rather than assuming 0/1 the way it did while this
 * script only supported 32 KiB. Not PROVIDE(): these are computed facts
 * about the layout this file itself imposes, never a consumer's to override. */
__mmc3_prg_rom_banks     = __prg_rom_size / 8;
__mmc3_boot_bank_window1 = __mmc3_prg_rom_banks - __mmc3_resident_windows - 1;
__mmc3_boot_bank_window2 = __mmc3_boot_bank_window1 + 1;
/* AT __mmc3_resident_windows == 1 these two land on the two HARDWIRED banks
 * (N-2 and N-1), so boot mirrors $C000-$FFFF into $8000-$BFFF rather than
 * extending the resident image. Deliberate, not a degenerate case: nothing is
 * linked to run at $8000-$BFFF in that configuration, so the safest possible
 * power-on state is every window showing linked, valid content instead of
 * whatever the cartridge powers up with. The project's own mode-entry switch
 * replaces both the moment it wants its domain. */

/* c_writeable/__stack are DELIBERATELY NOT set here, unlike an earlier
 * revision of this file -- same reasoning as the project's own established
 * "you don't need c-in-ram.ld, implement it yourself" conclusion (this
 * file's own two lines were themselves already that same trick, replacing
 * the SDK's c-in-ram.ld): where the C writable arena (.data/.bss/heap, and
 * the dynamic stack that shares it -- see this project's own memory on
 * llvm-mos soft stacks) lives is project policy, not an MMC3 hardware fact,
 * and REGION_ALIAS specifically CANNOT be overridden after the fact the way
 * __stack alone can (confirmed empirically: ld.lld rejects a second
 * REGION_ALIAS for the same name outright, "redefinition of memory
 * region" -- unlike a plain assignment, where the last one processed simply
 * wins). Baking a default in here would therefore permanently commit every
 * consumer to system RAM, with no way to move it to PRG-RAM afterward
 * (exactly what this project's own demo/link.ld now does) short of forking
 * this file. A consuming project must declare its own
 * `REGION_ALIAS("c_writeable", <region>)` and `__stack = <address>;` AFTER
 * `INCLUDE mmc3-helper.ld` (the referenced region -- ram or prg_ram, both
 * declared below -- must already exist by then) -- see demo/link.ld for a
 * worked example targeting prg_ram instead of ram.
 */

MEMORY {
  /* $8000-$DFFF by default (24 KiB, the full flat resident span); $8000-$BFFF
   * if a project sets __mmc3_resident_windows to 2 (see its own comment
   * above). Ordinary untagged code/data lives here. Always the top banks of
   * the ROM file, immediately below prg_rom_fixed -- that placement is what
   * makes the third window really be the second-to-last physical bank at any
   * ROM size, see this file's own header comment. */
  prg_rom_switchable : ORIGIN = 0xe000 - __mmc3_resident_windows * 0x2000,
                       LENGTH = __mmc3_resident_windows * 0x2000
  /* Whatever is left of __prg_rom_size once the resident image, the fixed
   * bank, and any banks the project reserved for its own domains are
   * accounted for: pure zero padding, so the emitted file is exactly the
   * size the NES2.0 header claims. Never CPU-addressed (nothing is placed
   * here), so like nes2_header/chr_rom its ORIGIN is a fake address that
   * exists only for FULL()'s bookkeeping -- 0x03000000, the next one free
   * after nes.ld's chr_rom (0x01000000) and nes2_header (0x02000000).
   * LENGTH is legitimately 0 for a ROM with no slack (e.g. 32 KiB with 3
   * resident windows -- the exact shape this script supported before it
   * grew size independence); FULL() of an empty region emits nothing, the
   * same way nes.ld's own chr_rom region behaves on a CHR-RAM-only cart. */
  prg_rom_spare      : ORIGIN = 0x03000000,
                       LENGTH = (__mmc3_prg_rom_banks - __mmc3_resident_windows
                                 - __mmc3_reserved_banks - 1) * 0x2000
  /* Hardwired to the LAST physical bank, unconditionally, regardless of any
   * register -- the one truly permanent, always-resident home, matching
   * VRC1's own $E000-$FFFF exactly. .prg_rom_fixed-tagged content only. */
  prg_rom_fixed       : ORIGIN = 0xe000, LENGTH = 0x2000
  /* $6000-$7FFF: PRG-RAM ("cartridge memory", mmc3.hpp's ::cartmem). See
   * this file's own header comment on __program_ram_size/__program_nvram_size. */
  prg_ram             : ORIGIN = 0x6000, LENGTH = (__program_ram_size + __program_nvram_size) * 1024
  /* The 16-byte NES2.0 header -- same bookkeeping-only region VRC1 uses,
   * never CPU/PPU-addressed. Filled by header.hpp's NES2_HEADER() macro. */
  nes2_header         : ORIGIN = 0x02000000, LENGTH = 16
}

/* Region geometry, republished as ordinary linker symbols so a build-time
 * space report (tools/report_space.cmake, wired up in CMakeLists.txt) can
 * describe every region WITHOUT restating this file's layout in a second
 * place. A region that holds nothing emits no output section at all, so the
 * linked ELF alone cannot even tell you an empty 8 KiB bank exists -- these
 * symbols are what make "bank 2: 0 / 8192 used" reportable. Paired by name:
 * __space_origin_X / __space_length_X. A consuming project should publish the
 * same two symbols for any region of its own (see demo/link.ld's domains). */
__space_origin_prg_rom_switchable = ORIGIN(prg_rom_switchable);
__space_length_prg_rom_switchable = LENGTH(prg_rom_switchable);
__space_origin_prg_rom_fixed      = ORIGIN(prg_rom_fixed);
__space_length_prg_rom_fixed      = LENGTH(prg_rom_fixed);
__space_origin_prg_rom_spare      = ORIGIN(prg_rom_spare);
__space_length_prg_rom_spare      = LENGTH(prg_rom_spare);
__space_origin_prg_ram            = ORIGIN(prg_ram);
__space_length_prg_ram            = LENGTH(prg_ram);
__space_origin_ram                = ORIGIN(ram);
__space_length_ram                = LENGTH(ram);
__space_origin_zp                 = ORIGIN(zp);
__space_length_zp                 = LENGTH(zp);
__space_origin_chr_rom            = ORIGIN(chr_rom);
__space_length_chr_rom            = LENGTH(chr_rom);

REGION_ALIAS("c_readonly", prg_rom_switchable)

SECTIONS {
  .prg_rom_fixed : { *(.prg_rom_fixed .prg_rom_fixed.*) } >prg_rom_fixed
  /* mmc3.hpp's ::cartmem / ::sysmem -- explicit memory positioning.
   * NOLOAD: runtime-only storage, nothing to load from the ROM file. */
  .cartmem (NOLOAD) : { *(.cartmem .cartmem.*) } >prg_ram
  .sysmem  (NOLOAD) : { *(.sysmem .sysmem.*) } >ram
  .nes2_header   : { KEEP(*(.nes2_header)) } >nes2_header
}

/* _nes2_header (see header.hpp's NES2_HEADER()) is otherwise never
 * referenced by any code, so without this the linker would never pull that
 * object out of libplatform-nes.a -- same reasoning as vrc1-helper.ld's
 * own EXTERN(_nes2_header). */
EXTERN(_nes2_header)

INCLUDE rompoke-table.ld
INCLUDE dpcm.ld
INCLUDE debug-log.ld

/* Falls back to _start when nothing provides a real _reset (e.g. CMake's
 * own internal compiler-identification/ABI probes) -- same reasoning as
 * vrc1-helper.ld's own PROVIDE(_reset = _start). */
PROVIDE(_reset = _start);

SECTIONS {
  .vector 0xfffa : { SHORT(nmi) SHORT(_reset) SHORT(irq) } >prg_rom_fixed
}

/* ONLY the header goes here -- every PRG region's FULL() belongs in the
 * consuming project's own top-level script, in this exact order:
 *
 *   OUTPUT_FORMAT {
 *     <the project's own MMC3_BANKED() domains, if any>  <- banks 0..N-1
 *     FULL(prg_rom_spare)
 *     FULL(prg_rom_switchable)
 *     FULL(prg_rom_fixed)
 *     FULL(chr_rom)
 *   }
 *
 * OUTPUT_FORMAT is additive across INCLUDEd files and processed in order
 * (confirmed empirically -- see vrc1-helper.ld's own OUTPUT_FORMAT comment),
 * and everything this file emits necessarily comes BEFORE anything the
 * including project emits. That is exactly why prg_rom_switchable is no
 * longer FULL()'d here as it was while this script was 32 KiB-only:
 * emitting it from this file would force it ahead of the consumer's own
 * banked domains, putting those domains between the resident image and the
 * fixed bank -- i.e. stealing the second-to-last bank out from under
 * $C000-$DFFF and shifting the boot banks __mmc3_boot_bank_window1/2
 * predict. The resident image has to be the LAST thing before
 * prg_rom_fixed, and only the consumer's own block can guarantee that.
 * See demo/link.ld for a worked example. */
OUTPUT_FORMAT {
  FULL(nes2_header)
}
