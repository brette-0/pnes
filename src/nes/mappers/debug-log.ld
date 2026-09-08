/*
 * debug-log.ld -- LIBRARY-OWNED, mapper-independent.
 *
 * log()'s call sites (see include/platform-nes/logger.hpp) emit no
 * instructions at all -- each one is a `static const` Entry declaration
 * tagged __attribute__((section(".pnes_log"))), carrying __FILE__,
 * __LINE__, and the message string as compiler-generated data.
 *
 * This rule is the other half of that: .pnes_log gets an output section
 * with no address and no MEMORY region, so it survives the link (present
 * in the linked .elf for the host-side `logger` tool to read back out) but
 * contributes zero bytes to the final ROM image -- objcopy only copies
 * sections that were placed into a region, and this one never is.
 *
 * Without an explicit rule here, GNU/LLVM linkers place an "orphan"
 * section (one no SECTIONS rule mentions) into the nearest matching region
 * anyway, which would silently leak this data into the ROM -- hence this
 * file existing at all rather than just staying quiet about .pnes_log.
 *
 * INCLUDEd by every mapper helper (mmc3-helper.ld, vrc1-helper.ld, ...) so
 * consuming projects get it automatically via their existing
 * `INCLUDE mmc3-helper.ld` / `INCLUDE vrc1-helper.ld` -- nothing for a
 * project's own top-level script to configure.
 *
 * KEEP() IS LOAD-BEARING, confirmed the hard way: each log() call site's
 * `used` attribute (logger.hpp) only stops the COMPILER's own dead-code
 * elimination from dropping the declaration during compilation/LTO -- it
 * does not by itself survive the linker's whole-program --gc-sections pass,
 * which nothing here ever references (that's the entire point: nothing at
 * runtime may reference log data). Without KEEP() here, a real link (LTO,
 * this project's default) silently drops the whole section -- verified by
 * building demo/src/main.cpp's own log("logger working") call, which
 * produced real .pnes_log symbols in the pre-LTO bitcode (llvm-nm) but no
 * .pnes_log section at all in the final linked .elf until this was added.
 *
 * A DEDICATED MEMORY REGION IS ALSO LOAD-BEARING, confirmed the same way:
 * giving the section a bare address (`.pnes_log 0 : ...`) with no region
 * both left LLD unable to place it ("no memory region specified") and, once
 * KEEP() actually started retaining real content, collided with the real
 * .zp.data/.zp.bss regions living at that same low address range. This
 * project already has a convention for exactly this -- a fake, ELF-only
 * address namespace no real hardware region will ever use -- see
 * mmc3-helper.ld's chr_rom (0x01000000), nes2_header (0x02000000), and
 * prg_rom_spare (0x03000000). 0x04000000 continues that sequence.
 */

MEMORY {
  pnes_log : ORIGIN = 0x04000000, LENGTH = 0x01000000
}

SECTIONS {
  .pnes_log : { KEEP(*(.pnes_log .pnes_log.*)) } >pnes_log
}
