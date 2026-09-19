/**
 * @file mmc3.cpp
 * @brief Emu-side (off-NES) MMC3 register storage, CHR bank bookkeeping, and
 *        tile-address translation. Counterpart to src/nes/mappers/mmc3.cpp,
 *        which owns the real $8000/$8001-port-pair hardware writes and is
 *        only built for the NES target -- see mmc3.hpp's own file comment.
 *
 * window1Control/window2Control/chr0Control-chr5Control are declared
 * unconditionally in mmc3.hpp (game code calls ::mmc3::SwitchBank /
 * ::mmc3::SwitchCHRBank on them the same way on every target), so they need
 * a definition on EVERY target, not just NES -- ::Register itself already
 * branches its hardware-poke path on TARGET_NES internally.
 *
 * Real NES hardware performs CHR bank switching in silicon: the PPU's own
 * address bus is what MMC3 intercepts, so nothing in software needs to
 * translate a tile address there. Off-NES, the emu PPU (src/emu/ppu.cpp)
 * instead renders straight from one flat, non-bank-switched CHR-ROM image
 * (::patternTable) -- the rest of this file is the software stand-in for
 * what the cartridge's mapper would otherwise be doing: it tracks which
 * physical CHR bank is currently switched into each of MMC3's six windows,
 * and answers "what's the real offset for this PPU-space tile address" via
 * ::GetTileLMA. This file also supplies the strong (non-weak) definition of
 * ::ppu::ResolveTile the emu PPU calls unconditionally for every tile fetch
 * -- see that function's own doc comment (video.hpp) for why this always
 * wins over the emu PPU's own weak default with no runtime dispatch, and
 * ALTERNATIVE_NAMETABLE == 1 (four-screen)'s own comment below for the
 * matching ::ppu::nametableCount override.
 */
#include <platform-nes/mappers/mmc3.hpp>
#include <platform-nes/video.hpp>
#include <platform-nes/interrupts.hpp>

#include <cstdlib>

mmc3::Register<6> mmc3::window1Control;
mmc3::Register<7> mmc3::window2Control;

mmc3::Register<0> mmc3::chr0Control;
mmc3::Register<1> mmc3::chr1Control;
mmc3::Register<2> mmc3::chr2Control;
mmc3::Register<3> mmc3::chr3Control;
mmc3::Register<4> mmc3::chr4Control;
mmc3::Register<5> mmc3::chr5Control;

bool mmc3::shape = true;   // hardware's mode 0 (large windows at front) -- see mmc3.hpp's own comment.
u8   mmc3::banks[6] = {};

void mmc3::NotifyCHRWrite(const u8 index, const u8 bank) {
    banks[index] = bank;
    ++ppu::chrGeneration;
}

void mmc3::SetCHRMode(const bool largeWindowsAtFront) {
    shape = largeWindowsAtFront;
}

void mmc3::SetMirroring(const bool horizontal) {
    mirroring = horizontal;
}

/**
 * @brief Off-NES stand-in for the real $C000/$C001/$E001 MMC3 IRQ-arming
 * sequence: there's no scanline-counter hardware to poke, so this just arms
 * the shared single-slot ::irq::irqPending mechanism with the application's
 * fixed ::IRQ entry point (::irq_vector) at @p position -- the same handler
 * the real hardware IRQ vector would reach, only the position varies.
 * Matches ::video::WaitThenReactToSpriteZero's shape, but always targets
 * ::irq_vector rather than a caller-supplied callback, since a real
 * interrupt source has exactly one destination, chosen at compile time --
 * never a runtime-supplied function pointer.
 *
 * @p scanline plays no part in this -- see this function's own doc comment
 * (mmc3.hpp) for why it's specifically NOT reused to derive @p position.
 */
void mmc3::ScheduleScanlineIRQ(const u8, const vec2<u16> position) {
    irq::irqHandler      = irq_vector;
    irq::irqPosition     = position;
    irq::irqPendingValid = true;
}

/** @brief Off-NES stand-in for the real $E000 disable+acknowledge write. */
void mmc3::AcknowledgeScanlineIRQ() {
    irq::irqPendingValid = false;
}

/**
 * @brief Resolves @p tileVMA through the currently-selected CHR banks.
 *
 * MMC3 CHR windows, mode 0 (::shape true, hardware's own default -- see
 * mmc3.hpp's file comment: PRG mode 0 / CHR mode 0 is the only combination
 * the NES side of this module ever uses):
 *
 *   $0000-$07FF  R0 (2 KiB)   $1000-$13FF  R2 (1 KiB)
 *   $0800-$0FFF  R1 (2 KiB)   $1400-$17FF  R3 (1 KiB)
 *                             $1800-$1BFF  R4 (1 KiB)
 *                             $1C00-$1FFF  R5 (1 KiB)
 *
 * Mode 1 (::shape false) swaps the two halves: R2-R5 at $0000-$0FFF (1 KiB
 * each), R0/R1 at $1000-$1FFF (2 KiB each).
 *
 * Bank numbers are ALL in 1 KiB units, including R0/R1: real MMC3 silicon
 * counts every CHR bank register in 1 KiB units regardless of how wide the
 * window it controls is, and R0/R1 (2 KiB windows) simply ignore the bottom
 * bit of the value written to them -- the register can only ever latch an
 * even 1 KiB-granularity bank, which is what makes a 2 KiB-aligned window
 * out of it (see mmc3.hpp's own comment on ::SwitchCHRBank; confirmed
 * against nesdev.org/wiki/MMC3: "R0 and R1 ignore the bottom bit, as the
 * value written still counts banks in 1KB units but odd numbered banks
 * can't be selected"). So the physical offset is `(bank & ~1) * 1 KiB`,
 * for every register including R0/R1 -- NOT `bank * 2 KiB` -- plus @p
 * tileVMA's offset within that window.
 *
 * The result is wrapped modulo ::ppu::chrRomBytes: a bank register can be
 * (and, in practice, is) written with a value that addresses past what this
 * build actually embeds -- e.g. a board whose real CHR-ROM chip is bigger
 * than the CHR art currently linked into a desktop preview build. Real
 * hardware doesn't fault on that; the chip's own address pins are wired to
 * only as many bits as its actual capacity needs, so a too-large address
 * simply aliases back into the chip's real range. This mirrors that in
 * software, rather than indexing ::patternTable (::CHR_ROM) out of bounds.
 */
u32 mmc3::GetTileLMA(const u16 tileVMA) {
    constexpr u32 k1K = 0x400;
    const u16 vma = tileVMA & 0x1FFF;
    // R0/R1 only: the register latches an even bank number -- see this
    // function's own doc comment.
    const u8 bank0 = banks[0] & ~1u;
    const u8 bank1 = banks[1] & ~1u;
    u32 lma;

    if (shape) {
        if      (vma < 0x0800) lma = bank0 * k1K + (vma - 0x0000);
        else if (vma < 0x1000) lma = bank1 * k1K + (vma - 0x0800);
        else if (vma < 0x1400) lma = banks[2] * k1K + (vma - 0x1000);
        else if (vma < 0x1800) lma = banks[3] * k1K + (vma - 0x1400);
        else if (vma < 0x1C00) lma = banks[4] * k1K + (vma - 0x1800);
        else                   lma = banks[5] * k1K + (vma - 0x1C00);
    } else {
        if      (vma < 0x0400) lma = banks[2] * k1K + (vma - 0x0000);
        else if (vma < 0x0800) lma = banks[3] * k1K + (vma - 0x0400);
        else if (vma < 0x0C00) lma = banks[4] * k1K + (vma - 0x0800);
        else if (vma < 0x1000) lma = banks[5] * k1K + (vma - 0x0C00);
        else if (vma < 0x1800) lma = bank0 * k1K + (vma - 0x1000);
        else                   lma = bank1 * k1K + (vma - 0x1800);
    }

    return lma % ppu::chrRomBytes;
}

/**
 * @brief Strong ::ppu::ResolveTile override -- see that function's own doc
 * comment (video.hpp) for the weak/strong relationship. Applies to every
 * MMC3 board, four-screen or not: CHR bank switching is independent of
 * nametable wiring.
 */
u32 ppu::ResolveTile(const u16 tileVMA) {
    return mmc3::GetTileLMA(tileVMA);
}

#if ALTERNATIVE_NAMETABLE == 1
/**
 * @brief Four-screen nametable count -- a real extra 2 KiB cartridge VRAM
 * chip, giving 4 genuinely distinct physical nametables with no mirroring
 * aliasing at all (mmc3.hpp's own comment on ALTERNATIVE_NAMETABLE), only
 * compiled in when this board build carries that chip. A board built without
 * it never defines this symbol, so the emu PPU's weak default
 * (::ppu::nametableCount == 2) stands unchanged -- correct for MMC3's
 * ordinary runtime H/V mirroring switch (::SetMirroring), ::emu::
 * ComputeNtGeometry (src/emu/emu.hpp) grids either shape from the same
 * ::VideoRAM allocation, so nothing else needs to special-case this board:
 * ::video::vram_bytes() already sizes ::VideoRAM for all 4 pages, and the
 * weak ::ppu::ReadNametable/::WriteNametable/::Flush defaults already walk
 * whichever count is linked in. A real four-screen board's $A000 write is
 * simply meaningless against this (each quadrant already has fixed,
 * dedicated storage), so ::SetMirroring needs no special-casing either.
 */
extern const u8 ppu::nametableCount = 4;
#endif
