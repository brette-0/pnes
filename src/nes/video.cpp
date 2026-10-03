#include <platform-nes/video.hpp>
#include <platform-nes/technology.hpp>
#include <platform-nes/interrupts.hpp>

// WHERE THIS MODULE'S CODE GOES -- the consuming project's choice, supplied as
// PLATFORM_NES_VIDEO_SECTION. Optional, unlike audio's: unset means the default,
// everything inlined into callers and placed nowhere in particular.
//
// VIDEO_BANK tags every definition that may move. ::AI tags the ones that must
// NOT: anything an interrupt handler reaches pays argument marshalling plus
// jsr/rts inside a budget of one vblank or one scanline, and a placed module
// adds a bank switch on top. Which functions those are is a property of the
// CALLER, so the pinned set is what a project can plausibly call from an NMI or
// IRQ -- the scroll helpers, the name/attribute-table writers, RefreshSprites.
//
// The two are mutually exclusive, and ::AI alone is the whole answer. A section
// cannot place an inlined function, and in the one case where it could -- taking
// its address forces an out-of-line copy -- landing that copy in the banked
// section is precisely wrong, since the pin exists to keep it reachable without
// banking. So pinned functions carry no section at all.
//
// Pinning is free at present: LTO inlines all of them anyway. It only stops the
// inliner from deciding otherwise later, where the failure is an overrun with no
// diagnostic.
#ifdef PLATFORM_NES_VIDEO_SECTION
#define VIDEO_BANK MODULE_PLACEMENT(PLATFORM_NES_VIDEO_SECTION)
#else
#define VIDEO_BANK
#endif
#include <intsh>
using namespace br0::intsh;

const u16 video::PatternTables = 0;
constexpr u16 NameTables       = 0x2000;
constexpr u16 PaletteTables    = 0x3f00;
constexpr u16 nVideoRam        = 0x800;
atomic u16 xScroll = 0;
atomic u16 yScroll = 0;
namespace ppu {
tech::wo_register<raw::PPUCTRL>   PPUCTRL;
tech::wo_register<raw::PPUMASK>   PPUMASK;
}   // namespace ppu

namespace oam {
tech::wo_register<ppu::raw::OAMADDR> OAMADDR;
tech::wo_register<ppu::raw::OAMDMA>  OAMDMA;
}   // namespace oam

static u16 xy_to_nt_addr(const u16 x, const u16 y) {
    constexpr u16 base = 0x2000;
    const u16 nt_h = static_cast<u16>((x >> 5 & 1) << 10);
    const u16 col  = x & 0x1F;
    if (y < 30) {
        return base + nt_h + (y << 5) + col;
    }
    const u16 nt_v = static_cast<u16>((y / 30) << 11);
    const u16 row  = y % 30;
    return base + nt_h + nt_v + row * 32 + col;
}

static u16 xy_to_at_addr(const u16 x, const u16 y) {
    constexpr u16 base = 0x2000;
    const u16 nt_h = static_cast<u16>((x >> 5 & 1) << 10);
    const u16 col  = x & 0x1F;
    if (y < 30) {
        return base + nt_h + 0x3C0 + ((y >> 2) << 3) + (col >> 2);
    }
    const u16 nt_v = static_cast<u16>((y / 30) << 11);
    const u16 row  = y % 30;
    return base + nt_h + nt_v + 0x3C0 + (row / 4) * 8 + (col / 4);
}

namespace video {

// Spins on ::irq::nmi_done, which every ::NMI vector sets true right after
// its body returns (see that macro's own doc comment), then clears it --
// so this blocks until exactly one more NMI has run, giving NES the same
// "wait for the next VBlank to be presented" semantics ::WaitForPresent
// already has on desktop, with no per-mode done-flag required.
VIDEO_BANK void WaitForPresent() {
    while (!irq::nmi_done) {}
    irq::nmi_done = false;
}

}   // namespace video

namespace ppu {

VIDEO_BANK void EnableRendering(const u8 ppuCtrl_, const u8 ppuMask_) {
    PPUCTRL = 0x80 | ppuCtrl_;   // each assignment writes shadow + NMI enable
    PPUMASK = ppuMask_;
}

// Weak default: flushes the 2 KiB of physical CIRAM every ordinary
// (non-four-screen) mirroring mode aliases across the full $2000-$2FFF
// nametable range. A board wired for four-screen mirroring exposes a full
// 4 KiB of distinct physical storage instead -- no mirroring left to make 2
// pages implicitly cover all 4 logical nametables -- so its own mapper TU
// (see src/nes/mappers/mmc3.cpp's ALTERNATIVE_NAMETABLE build) supplies a
// strong override that walks all 4 pages. Resolved at link time, same idiom
// as the emu backend's ppu::ReadNametable/WriteNametable (src/emu/ppu.cpp).
__attribute__((weak))
VIDEO_BANK void Flush(const u8 nt, const u8 at) {
    tech::peek(raw::PPUSTATUS);
    tech::poke(raw::PPUADDR, NameTables >> 8);
    tech::poke(raw::PPUADDR, NameTables & 0xFF);

    for (auto page = 0; page < nVideoRam / 0x400; page++) {
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

// Pinned: a raster split writes PPUSCROLL at an exact dot, so anything added in
// front of these writes moves the split.
__attribute__((hot))
AI void SetScroll(const vec2<u16> pos) {
    const u16 x = pos.x;
    u16 y = pos.y;
    xScroll = x; yScroll = y;

    // Wraps y Past one nametable's height (240) into the next one, tagging
    // bit 0x100 so the nt-bit derivation below picks it up (y >> 7 & 0x02
    // reads out exactly that tag). This used to run twice unconditionally:
    // the first pass's tag (y |= 0x100, via XOR against an already-clear
    // bit) makes y >= 240 true again on its own, so the second copy fired
    // every time the first one did too, re-subtracting 240 and re-toggling
    // the tag back off -- e.g. y=240 (exactly one nametable down) came out
    // as final y=16 with the tag lost, instead of the intended y=0 with the
    // nametable-select bit correctly set.
    if (y >= 240) {
        y -= 240;
        y ^= 0x100;
    }

    const auto nt = x >> 8 & 0x01
               | y >> 7 & 0x02;

    PPUCTRL = PPUCTRL & 0xFC | nt;

    tech::poke(raw::PPUSCROLL, static_cast<u8>(x & 0xFF));
    tech::poke(raw::PPUSCROLL, static_cast<u8>(y & 0xFF));
}

AI void DeltaScroll(const vec2<i8> delta) {
    SetScroll({static_cast<u16>(xScroll + delta.x), static_cast<u16>(yScroll + delta.y)});
}

__attribute__((hot))
AI void WriteFromBufferToNameTable(
    const u16 address, const u8* source, const u8 sBuffer, const u8 polarity
) {
    u8 ctrl = PPUCTRL & ~ctrl::POLARITY;
    if (polarity) ctrl = ctrl | ctrl::POLARITY;
    PPUCTRL = ctrl;   // one write back: shadow + hardware

    tech::peek(raw::PPUSTATUS);
    tech::poke(raw::PPUADDR, static_cast<u8>(address >> 8));
    tech::poke(raw::PPUADDR, static_cast<u8>(address & 0xFF));

    for (auto i = 0; i < sBuffer; i++) {
        tech::poke(raw::PPUDATA, source[i]);
    }
}

__attribute__((hot))
AI void WriteFromBufferToNameTable(
    const vec2<u16> pos, const u8* source, const u8 sBuffer, const u8 polarity
) {
    WriteFromBufferToNameTable(xy_to_nt_addr(pos.x, pos.y), source, sBuffer, polarity);
}

// Address overload: the caller already projected (x,y) -> a $2000-based PPU address
// (via CartesianToAddress), so this skips xy_to_nt_addr entirely -- just the latch
// reset and three pokes. Meant for the vblank window, where the divide/modulo in the
// (x,y) form is the cost worth hoisting out.
__attribute__((hot))
AI void WriteRepeatedToNameTable(
    const u16 address, const u8 value, const u8 amt, const u8 polarity
) {
    u8 ctrl = PPUCTRL & ~ctrl::POLARITY;
    if (polarity) ctrl = ctrl | ctrl::POLARITY;
    PPUCTRL = ctrl;   // one write back: shadow + hardware

    tech::peek(raw::PPUSTATUS);
    tech::poke(raw::PPUADDR, static_cast<u8>(address >> 8));
    tech::poke(raw::PPUADDR, static_cast<u8>(address & 0xFF));

    for (u8 i = 0; i < amt; i++) {
        tech::poke(raw::PPUDATA, value);
    }
}

__attribute__((hot))
AI void WriteRepeatedToNameTable(
    const vec2<u16> pos, const u8 value, const u8 amt, const u8 polarity
) {
    WriteRepeatedToNameTable(xy_to_nt_addr(pos.x, pos.y), value, amt, polarity);
}

__attribute__((hot))
AI void WriteSingleToNameTable(const u16 address, const u8 value) {
    tech::peek(raw::PPUSTATUS);
    tech::poke(raw::PPUADDR, static_cast<u8>(address >> 8));
    tech::poke(raw::PPUADDR, static_cast<u8>(address & 0xFF));
    tech::poke(raw::PPUDATA, value);
}

__attribute__((hot))
VIDEO_BANK void WriteSingleToNameTable(const vec2<u16> pos, const u8 value) {
    WriteSingleToNameTable(xy_to_nt_addr(pos.x, pos.y), value);
}

template <typename Idx>
__attribute__((hot))
VIDEO_BANK void WriteFromProviderToNameTable(
    const u16 address, u8 (*fn)(Idx), const u8 amt, const u8 polarity
) {
    u8 ctrl = PPUCTRL & ~ctrl::POLARITY;
    if (polarity) ctrl = ctrl | ctrl::POLARITY;
    PPUCTRL = ctrl;   // one write back: shadow + hardware

    tech::peek(raw::PPUSTATUS);
    tech::poke(raw::PPUADDR, static_cast<u8>(address >> 8));
    tech::poke(raw::PPUADDR, static_cast<u8>(address & 0xFF));

    for (Idx i = 0; i < amt; ++i) {
        tech::poke(raw::PPUDATA, fn(i));
    }
}

// Explicit instantiations for the provider index types in use. The body pokes
// PPU registers, so it must stay in this backend rather than the header.
template void WriteFromProviderToNameTable<u8>(u16, u8 (*)(u8), u8, u8);
template void WriteFromProviderToNameTable<u16>(u16, u8 (*)(u16), u8, u8);

template <typename Idx>
__attribute__((hot))
VIDEO_BANK void WriteFromProviderToNameTable(
    const vec2<u16> pos, u8 (*fn)(Idx), const u8 amt, const u8 polarity
) {
    WriteFromProviderToNameTable(xy_to_nt_addr(pos.x, pos.y), fn, amt, polarity);
}

// Explicit instantiations for the provider index types in use. The body pokes
// PPU registers, so it must stay in this backend rather than the header.
template void WriteFromProviderToNameTable<u8>(vec2<u16>, u8 (*)(u8), u8, u8);
template void WriteFromProviderToNameTable<u16>(vec2<u16>, u8 (*)(u16), u8, u8);

__attribute__((hot))
AI void WriteFromBufferToAttributeTable(
    const u16 address, const u8* source, const u8 sBuffer, const u8 polarity
) {
    PPUCTRL = PPUCTRL & ~ctrl::POLARITY;

    if (polarity) {
        // AT rows for the same tile column are 8 bytes apart.  Re-address each
        // byte explicitly (peek PPUSTATUS + 2 pokes = 12 cycles/byte) instead of
        // seven dummy PPUDATA reads (7×4 = 28 cycles/byte) to stride between rows.
        // base_lo + i*8 stays ≤ $FF for sBuffer≤8 (base_lo ≤ $C7, 7×8 = $38).
        const u8 hi = static_cast<u8>(address >> 8);
        const u8 lo = static_cast<u8>(address & 0xFF);
        for (u8 i = 0; i < sBuffer; i++) {
            tech::peek(raw::PPUSTATUS);
            tech::poke(raw::PPUADDR, hi);
            tech::poke(raw::PPUADDR, static_cast<u8>(lo + (i << 3)));
            tech::poke(raw::PPUDATA, source[i]);
        }
        return;
    }

    tech::peek(raw::PPUSTATUS);
    tech::poke(raw::PPUADDR, static_cast<u8>(address >> 8));
    tech::poke(raw::PPUADDR, static_cast<u8>(address & 0xFF));
    for (u8 i = 0; i < sBuffer; i++) {
        tech::poke(raw::PPUDATA, source[i]);
    }
}

__attribute__((hot))
AI void WriteFromBufferToAttributeTable(
    const vec2<u16> pos, const u8* source, const u8 sBuffer, const u8 polarity
) {
    WriteFromBufferToAttributeTable(xy_to_at_addr(pos.x, pos.y), source, sBuffer, polarity);
}

AI void WriteSingleToAttributeTable(const u16 address, const u8 value) {
    tech::peek(raw::PPUSTATUS);
    tech::poke(raw::PPUADDR, static_cast<u8>(address >> 8));
    tech::poke(raw::PPUADDR, static_cast<u8>(address & 0xFF));
    tech::poke(raw::PPUDATA, value);
}

AI void WriteSingleToAttributeTable(const vec2<u16> pos, const u8 value) {
    WriteSingleToAttributeTable(xy_to_at_addr(pos.x, pos.y), value);
}

template <typename Idx>
__attribute__((hot))
AI void WriteFromProviderToAttributeTable(
    const u16 address, u8 (*fn)(Idx), const u8 amt, const u8 polarity
) {
    PPUCTRL = PPUCTRL & ~ctrl::POLARITY;

    if (polarity) {
        // AT rows for the same tile column are 8 bytes apart, so re-address per
        // byte instead of striding via PPUCTRL/PPUDATA reads. Unlike
        // WriteFromBufferToAttributeTable's sBuffer<=8 callers, amt here can run
        // past 8 (e.g. title::InitTitleScreen's full-height pass), so the stride
        // is carried through a 16-bit add rather than truncated to the low byte.
        for (Idx i = 0; i < amt; ++i) {
            const u16 addr = static_cast<u16>(address + (static_cast<u16>(i) << 3));
            tech::peek(raw::PPUSTATUS);
            tech::poke(raw::PPUADDR, static_cast<u8>(addr >> 8));
            tech::poke(raw::PPUADDR, static_cast<u8>(addr & 0xFF));
            tech::poke(raw::PPUDATA, fn(i));
        }
        return;
    }

    tech::peek(raw::PPUSTATUS);
    tech::poke(raw::PPUADDR, static_cast<u8>(address >> 8));
    tech::poke(raw::PPUADDR, static_cast<u8>(address & 0xFF));
    for (Idx i = 0; i < amt; ++i) {
        tech::poke(raw::PPUDATA, fn(i));
    }
}

// Explicit instantiations for the provider index types in use. The body pokes
// PPU registers, so it must stay in this backend rather than the header.
template void WriteFromProviderToAttributeTable<u8>(u16, u8 (*)(u8), u8, u8);
template void WriteFromProviderToAttributeTable<u16>(u16, u8 (*)(u16), u8, u8);

template <typename Idx>
__attribute__((hot))
AI void WriteFromProviderToAttributeTable(
    const vec2<u16> pos, u8 (*fn)(Idx), const u8 amt, const u8 polarity
) {
    WriteFromProviderToAttributeTable(xy_to_at_addr(pos.x, pos.y), fn, amt, polarity);
}

// Explicit instantiations for the provider index types in use. The body pokes
// PPU registers, so it must stay in this backend rather than the header.
template void WriteFromProviderToAttributeTable<u8>(vec2<u16>, u8 (*)(u8), u8, u8);
template void WriteFromProviderToAttributeTable<u16>(vec2<u16>, u8 (*)(u16), u8, u8);

VIDEO_BANK u16 CartesianToAddress(const vec2<u16>& pos) {
    return xy_to_nt_addr(pos.x, pos.y);
}

AI scroll_t CartesianToScroll(const vec2<u16> pos) {
    const u16 px = pos.x;
    auto y = pos.y;
    if (y >= 240) { y -= 240; y ^= 0x100; }
    if (y >= 240) { y -= 240; y ^= 0x100; }
    const auto nt = static_cast<u8>(px >> 8 & 0x01 | y >> 7 & 0x02);
    return (scroll_t){{ static_cast<u8>(PPUCTRL & 0xFC | nt),
        static_cast<u8>(px & 0xFF), static_cast<u8>(y & 0xFF) }
    };
}

__attribute__((hot))
AI void SetColorPriority(const u8 priority) {
    u8 mask = PPUMASK & ~(mask::RED | mask::GREEN | mask::BLUE);
    mask = mask | priority & (mask::RED | mask::GREEN | mask::BLUE);
    PPUMASK = mask;   // one write back: shadow + hardware
}

namespace pal {

__attribute__((hot))
VIDEO_BANK void WriteFromBuffer(const u8 offset, const u8* source, const u8 sBuffer) {
    tech::peek(raw::PPUSTATUS);
    tech::poke(raw::PPUADDR, static_cast<u8>((offset + PaletteTables) >> 8));
    tech::poke(raw::PPUADDR, static_cast<u8>(offset + PaletteTables & 0xFF));

    for (auto i = 0; i < sBuffer; i++) {
        tech::poke(raw::PPUDATA, source[i]);
    }
}

AI void WriteSingle(const u8 offset, const u8 value) {
    tech::peek(raw::PPUSTATUS);
    tech::poke(raw::PPUADDR, static_cast<u8>((offset + PaletteTables) >> 8));
    tech::poke(raw::PPUADDR, static_cast<u8>(offset + PaletteTables & 0xFF));
    tech::poke(raw::PPUDATA, value);
}

}   // namespace pal

}   // namespace ppu

namespace oam {

AI void RefreshSprites(const sprite_t *oam) {
    u16 addr;
    __builtin_memcpy(&addr, &oam, sizeof addr);
    tech::poke(ppu::raw::OAMDMA, static_cast<u8>(addr >> 8));
}

VIDEO_BANK void OAMFromBuffer(sprite_t *oam, const u8 slot, const u16 off,
                   const u8 width, const u8 *src, const u16 count) {
    (void)width;
    u8 *dst = reinterpret_cast<u8 *>(oam) + static_cast<u16>(slot) * spriteStride + off;
    const u8 *s = src + off;
    for (u16 i = 0; i < count; i++)
        dst[i * spriteStride] = s[i * spriteStride];
}

VIDEO_BANK void OAMFromProvider(sprite_t *oam, const u8 slot, const u16 off,
                     const u8 width, oam_t (*fn)(u16), const u16 count) {
    (void)width;
    u8 *base = reinterpret_cast<u8 *>(oam) + static_cast<u16>(slot) * spriteStride + off;
    for (u16 i = 0; i < count; i++)
        base[i * spriteStride] = fn(i);
}

}   // namespace oam

namespace ppu {

VIDEO_BANK void StreamFromVideoMemory(const u16 offset, atomic u8* target, const u8 size) {
    tech::peek(raw::PPUSTATUS);
    tech::poke(raw::PPUADDR, static_cast<u8>(offset >> 8));
    tech::poke(raw::PPUADDR, static_cast<u8>(offset & 0xFF));
    for (auto i = 0; i < size; i++) {
        target[i] = tech::peek(raw::PPUDATA);
    }
}

}   // namespace ppu

__attribute__((hot))
VIDEO_BANK void video::WaitThenReactToSpriteZero(const vec2<u16> pos, void (*fn)(), atomic u8* latch) {
    (void)pos;

    while (!*latch) {
        while (  tech::peek(ppu::raw::PPUSTATUS) & 0x40)  { }  // wait for pre-render to clear stale hit
        while (!(tech::peek(ppu::raw::PPUSTATUS) & 0x40)) { }  // wait for actual sprite 0 hit
        fn();
        *latch = true;
    }
}
