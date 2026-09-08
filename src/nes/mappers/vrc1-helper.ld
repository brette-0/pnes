/*
 * NES-VRC1 (mapper 75) linker helper script -- LIBRARY-OWNED.
 *
 * Real VRC1 hardware addressing:
 *   $8000-$9FFF  8 KiB PRG-ROM, switchable  (PRG Select 0, write $8000-$8FFF)
 *   $A000-$BFFF  8 KiB PRG-ROM, switchable  (PRG Select 1, write $A000-$AFFF)
 *   $C000-$DFFF  8 KiB PRG-ROM, switchable  (PRG Select 2, write $C000-$CFFF)
 *   $E000-$FFFF  8 KiB PRG-ROM, FIXED to the last physical bank
 *
 * BANKED CALL, Phase 5: this file is what used to be the whole of vrc1.ld,
 * split so THE LIBRARY only ever owns the genuinely hardware/mechanical
 * facts, never a real project's own linking decisions (see
 * BANKED_CALL_THEORY.txt's "THE FINAL LINKER SCRIPT MUST BE USER-MADE"
 * section for the reasoning this split follows). What stays here:
 *
 * - MEMORY: prg_rom_switchable/prg_rom_fixed's ORIGIN/LENGTH are pure VRC1
 *   hardware facts, true for any project targeting this chip.
 *   nes2_header is mechanical NES2.0-format bookkeeping, not project policy.
 * - The reset vector (.vector, PROVIDE(_reset=_start)) and .nes2_header
 *   extraction (EXTERN(_nes2_header)): mechanical, crt0-required plumbing.
 * - REGION_ALIAS("c_readonly", ...): wires ordinary/untagged code into
 *   prg_rom_switchable, true regardless of what any one project adds on top.
 *
 * What does NOT live here, and belongs in the CONSUMING project's own
 * top-level script instead (see demo/link.ld for the reference example):
 * - __prg_rom_size/__chr_rom_size and their ASSERTs: this project's own ROM
 *   size, not a VRC1 fact.
 * - Any BANKED()-tagged domain's own MEMORY region/SECTIONS rule/ASSERTs:
 *   which sections a project's own code lives in, and how much space each
 *   needs, is that project's decision, never this library's to make or
 *   name on their behalf (see BANKED_CALL_THEORY.txt's "REMOVING THE
 *   ENFORCED SECTION-NAMING CONVENTION").
 *
 * PRG-ROM is split into two real, address-bound MEMORY regions:
 * prg_rom_switchable ($8000-$DFFF, up to 24 KiB -- windows 1-3) and
 * prg_rom_fixed ($E000-$FFFF, 8 KiB -- hardwired). Content explicitly
 * tagged .prg_rom_fixed (see ::fixed in vrc1.hpp) is guaranteed by the
 * linker to fit inside that 8 KiB, erroring out on overflow rather than
 * silently landing wherever ordinary switchable content happens to end --
 * which is what an earlier version of this script did by aliasing
 * prg_rom_fixed onto the *whole* flat $8000-$FFFF region, and which broke
 * the reset vector (see below). Ordinary untagged code/data goes into
 * prg_rom_switchable.
 *
 * __prg_rom_switchable_reserved (PROVIDE()'d below, default 0): lets a
 * consuming project reserve bytes off prg_rom_switchable's own tail for
 * its own dedicated, fixed-address carve-out regions (see demo/link.ld's
 * prg_rom_window_test for a worked example) -- this library has no opinion
 * on whether or how much a project needs, it just leaves the hook open.
 *
 * At a project's PRG-ROM size (32 KiB = exactly 4 banks) with no explicit
 * .prg_rom_bankN tags used, each switchable window has only one physical
 * bank it could ever show once boot-time PRG Select 0/1/2 initialization
 * (see _reset, below) has run -- so every existing call behaves like an
 * ordinary same-bank call with zero source changes, while the physical ROM
 * image, NES2.0 header (mapper 75) and per-window addressing are genuine
 * VRC1. Growing past that (multiple real banks per window, or a domain
 * spanning more than one window) needs explicit .prg_rom_bankN tagging --
 * see BANKED_CALL_THEORY.txt's Phase 3/4 notes, and demo/link.ld for a
 * real, worked example of both.
 *
 * RESET VECTOR: real VRC1 hardware powers up with the PRG-select registers
 * in an undefined state, so at cold boot the switchable windows ($8000-
 * $DFFF) can show ANY physical bank -- not necessarily the one this
 * script's flat layout assumes goes there. Only the fixed window ($E000-
 * $FFFF) is guaranteed correct at power-on. crt0's own entry point, _start
 * (see mos-platform/common/lib/text-sections.ld: `_start = .;`, the very
 * first byte of ordinary .text), therefore lands in the switchable region
 * and is NOT safe to jump to directly from reset -- nor is any of crt0's
 * own startup code that runs before main, since that's ordinary .text too.
 * The reset vector below instead targets _reset (defined in vrc1.cpp,
 * tagged ::fixed): a minimal stub that performs the boot-time PRG Select
 * 0/1/2 initialization *before* anything in the switchable region has
 * executed, then falls through into _start once doing so is actually safe.
 * This mirrors the _reset/.reset pattern nes-mmc3's own linker scripts use
 * for exactly the same reason (see mos-platform/nes-mmc3/lib/_reset.ld and
 * reset-banked-8.o, which does the equivalent for MMC3's own bank-select
 * registers).
 *
 * NOTE ON __mapper: earlier revisions of this script set the linker symbol
 * __mapper here, for llvm-mos's own ines-header.ld generator. This project's
 * NES2.0 header is now built entirely by header.hpp's HEADER() macro from
 * the MAPPER/SUBMAPPER compiler defines (see CMakeLists.txt and nes2.cpp),
 * which ines-header.ld never sees -- this script doesn't INCLUDE it -- so
 * __mapper is dead weight and is no longer defined here.
 *
 * COROLLARY: using the generic nes platform (rather than nes-nrom) also
 * drops nes-nrom crt0.o's weak __prg_rom_size/__chr_rom_size defaults
 * (32/8) -- it's the only crt0 that happens to define them, as a side
 * effect of being NROM-specific. The generic nes platform's crt0.o leaves
 * both undefined, so the CONSUMING project's own top-level script must
 * PROVIDE them (see demo/link.ld) or linking fails with "symbol not found:
 * __prg_rom_size".
 *
 * POWER-OF-2: `INCLUDE nes-no-pow2.ld` below, not the SDK's own nes.ld --
 * see nes-no-pow2.ld's own file comment for the full investigation of why
 * that fork exists (short version: an ordinary ASSERT, not an ld.lld
 * limitation, stricter than anything this project actually needs; real
 * hardware justification exists for OTHER, simpler mapper boards, not VRC1,
 * which has genuine internal register/decode logic, not a passive latch).
 *
 * MULTI-BANK ENCODING: a genuinely ambiguous window (more than one physical
 * bank can appear there) needs the LINKED ADDRESS of each candidate bank's
 * content to stay DISTINGUISHABLE from every other candidate's, even though
 * they all execute at the SAME real CPU address once banked in. This
 * project borrows the exact convention llvm-mos's own banked NES platforms
 * (nes-unrom, nes-mmc1, nes-mmc3, ...) already use for this, verified
 * empirically against nes-unrom's own _prg-rom-banked.ld and a real linked
 * test binary before reusing it here: give bank N's MEMORY region an
 * ORIGIN of (N * 0x10000) + its real CPU address, N=0 for the default/
 * already-existing bank (whose origin is therefore just its plain real
 * address, unchanged). This keeps every bank's LMA numerically disjoint at
 * link time (GNU ld/LLD MEMORY regions just need non-overlapping ORIGIN/
 * LENGTH, and 0x10000 strides guarantee that regardless of real window
 * size), while every actual REFERENCE to a symbol placed there -- an
 * absolute JSR operand, or a runtime pointer value, confirmed both ways --
 * automatically truncates to the low 16 bits, because 6502/65C02
 * relocations only ever have 16 bits of room to begin with. No compiler or
 * linker feature beyond ordinary relocation truncation is involved; the
 * extra high bits are pure linker-internal bookkeeping that never survives
 * into an emitted instruction. A REAL BANK'S NUMBER IS file_offset/0x2000,
 * IN THE ORDER regions are listed in the consuming project's own
 * OUTPUT_FORMAT block -- not the order a human happens to think of them.
 * Confirmed the hard way: a wrong hand-entered base bank, never cross-
 * checked against real file layout, shipped a real crash (garbage
 * execution -- see BANKED_CALL_THEORY.txt's "CRITICAL POST-PHASE-4 FIX" and
 * the oversized-domain notes for the full account). Any project defining
 * its own explicit-bank domains should add the same kind of build-time
 * ASSERT demo/link.ld does, checking the real bank arithmetic rather than
 * trusting a hand-picked number.
 */

INCLUDE nes-no-pow2.ld
INCLUDE c-in-ram.ld

PROVIDE(__prg_rom_switchable_reserved = 0);

ASSERT(__chr_rom_size <= 8,
       "This VRC1 script does not wire up CHR banking; only the existing flat 8 KiB of CHR-ROM is supported.")

MEMORY {
  /* Windows 1-3: $8000-$DFFF, minus whatever the consuming project reserves
   * off the tail via __prg_rom_switchable_reserved (see its own comment
   * above). Ordinary untagged code/data lives here. */
  prg_rom_switchable : ORIGIN = 0x8000, LENGTH = 0x6000 - __prg_rom_switchable_reserved
  /* Hardwired to the last physical bank on real hardware, regardless of any
   * PRG-select register's value. .prg_rom_fixed-tagged content only. */
  prg_rom_fixed       : ORIGIN = 0xe000, LENGTH = 0x2000
  /* The 16-byte NES2.0 header prefixed to the ROM file itself -- never CPU-
   * or PPU-addressed, so (like nes.ld's chr_rom region at 0x01000000) the
   * origin is a fake address that exists only for FULL()'s bookkeeping
   * below. Filled by header.hpp's NES2_HEADER() macro (see nes2.cpp). */
  nes2_header         : ORIGIN = 0x02000000, LENGTH = 16
}

REGION_ALIAS("c_readonly", prg_rom_switchable)

SECTIONS {
  .prg_rom_fixed : { *(.prg_rom_fixed .prg_rom_fixed.*) } >prg_rom_fixed
  .nes2_header   : { KEEP(*(.nes2_header)) } >nes2_header
}

/* _nes2_header (see header.hpp's NES2_HEADER()) is otherwise never referenced
 * by any code -- nothing in the executable calls into nes2.cpp -- so without
 * this the linker would never pull that object out of libplatform-nes.a and
 * .nes2_header above would stay empty (silently zero-padded by FULL(), in
 * the consuming project's own OUTPUT_FORMAT block). Naming the symbol here
 * forces extraction, exactly like the .vector table's direct
 * SHORT(nmi)/SHORT(_reset)/SHORT(irq) references do for those objects. */
EXTERN(_nes2_header)

INCLUDE rompoke-table.ld
INCLUDE dpcm.ld
INCLUDE debug-log.ld

/* Falls back to _start when nothing provides a real _reset (e.g. CMake's
 * own internal compiler-identification/ABI probes, which link a bare
 * `int main(){}` scratch file with no vrc1.cpp in sight) -- PROVIDE only
 * takes effect when the symbol is otherwise undefined, so vrc1.cpp's real
 * _reset always wins in an actual project build. Without this, any build
 * that doesn't happen to link vrc1.cpp fails to link at all, even for an
 * unrelated trivial test program -- this bit real, well before it ever got
 * near application code. */
PROVIDE(_reset = _start);

SECTIONS {
  .vector 0xfffa : { SHORT(nmi) SHORT(_reset) SHORT(irq) } >prg_rom_fixed
}

/* Only the two regions this file itself owns -- prg_rom_fixed, chr_rom, and
 * anything else a consuming project adds (its own domains, plus
 * prg_rom_fixed again if it needs FULL()'d after its own content) belong in
 * THAT project's own OUTPUT_FORMAT block, not here. Confirmed empirically
 * (not assumed) that OUTPUT_FORMAT, SECTIONS, and MEMORY are all additive
 * across multiple occurrences/multiple INCLUDEd files in this toolchain --
 * built a minimal two-region test script split exactly this way and
 * verified via hexdump that content from both halves landed correctly. */
OUTPUT_FORMAT {
  FULL(nes2_header)
  FULL(prg_rom_switchable)
}
