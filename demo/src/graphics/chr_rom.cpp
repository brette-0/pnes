// The cartridge's entire CHR ROM image, in one object.
//
// The layout is declared up front in demo/chr/config.toml and packed by
// blobman (tools/blobman) into a single demo/chr/chr.bin, already padded to
// the full [manifest] size with the gaps between entries zeroed. Tile ids come
// from the header blobman writes beside the blob (demo/chr/chr.h), so nothing
// here has to be read to know where a tile sits -- this TU's only job is to
// get those bytes into the .chr_rom output section (demo/link.ld's chr_rom
// region) and say how many there are.
//
// TRACK THESE BY HAND, all four must agree:
//   * [manifest] size in demo/chr/config.toml  (what blobman writes)
//   * chrRomSize below                         (what lands in .chr_rom)
//   * __chr_rom_size in demo/link.ld           (KiB; the region it lands in)
//   * HEADER()'s chr_rom_bytes in header.hpp   (what the NES2.0 header claims)
//
// #embed paths are relative to THIS file (demo/src/graphics/).

#include <pnes/video.hpp>

#include <array>
#include <cstddef>

namespace {
    constexpr std::size_t chrRomSize = 0x4000;   // 16 KiB -- two MMC3 CHR pages

    // The staging buffer is `int` because the compilers disagree on what
    // #embed expands to: GCC yields ints in [0,255], Clang yields char-typed
    // values (0xFF -> -1 on a signed-char target). No 8-bit type holds that
    // combined [-128,255] range without a braced-init narrowing error. It is
    // internal-linkage constexpr, consumed purely at compile time, so its
    // width costs nothing at runtime.
    constexpr int chrRomRaw[] = {
#embed "../../chr/chr.bin"
    };

    static_assert(sizeof(chrRomRaw) / sizeof(*chrRomRaw) == chrRomSize,
                  "demo/chr/chr.bin is not chrRomSize bytes -- repack it, or "
                  "update chrRomSize, link.ld's __chr_rom_size and header.hpp "
                  "together");
}

// INLINE IS LOAD-BEARING, not a style choice. constexpr at namespace scope
// implies const, which implies INTERNAL linkage -- and a local, unreferenced
// 16 KiB section is exactly what --gc-sections exists to delete. Nothing names
// this object: the NES linker script places it by section, and off-NES the
// library reaches it through the linker's own __start_chr_rom. gnu::retain
// would protect it, but devkitARM/devkitPPC and clang-cl report
// `'retain' attribute ignored`, so it cannot be relied on.
//
// `inline` gives it external linkage with COMDAT folding, which is what the
// old per-asset chain (_CHR_EMIT_IMAGE in video.hpp) used and what every
// devkitPro target and Windows need to keep it alive. Dropping it builds fine
// on NES (demo/link.ld KEEP()s .chr_rom) and on targets that do not garbage
// collect sections, then fails at link everywhere that does, with
// __start_chr_rom undefined.
[[gnu::section(_CHR_SECTION), gnu::used, gnu::retain]]
inline constexpr auto chr_rom_image = [] {
    std::array<u8, chrRomSize> image{};
    for (std::size_t i = 0; i < chrRomSize; i++) {
        image[i] = static_cast<u8>(chrRomRaw[i]);
    }
    return image;
}();

// Off-NES tile-address translators wrap their resolved offset against this, so
// a bank register addressing past what the build embeds cannot read out of
// bounds (see mmc3::GetTileLMA).
[[gnu::used, gnu::retain]]
inline constexpr unsigned ppu::chrRomBytes = chrRomSize;
