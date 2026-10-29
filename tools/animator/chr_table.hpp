/**
 * @file chr_table.hpp
 * @brief Builds a tile table from a blobman manifest (tools/blobman).
 *
 * The read side of blobman: the same [manifest]/entry layout is parsed and
 * validated the same way, but instead of packing a blob this produces one
 * entry per table, one cell per tile (or 8x16 tile pair) of every include,
 * each carrying the PPU tile id the hardware would see and the decoded pixels
 * as 2bpp indices -- palette.hpp is what turns those into colours.
 *
 * Deliberately Qt-free -- plain std types only, so the manifest and CHR format
 * logic stays independent of the GUI that draws it.
 */
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace chr {

/// NES tile geometry: 8x8 pixels, 2bpp as two 8-byte bitplanes.
constexpr int kTilePixels = 8;
constexpr int kTileBytes  = 16;

/// A pattern table holds 256 tiles, which is the whole range of a tile id.
constexpr int kPatternTableBytes = kTileBytes * 256;

/// How the table groups CHR tiles into cells.
enum class TileMode {
    Size8x8,   ///< One tile per cell.
    Size8x16,  ///< Two vertically adjacent tiles per cell (PPUCTRL sprite height).
};

/// One entry table of the manifest, in manifest (toml key) terms.
struct Entry {
    std::string   name;        ///< The toml key.
    std::uint32_t lma = 0;     ///< Offset inside the blob.
    std::uint32_t vma = 0;     ///< PPU address the entry is seen at.
    std::uint32_t size = 0;    ///< Bytes the entry reserves.
    std::uint32_t filled = 0;  ///< Bytes its includes actually fill.
};

/// One drawable unit: an 8x8 tile, or an 8x16 pair of them.
struct Cell {
    int         id = 0;             ///< PPU tile id; the TOP tile's id when 8x16.

    /// The byte OAM takes for this cell, which is NOT always ::id.
    ///
    /// In 8x16 mode the pattern byte's bit 0 selects the pattern table and bits
    /// 7-1 select the pair, so the half the entry's VMA puts the art in has to
    /// be folded into the id -- the manifest is the only thing that knows it,
    /// and a consumer handed a bare id can only guess. An 8x8 cell's half comes
    /// from PPUCTRL instead, so there it is ::id unchanged.
    int         patternByte = 0;
    std::size_t entry = 0;          ///< Index into Table::entries.
    std::string include;            ///< Stem of the .chr file it came from.
    int         tileInInclude = 0;  ///< 0-based tile index within that include.
    int         heightTiles = 1;    ///< 1 (8x8) or 2 (8x16).

    /// 2bpp palette indices (0-3, coloured by a pal::Palette), row-major,
    /// kTilePixels wide by kTilePixels * heightTiles tall.
    std::vector<std::uint8_t> pixels;

    [[nodiscard]] int widthPx() const { return kTilePixels; }
    [[nodiscard]] int heightPx() const { return kTilePixels * heightTiles; }
};

/// Everything one manifest resolves to.
struct Table {
    std::filesystem::path    path;         ///< The manifest that was read.
    std::uint32_t            blobSize = 0; ///< [manifest] size.
    TileMode                 mode = TileMode::Size8x8;
    std::vector<Entry>       entries;      ///< In blob order (sorted by LMA), as blobman emits them.
    std::vector<Cell>        cells;        ///< Entry order, then include pack order.
    std::vector<std::string> warnings;     ///< Non-fatal oddities worth surfacing.
};

/**
 * @brief Reads a blobman manifest and decodes every included tile.
 *
 * Validation mirrors blobman's own, so a manifest the build accepts loads here
 * and one it rejects is rejected here for the same stated reason: LMA/VMA/size
 * must be present and positive, the VMA tile-aligned, an entry must not
 * straddle a pattern-table boundary at its VMA nor overflow the blob, entries
 * must not overlap, and an include must start on a tile boundary and fit in
 * its entry.
 *
 * Include paths resolve against the manifest's own directory -- which is what
 * blobman effectively does too, since the build runs it with the manifest's
 * directory as the working directory (see the root CMakeLists.txt). The
 * process working directory is tried as a fallback.
 *
 * @param path  Manifest (.toml) to read.
 * @param mode  Whether to group tiles into 8x16 pairs.
 * @param error Set to a human-readable reason when the result is empty;
 *              untouched on success.
 */
std::optional<Table> loadManifest(const std::filesystem::path& path, TileMode mode, std::string& error);

} // namespace chr
