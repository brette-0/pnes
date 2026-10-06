/*
 * Demo's own top-level linker script -- CONSUMER-OWNED, not library code.
 *
 * Converted from VRC1 to MMC3: INCLUDEs mmc3-helper.ld instead of
 * vrc1-helper.ld. The old VRC1-only smoke-test domains are gone (vrc1.cpp
 * no longer links).
 *
 * FamiStudio has no dedicated bank any more: this demo has no frequent
 * cross-bank calling, so its compiled code just lives as ordinary
 * always-resident content in prg_rom_switchable -- audio.cpp calls its
 * entry points directly, no farcall wrapper needed.
 */

/* This demo's own ROM size -- a project choice, not an MMC3 fact. 256 KiB
 * (32 banks), doubled from 128 KiB for headroom for new banked domains (see
 * the named prg_rom_empty_N placeholder banks below). Any power of 2 from 32
 * up to 512 KiB (MMC3's hardware max) works with no other edit --
 * mmc3-helper.ld anchors the resident image to the TOP of the file and pads
 * the rest into prg_rom_spare.
 *
 * TRACK THIS BY HAND: demo/src/header.hpp's HEADER() prg_rom_bytes argument
 * must match (the NES2.0 header can't see linker symbols). Growing this does
 * NOT grow the flat resident image ($8000-$DFFF) -- new banks are only
 * reachable through MMC3_BANKED() domains. */
PROVIDE(__prg_rom_size = 256);
PROVIDE(__chr_rom_size = 8);

/* Eight real banked domains, claiming banks that would otherwise be
 * prg_rom_spare: level code (0), level data (1), audio (2), audio_data (3),
 * cold (4), actors (5), level's dynamic-plane source (6), level graphics (7)
 * -- see banks.hpp. Plus 22 placeholder banks (8-29), named .prg_rom_empty_0
 * through .prg_rom_empty_21 below, carved out of what would otherwise be
 * anonymous prg_rom_spare padding so the doubled ROM's extra space is
 * individually addressable instead of one unstructured blob -- a bank can be
 * converted into a real domain later by renaming its section, no layout
 * rework needed. MUST be set before `INCLUDE mmc3-helper.ld`: PROVIDE() only
 * takes its first value. */
PROVIDE(__mmc3_reserved_banks = 30);

/* ONE resident window ($C000-$DFFF), not the default three. Both switchable
 * windows go to the level domain below (16 KiB contiguous, since no farcall
 * can enter a domain wider than one window).
 *
 * The risk: while level's banks are mapped, NOTHING at $8000-$BFFF exists
 * except level. Everything else must fit in $C000-$DFFF or the fixed bank --
 * it does (~2.9 KiB measured), but nothing enforces it. */
PROVIDE(__mmc3_resident_windows = 1);

/* Level's code has no special placement rule any more, so it needs none of
 * the old "declare before INCLUDE" wildcard-ordering trick. ::LEVEL_CODE/
 * ::LEVEL_DATA/::ACTORS/::COLD/::FIXED all route through
 * CREATE_SEGMENT_KEYWORD, giving each piece its own uniquely-named section
 * that no generic wildcard can match -- so those rules live in the ordinary
 * post-INCLUDE block below. Anything untagged still falls through to the
 * resident catch-all as before. */

INCLUDE mmc3-helper.ld

/* ------------------------------------------------------------------------
 * BANKED DOMAINS -- banks 0-6, 8 KiB each. Most reach through window 1
 * ($8000-$9FFF, R6); audio_data and level_dynamic are pinned to window 2
 * ($A000-$BFFF, R7) because each must be mappable at the same time as a
 * specific window-1 partner -- see their own comments below.
 *
 * ORIGIN is ((bank + 1) * 0x10000) + (0x8000 or 0xA000) -- see mmc3.hpp's
 * section_t ENCODING comment for why the +1 bias exists. The high bits never
 * reach emitted code (a JSR operand is 16 bits), so every one of these
 * truncates back to a plain $8000/$A000 call.
 *
 * A domain's REAL bank number is its file offset / 0x2000, decided by the
 * FULL() order in OUTPUT_FORMAT at the bottom of this file -- NOT by these
 * ORIGINs or the numbers in their names. The two are kept in agreement by
 * hand (level_code is FULL()'d first, so it's bank 0, so its ORIGIN encodes
 * 0+1). Change one, change all three: FULL() order, ORIGIN here, and
 * section() in demo/src/banks.hpp.
 * ------------------------------------------------------------------------ */
MEMORY {
  /* LEVEL CODE: bank 0, window 1. Level's own code -- reads the static plane
   * and dynamic-plane ROM source directly out of window 2, so it stays
   * mapped for the whole level session. See banks.hpp's ::level_code_tag. */
  prg_rom_level_code : ORIGIN = 0x00018000, LENGTH = 0x2000  /* bank 0, window 1 */
  /* LEVEL DATA: bank 1, window 2. TileData_1_1 + DynLengths_1_1. Not paired
   * with prg_rom_level_code by a compile-time tag -- level::LoadLevel switches
   * window 2 here with a runtime bank number (banks.hpp's ::LevelDataBank)
   * and holds it for the whole level session. */
  prg_rom_level_data : ORIGIN = 0x0002a000, LENGTH = 0x2000  /* bank 1, window 2 */
  /* AUDIO CODE: FamiStudio's engine + platform-nes's audio module. Adjacency
   * is a hard contract -- audio.cpp calls the engine with no bank switching,
   * valid only because they share a bank. */
  prg_rom_audio : ORIGIN = 0x00038000, LENGTH = 0x2000  /* bank 2, window 1 */
  /* AUDIO DATA: songs and SFX, separate so they aren't capped by bank 2's
   * leftover space. WINDOW 2 is forced: the engine walks this data while
   * executing, so both must be mapped at once (mmc3::CallPairedBlock
   * asserts it). */
  prg_rom_audio_data : ORIGIN = 0x0004a000, LENGTH = 0x2000  /* bank 3, window 2 */
  /* COLD: bank 4, window 1. Code that runs once (level's one-time setup),
   * reached via mmc3::CallInBlock<cold_tag>. See ::cold_tag. */
  prg_rom_cold : ORIGIN = 0x00058000, LENGTH = 0x2000  /* bank 4, window 1 */
  /* ACTORS: bank 5, window 1. Actor's methods + Player's, reached every
   * frame via mmc3::Call<level::UpdateActors> so the whole roster updates
   * behind one switch. See ::actor_tag. */
  prg_rom_actors : ORIGIN = 0x00068000, LENGTH = 0x2000  /* bank 5, window 1 */
  /* LEVEL DYNAMIC-SOURCE: bank 6, window 2. DynData_1_1, read once at load via
   * a raw mmc3::SwitchBank(window2Control, LevelDynamicBank(n)) in LoadLevel,
   * not permanently pinned like prg_rom_level_data. See ::LevelDynamicBank. */
  prg_rom_level_dynamic : ORIGIN = 0x0007a000, LENGTH = 0x2000  /* bank 6, window 2 */
  /* LEVEL GRAPHICS: bank 7, window 2. Metatile planes, level palette data,
   * and the level's string/charmap data -- reached via
   * mmc3::CallInBlock<level_graphics_tag>, switched in and back out around
   * the reads that need it. See banks.hpp's ::level_graphics_tag. */
  prg_rom_level_graphics : ORIGIN = 0x0008a000, LENGTH = 0x2000  /* bank 7, window 2 */

  /* OPTIONS MENU: bank 8, window 1. uitk-generated options-scene code + data
   * (demo/ui/options.uis: linker_prefix "OPTIONS", data_prefix
   * "OPTIONS_DATA") -- small enough to share one bank, same "distinct
   * section NAME, same physical bank" pattern as ::COLD/::TITLE (banks.hpp),
   * so OPTIONS_DATA's literal section name just needs the ".prg_rom_options"
   * prefix to fall under this rule's existing *.* wildcard below. Carved out
   * of what was prg_rom_empty_0 -- see __mmc3_reserved_banks's comment for
   * why these placeholder banks exist named rather than as anonymous
   * prg_rom_spare. */
  prg_rom_options : ORIGIN = 0x00098000, LENGTH = 0x2000  /* bank 8, window 1 */

  /* PLACEHOLDER BANKS: 9-29, unused for now. Named individually (instead of
   * left as anonymous prg_rom_spare) so each is addressable on its own and
   * visible in the space report -- see the comment above
   * __mmc3_reserved_banks for why. */
  prg_rom_empty_1 : ORIGIN = 0x000a8000, LENGTH = 0x2000  /* bank 9, window 1 */
  prg_rom_empty_2 : ORIGIN = 0x000b8000, LENGTH = 0x2000  /* bank 10, window 1 */
  prg_rom_empty_3 : ORIGIN = 0x000c8000, LENGTH = 0x2000  /* bank 11, window 1 */
  prg_rom_empty_4 : ORIGIN = 0x000d8000, LENGTH = 0x2000  /* bank 12, window 1 */
  prg_rom_empty_5 : ORIGIN = 0x000e8000, LENGTH = 0x2000  /* bank 13, window 1 */
  prg_rom_empty_6 : ORIGIN = 0x000f8000, LENGTH = 0x2000  /* bank 14, window 1 */
  prg_rom_empty_7 : ORIGIN = 0x00108000, LENGTH = 0x2000  /* bank 15, window 1 */
  prg_rom_empty_8 : ORIGIN = 0x00118000, LENGTH = 0x2000  /* bank 16, window 1 */
  prg_rom_empty_9 : ORIGIN = 0x00128000, LENGTH = 0x2000  /* bank 17, window 1 */
  prg_rom_empty_10 : ORIGIN = 0x00138000, LENGTH = 0x2000  /* bank 18, window 1 */
  prg_rom_empty_11 : ORIGIN = 0x00148000, LENGTH = 0x2000  /* bank 19, window 1 */
  prg_rom_empty_12 : ORIGIN = 0x00158000, LENGTH = 0x2000  /* bank 20, window 1 */
  prg_rom_empty_13 : ORIGIN = 0x00168000, LENGTH = 0x2000  /* bank 21, window 1 */
  prg_rom_empty_14 : ORIGIN = 0x00178000, LENGTH = 0x2000  /* bank 22, window 1 */
  prg_rom_empty_15 : ORIGIN = 0x00188000, LENGTH = 0x2000  /* bank 23, window 1 */
  prg_rom_empty_16 : ORIGIN = 0x00198000, LENGTH = 0x2000  /* bank 24, window 1 */
  prg_rom_empty_17 : ORIGIN = 0x001a8000, LENGTH = 0x2000  /* bank 25, window 1 */
  prg_rom_empty_18 : ORIGIN = 0x001b8000, LENGTH = 0x2000  /* bank 26, window 1 */
  prg_rom_empty_19 : ORIGIN = 0x001c8000, LENGTH = 0x2000  /* bank 27, window 1 */
  prg_rom_empty_20 : ORIGIN = 0x001d8000, LENGTH = 0x2000  /* bank 28, window 1 */
  prg_rom_empty_21 : ORIGIN = 0x001e8000, LENGTH = 0x2000  /* bank 29, window 1 */
}

/* Published for the build-time space report -- see mmc3-helper.ld's own
 * __space_* block for why an empty bank needs these to be visible at all. */
__space_origin_prg_rom_level_code = ORIGIN(prg_rom_level_code);
__space_length_prg_rom_level_code = LENGTH(prg_rom_level_code);
__space_origin_prg_rom_level_data = ORIGIN(prg_rom_level_data);
__space_length_prg_rom_level_data = LENGTH(prg_rom_level_data);
__space_origin_prg_rom_audio = ORIGIN(prg_rom_audio);
__space_length_prg_rom_audio = LENGTH(prg_rom_audio);
__space_origin_prg_rom_audio_data = ORIGIN(prg_rom_audio_data);
__space_length_prg_rom_audio_data = LENGTH(prg_rom_audio_data);
__space_origin_prg_rom_cold = ORIGIN(prg_rom_cold);
__space_length_prg_rom_cold = LENGTH(prg_rom_cold);
__space_origin_prg_rom_actors = ORIGIN(prg_rom_actors);
__space_length_prg_rom_actors = LENGTH(prg_rom_actors);
__space_origin_prg_rom_level_dynamic = ORIGIN(prg_rom_level_dynamic);
__space_length_prg_rom_level_dynamic = LENGTH(prg_rom_level_dynamic);
__space_origin_prg_rom_level_graphics = ORIGIN(prg_rom_level_graphics);
__space_length_prg_rom_level_graphics = LENGTH(prg_rom_level_graphics);
__space_origin_prg_rom_options = ORIGIN(prg_rom_options);
__space_length_prg_rom_options = LENGTH(prg_rom_options);
__space_origin_prg_rom_empty_1 = ORIGIN(prg_rom_empty_1);
__space_length_prg_rom_empty_1 = LENGTH(prg_rom_empty_1);
__space_origin_prg_rom_empty_2 = ORIGIN(prg_rom_empty_2);
__space_length_prg_rom_empty_2 = LENGTH(prg_rom_empty_2);
__space_origin_prg_rom_empty_3 = ORIGIN(prg_rom_empty_3);
__space_length_prg_rom_empty_3 = LENGTH(prg_rom_empty_3);
__space_origin_prg_rom_empty_4 = ORIGIN(prg_rom_empty_4);
__space_length_prg_rom_empty_4 = LENGTH(prg_rom_empty_4);
__space_origin_prg_rom_empty_5 = ORIGIN(prg_rom_empty_5);
__space_length_prg_rom_empty_5 = LENGTH(prg_rom_empty_5);
__space_origin_prg_rom_empty_6 = ORIGIN(prg_rom_empty_6);
__space_length_prg_rom_empty_6 = LENGTH(prg_rom_empty_6);
__space_origin_prg_rom_empty_7 = ORIGIN(prg_rom_empty_7);
__space_length_prg_rom_empty_7 = LENGTH(prg_rom_empty_7);
__space_origin_prg_rom_empty_8 = ORIGIN(prg_rom_empty_8);
__space_length_prg_rom_empty_8 = LENGTH(prg_rom_empty_8);
__space_origin_prg_rom_empty_9 = ORIGIN(prg_rom_empty_9);
__space_length_prg_rom_empty_9 = LENGTH(prg_rom_empty_9);
__space_origin_prg_rom_empty_10 = ORIGIN(prg_rom_empty_10);
__space_length_prg_rom_empty_10 = LENGTH(prg_rom_empty_10);
__space_origin_prg_rom_empty_11 = ORIGIN(prg_rom_empty_11);
__space_length_prg_rom_empty_11 = LENGTH(prg_rom_empty_11);
__space_origin_prg_rom_empty_12 = ORIGIN(prg_rom_empty_12);
__space_length_prg_rom_empty_12 = LENGTH(prg_rom_empty_12);
__space_origin_prg_rom_empty_13 = ORIGIN(prg_rom_empty_13);
__space_length_prg_rom_empty_13 = LENGTH(prg_rom_empty_13);
__space_origin_prg_rom_empty_14 = ORIGIN(prg_rom_empty_14);
__space_length_prg_rom_empty_14 = LENGTH(prg_rom_empty_14);
__space_origin_prg_rom_empty_15 = ORIGIN(prg_rom_empty_15);
__space_length_prg_rom_empty_15 = LENGTH(prg_rom_empty_15);
__space_origin_prg_rom_empty_16 = ORIGIN(prg_rom_empty_16);
__space_length_prg_rom_empty_16 = LENGTH(prg_rom_empty_16);
__space_origin_prg_rom_empty_17 = ORIGIN(prg_rom_empty_17);
__space_length_prg_rom_empty_17 = LENGTH(prg_rom_empty_17);
__space_origin_prg_rom_empty_18 = ORIGIN(prg_rom_empty_18);
__space_length_prg_rom_empty_18 = LENGTH(prg_rom_empty_18);
__space_origin_prg_rom_empty_19 = ORIGIN(prg_rom_empty_19);
__space_length_prg_rom_empty_19 = LENGTH(prg_rom_empty_19);
__space_origin_prg_rom_empty_20 = ORIGIN(prg_rom_empty_20);
__space_length_prg_rom_empty_20 = LENGTH(prg_rom_empty_20);
__space_origin_prg_rom_empty_21 = ORIGIN(prg_rom_empty_21);
__space_length_prg_rom_empty_21 = LENGTH(prg_rom_empty_21);

SECTIONS {
  /* Level's interrupt path needs no rule at all: nmi_handler, irq_handler and
   * ApplyHudSplit are tagged ::FIXED in level.cpp, so they land in
   * .prg_rom_fixed and mmc3-helper.ld's existing rule takes them. */
  .prg_rom_level_code : { *(.prg_rom_level_code .prg_rom_level_code.*) } >prg_rom_level_code
  .prg_rom_level_data : { *(.prg_rom_level_data .prg_rom_level_data.*) } >prg_rom_level_data
  .prg_rom_audio : { *(.prg_rom_audio .prg_rom_audio.*) } >prg_rom_audio
  .prg_rom_audio_data : {
    *(.prg_rom_music .prg_rom_music.*)
    *(.prg_rom_sfx .prg_rom_sfx.*)
  } >prg_rom_audio_data
  .prg_rom_cold : { *(.prg_rom_cold .prg_rom_cold.*) } >prg_rom_cold
  .prg_rom_actors : { *(.prg_rom_actors .prg_rom_actors.*) } >prg_rom_actors
  .prg_rom_level_dynamic : { *(.prg_rom_level_dynamic .prg_rom_level_dynamic.*) } >prg_rom_level_dynamic
  .prg_rom_level_graphics : { *(.prg_rom_level_graphics .prg_rom_level_graphics.*) } >prg_rom_level_graphics

  /* .* wildcard covers OPTIONS_DATA too, same as LEVEL_CODE/COLD/ACTORS'
   * own rules -- see this bank's MEMORY comment above. */
  .prg_rom_options : { *(.prg_rom_options .prg_rom_options.*) } >prg_rom_options
  .prg_rom_empty_1 : { *(.prg_rom_empty_1 .prg_rom_empty_1.*) } >prg_rom_empty_1
  .prg_rom_empty_2 : { *(.prg_rom_empty_2 .prg_rom_empty_2.*) } >prg_rom_empty_2
  .prg_rom_empty_3 : { *(.prg_rom_empty_3 .prg_rom_empty_3.*) } >prg_rom_empty_3
  .prg_rom_empty_4 : { *(.prg_rom_empty_4 .prg_rom_empty_4.*) } >prg_rom_empty_4
  .prg_rom_empty_5 : { *(.prg_rom_empty_5 .prg_rom_empty_5.*) } >prg_rom_empty_5
  .prg_rom_empty_6 : { *(.prg_rom_empty_6 .prg_rom_empty_6.*) } >prg_rom_empty_6
  .prg_rom_empty_7 : { *(.prg_rom_empty_7 .prg_rom_empty_7.*) } >prg_rom_empty_7
  .prg_rom_empty_8 : { *(.prg_rom_empty_8 .prg_rom_empty_8.*) } >prg_rom_empty_8
  .prg_rom_empty_9 : { *(.prg_rom_empty_9 .prg_rom_empty_9.*) } >prg_rom_empty_9
  .prg_rom_empty_10 : { *(.prg_rom_empty_10 .prg_rom_empty_10.*) } >prg_rom_empty_10
  .prg_rom_empty_11 : { *(.prg_rom_empty_11 .prg_rom_empty_11.*) } >prg_rom_empty_11
  .prg_rom_empty_12 : { *(.prg_rom_empty_12 .prg_rom_empty_12.*) } >prg_rom_empty_12
  .prg_rom_empty_13 : { *(.prg_rom_empty_13 .prg_rom_empty_13.*) } >prg_rom_empty_13
  .prg_rom_empty_14 : { *(.prg_rom_empty_14 .prg_rom_empty_14.*) } >prg_rom_empty_14
  .prg_rom_empty_15 : { *(.prg_rom_empty_15 .prg_rom_empty_15.*) } >prg_rom_empty_15
  .prg_rom_empty_16 : { *(.prg_rom_empty_16 .prg_rom_empty_16.*) } >prg_rom_empty_16
  .prg_rom_empty_17 : { *(.prg_rom_empty_17 .prg_rom_empty_17.*) } >prg_rom_empty_17
  .prg_rom_empty_18 : { *(.prg_rom_empty_18 .prg_rom_empty_18.*) } >prg_rom_empty_18
  .prg_rom_empty_19 : { *(.prg_rom_empty_19 .prg_rom_empty_19.*) } >prg_rom_empty_19
  .prg_rom_empty_20 : { *(.prg_rom_empty_20 .prg_rom_empty_20.*) } >prg_rom_empty_20
  .prg_rom_empty_21 : { *(.prg_rom_empty_21 .prg_rom_empty_21.*) } >prg_rom_empty_21
}

/* Moves the C writable arena (.data/.bss/heap/soft stack) into PRG-RAM
 * instead of system RAM, freeing the console's 1.5 KiB onboard RAM for OAM
 * buffers and hot resident state.
 *
 * MUST come AFTER `INCLUDE mmc3-helper.ld`: prg_ram is declared in that
 * file's MEMORY block.
 *
 * __stack sits on the top BYTE of prg_ram (not one past the end), same
 * convention as the SDK's nes/lib/c-in-prg-ram-fixed.ld. */
REGION_ALIAS("c_writeable", prg_ram)
__stack = ORIGIN(prg_ram) + LENGTH(prg_ram) - 1;

/* Heap: a hard-reserved 128 bytes, carved out of the TOP of prg_ram and
 * walled off from both .data/.bss and the softstack -- not the SDK default
 * of "however many bytes happen to be free between wherever .bss/.noinit
 * end and wherever the stack has grown to".
 *
 * noinit.ld (pulled in via INCLUDE mmc3-helper.ld above) already set
 * __heap_start to that default (right after .data/.bss/.noinit, growing up
 * toward __stack) -- __static_writeable_end captures that SDK-computed
 * value BEFORE the override below discards it, so the ASSERT still catches
 * .data/.bss/.noinit (including any future __attribute__((section(".noinit")))
 * globals) growing far enough to collide with the reserved heap.
 *
 * __heap_start is moved to the last 128 bytes of prg_ram, __heap_default_limit
 * is capped at exactly that (a weak SDK default of 4096 would otherwise let
 * the allocator walk past the reservation with no error), and __stack is
 * pulled down to sit directly below it. Both boundaries are now fixed
 * addresses baked into the layout, not "however big something else happened
 * to be" -- .data/.bss can't grow into the heap (ASSERT below catches it at
 * link time) and the stack can't grow into it either (it starts one byte
 * below __heap_start, same top-of-region convention as __stack itself). */
__static_writeable_end = __heap_start;
__heap_default_limit = 128;
__heap_start = ORIGIN(prg_ram) + LENGTH(prg_ram) - 128;
__stack = __heap_start - 1;
ASSERT(__static_writeable_end <= __heap_start,
       "prg_ram's .data/.bss/.noinit grew into the reserved 128-byte heap at the top of prg_ram -- shrink static writeable usage or grow prg_ram/__prg_ram_size.")

/* Regression guard: the audio engine's code really did land in its bank. A
 * typo in FAMISTUDIO_CA65_CODE_SEGMENT (demo/famistudio_config.s) is
 * otherwise silent -- the segment lands elsewhere and audio.cpp's farcalls
 * bank in an empty 8 KiB.
 *
 * Zero IS tolerated via DEFINED(nmi_handler): CMake's compiler-ID/ABI probes
 * link a bare `int main(){}` with no audio in sight. */
PROVIDE(__audio_engine_size = SIZEOF(.prg_rom_audio));
ASSERT(__audio_engine_size > 0 || !DEFINED(nmi_handler),
       "The audio bank is empty -- demo/famistudio_config.s's FAMISTUDIO_CA65_CODE_SEGMENT no longer decodes to .prg_rom_audio (ld.lld XO65 escapes: _p is '.', __ is '_'), so audio::Update() would call an engine that is not in the ROM.")

/* Zero page is opt-in only: CMakeLists.txt sets -mlto-zp=0 so mos-zero-page-alloc
 * promotes nothing on its own, leaving $20-$FF free for `direct`-tagged globals
 * (technology.hpp) and FamiStudio's own ZEROPAGE segment (famistudio_config.s).
 * That contract depends on llvm-mos's 32-byte imaginary-register block
 * (__rc0-__rc31) staying exactly $00-$1F -- nes-no-pow2.ld already computes the
 * `zp` region as `__rc31 + 1` rather than hardcoding $20 for this reason, and
 * carries its own ASSERT(__rc31 == 0x1f, ...). That file is a tracked, unmodified
 * fork of the SDK's nes.ld (see its own header comment), so this second,
 * project-local ASSERT pins the same fact here instead of hand-editing it --
 * ORIGIN(zp) MUST be $20 for code that assumes `direct` variables start there. */
ASSERT(ORIGIN(zp) == 0x20,
       "Zero page's free region no longer starts at $20 -- llvm-mos's imaginary-register block (__rc0-__rc31) grew or shrank. `direct`-tagged globals and FamiStudio's ZEROPAGE segment assume ORIGIN(zp) == 0x20; update anything that hardcodes that address.")

/* mmc3-helper.ld FULL()s only nes2_header; every PRG region is this
 * project's job, in this exact order -- OUTPUT_FORMAT is additive across
 * INCLUDEd files, and this block's position (AFTER mmc3-helper.ld's own)
 * controls final file order.
 *
 * Order is the whole layout contract:
 * - Banked domains come FIRST, and THIS ORDER ASSIGNS THEIR BANK NUMBERS
 *   (level_code=0, level_data=1, audio=2, audio_data=3, cold=4, actors=5,
 *   level_dynamic=6, level_graphics=7 -- bank is just file offset / 0x2000).
 *   Their MEMORY ORIGINs above and section()s in banks.hpp hard-code those
 *   same numbers; reordering these lines silently re-banks everything with
 *   no warning.
 * - prg_rom_spare is the zero padding filling out __prg_rom_size.
 * - prg_rom_switchable then prg_rom_fixed occupy the top banks: real MMC3
 *   hardware always shows the last bank at $E000, second-to-last at $C000. */
OUTPUT_FORMAT {
  FULL(prg_rom_level_code)
  FULL(prg_rom_level_data)
  FULL(prg_rom_audio)
  FULL(prg_rom_audio_data)
  FULL(prg_rom_cold)
  FULL(prg_rom_actors)
  FULL(prg_rom_level_dynamic)
  FULL(prg_rom_level_graphics)
  FULL(prg_rom_options)
  FULL(prg_rom_empty_1)
  FULL(prg_rom_empty_2)
  FULL(prg_rom_empty_3)
  FULL(prg_rom_empty_4)
  FULL(prg_rom_empty_5)
  FULL(prg_rom_empty_6)
  FULL(prg_rom_empty_7)
  FULL(prg_rom_empty_8)
  FULL(prg_rom_empty_9)
  FULL(prg_rom_empty_10)
  FULL(prg_rom_empty_11)
  FULL(prg_rom_empty_12)
  FULL(prg_rom_empty_13)
  FULL(prg_rom_empty_14)
  FULL(prg_rom_empty_15)
  FULL(prg_rom_empty_16)
  FULL(prg_rom_empty_17)
  FULL(prg_rom_empty_18)
  FULL(prg_rom_empty_19)
  FULL(prg_rom_empty_20)
  FULL(prg_rom_empty_21)
  FULL(prg_rom_spare)
  FULL(prg_rom_switchable)
  FULL(prg_rom_fixed)
  FULL(chr_rom)
}
