#include <pnes/mappers/mmc3.hpp>
#include <pnes/video.hpp>

mmc3::Register<6> mmc3::window1Control;
mmc3::Register<7> mmc3::window2Control;

mmc3::Register<0> mmc3::chr0Control;
mmc3::Register<1> mmc3::chr1Control;
mmc3::Register<2> mmc3::chr2Control;
mmc3::Register<3> mmc3::chr3Control;
mmc3::Register<4> mmc3::chr4Control;
mmc3::Register<5> mmc3::chr5Control;

tech::wo_register<0xa001> mmc3::prgRamProtect;
tech::wo_register<0xc000> mmc3::irqLatch;

void mmc3::SetMirroring(const bool horizontal) {
    tech::poke(0xa000, horizontal ? 1 : 0);
}

// @p position is off-NES only (see this function's own doc comment,
// mmc3.hpp) -- real hardware's scanline counter has no pixel-column concept
// for it to mean, so it's simply unused here.
void mmc3::ScheduleScanlineIRQ(const u8 scanline, vec2<u16>) {
    irqLatch = scanline; // $C000: reload value.
    tech::poke(0xc001, 0);     // $C001: force reload at next clock (value ignored).
    tech::poke(0xe001, 0);     // $E001: enable (value ignored).
}

void mmc3::AcknowledgeScanlineIRQ() {
    tech::poke(0xe000, 0); // $E000: disable + acknowledge pending IRQ (value ignored).
}

#if ALTERNATIVE_NAMETABLE == 1
/**
 * @brief Strong ::ppu::Flush override -- see that function's own doc comment
 * (src/nes/video.cpp) for the weak/strong relationship. A four-screen board
 * has 4 KiB of distinct physical nametable/attribute storage across
 * $2000-$2FFF, none of it mirrored, so all 4 pages need writing where the
 * weak default's 2-page loop relies on mirroring to cover the other two.
 */
void ppu::Flush(const u8 nt, const u8 at) {
    tech::peek(raw::PPUSTATUS);
    tech::poke(raw::PPUADDR, 0x20);
    tech::poke(raw::PPUADDR, 0x00);

    for (auto page = 0; page < 4; page++) {
        for (auto nt_hunk = 0; nt_hunk < 0xf0; nt_hunk++) {
            tech::poke(raw::PPUDATA, nt);
            tech::poke(raw::PPUDATA, nt);
            tech::poke(raw::PPUDATA, nt);
            tech::poke(raw::PPUDATA, nt);
        }
        for (u8 at_hunk = 0; at_hunk < 0x10; at_hunk++) {
            tech::poke(raw::PPUDATA, at);
            tech::poke(raw::PPUDATA, at);
            tech::poke(raw::PPUDATA, at);
            tech::poke(raw::PPUDATA, at);
        }
    }
}
#endif

extern "C" void _start();

/**
 * @brief The two physical PRG banks that make $8000-$BFFF continuous with
 *        the rest of the always-resident image, defined by mmc3-helper.ld.
 *
 * NOT 0 and 1 in general: mmc3-helper.ld anchors the resident image to the
 * TOP of the ROM file (banks N-1-windows .. N-1) so that MMC3's two
 * hardwired windows -- last bank at $E000, second-to-last at $C000 -- always
 * show linked content no matter how big PRG-ROM is. Anything below that is
 * spare/banked space. 0/1 is simply what those two work out to at the 4-bank
 * minimum, which is the only size this project built while these were
 * hardcoded literals here.
 *
 * Declared as arrays and used via their ADDRESS: a linker-script symbol has
 * no storage to load a value out of -- `__mmc3_boot_bank_window1 = 60;` in
 * the script means the symbol's ADDRESS is 60 -- so taking the address and
 * truncating to u8 is what actually reads the number. Same trick llvm-mos's
 * own platform code uses for __prg_rom_size-style layout symbols; a plain
 * `extern u8` here would instead fetch whatever byte lives at address 60.
 */
extern "C" const char __mmc3_boot_bank_window1[];
extern "C" const char __mmc3_boot_bank_window2[];

/// @see __mmc3_boot_bank_window1 -- address-as-value, narrowed to the 8 bits
/// MMC3's bank registers take (only the low 6 are decoded by the chip; the
/// linker script's own ASSERT on __prg_rom_size <= 512 is what keeps the
/// number inside them). always_inline, not merely `static`: ::_reset calls
/// this before R6/R7 are established, so it must not become a real call into
/// a switchable bank -- and it is not marked ::FIXED, since inlining leaves
/// nothing to place. __UINTPTR_TYPE__ rather than <cstdint>'s uintptr_t: this
/// file includes no standard headers, and the builtin is always available.
AI static inline u8 BootBank(const char *symbol) {
    return static_cast<u8>(reinterpret_cast<__UINTPTR_TYPE__>(symbol));
}

/**
 * @brief Cold-boot init: silence the scanline IRQ unit, point R6/R7 at the
 *        flat image's first two banks, enable PRG-RAM, then fall through
 *        to crt0's _start.
 *
 * Runs BEFORE .bss is zeroed: raw tech::poke() only, no Register shadow
 * objects, and no calls to non-::FIXED functions -- the windows aren't
 * established yet, so anything unpinned could resolve into whatever bank
 * powered up mapped. Real hardware powers up with R0-R7, mirroring, PRG-RAM
 * and the IRQ unit all undefined, so nothing here can be skipped as probably
 * already correct.
 *
 * THE $E000 WRITE IS SAFETY-CRITICAL, not tidiness. If the IRQ-enable latch
 * powers up (or survives a soft reset) already set, the scanline counter can
 * fire long before any application code arms it -- A12 clocks on ordinary
 * $2006 writes, and early boot is full of those. The CPU's interrupt-disable
 * flag holds the request pending rather than servicing it, so it surfaces
 * later, the moment interrupts are enabled: a crash that looks intermittent
 * because it tracks residual state rather than anything in the source.
 * Writing $E000 first, unconditionally, disables generation and acknowledges
 * whatever was pending. $C000/$C001 need no pre-seed, since
 * ::ScheduleScanlineIRQ writes both before re-enabling via $E001.
 *
 * R0-R5 and $A000 are deliberately NOT touched: CHR banks and mirroring are
 * per-game choices, not mapper bootstrap facts, and getting them wrong is
 * cosmetic rather than fatal. A project sets them in its own RESET body.
 *
 * With no MMC3_BANKED() domain in use this reconstructs one flat $8000-$FFFF
 * image from the ROM's top four banks, at any size from 32 KiB to MMC3's
 * 512 KiB maximum: R6/R7 take the image's first two banks
 * (__mmc3_boot_bank_window1/2), and the remaining two appear at
 * $C000-$FFFF automatically in PRG mode 0. That is size-independent only
 * because the linker script anchors the image to the top of the ROM file.
 */
extern "C" FIXED void _reset() {
    tech::poke(0xe000, 0);    // IRQ: disable generation + acknowledge any pending/residual IRQ.
    tech::poke(0xa001, 0x80); // PRG-RAM: enable, write-permitted.
    // R6/R7 -> the resident image's own first two banks ($8000-$9FFF,
    // $A000-$BFFF). See ::__mmc3_boot_bank_window1: these are the ROM's top
    // banks, not 0/1, on anything larger than the 32 KiB minimum.
    tech::poke(0x8000, 6); tech::poke(0x8001, BootBank(__mmc3_boot_bank_window1));
    tech::poke(0x8000, 7); tech::poke(0x8001, BootBank(__mmc3_boot_bank_window2));
    _start();
}

/**
 * @brief Re-syncs window1Control/window2Control's RAM shadows to match
 *        what ::_reset already poked into hardware.
 *
 * Same reasoning and same ordering constraint as VRC1's SyncBankShadows:
 * runs as a global constructor, after crt0's .bss zeroing, which is what
 * makes writing through ::SwitchBank safe here but not in ::_reset.
 */
__attribute__((constructor(101)))
static void SyncBankShadows() {
    mmc3::SwitchBank(mmc3::window1Control, BootBank(__mmc3_boot_bank_window1));
    mmc3::SwitchBank(mmc3::window2Control, BootBank(__mmc3_boot_bank_window2));
}
