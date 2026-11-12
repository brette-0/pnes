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

/// Tiles a frame covers: the whole 0x00-0xff range of a tile id, which is what
/// makes a frame the unit a sprite can address rather than a choice of layout.
constexpr int kFrameTiles = 256;

/// How the table groups CHR tiles into cells.
enum class TileMode {
    Size8x8,   ///< One tile per cell.
    Size8x16,  ///< Two vertically adjacent tiles per cell (PPUCTRL sprite height).
};

/**
 * @brief The tile-id space a sprite draws from: 0x00-0xff, divided into N
 *        equally sized windows whose ORDER is what gives a tile its id.
 *
 * A frame is a RUNTIME arrangement -- which regions of character ROM the game
 * intends to have banked in together, and in what order -- so it is declared
 * in the .anim (proj::Project::frames), never in the CHR manifest. Character
 * generation neither designs a frame nor enforces one; it only says where the
 * art was packed. What this type adds is the other half: a tile's id is its
 * offset into the window holding it, plus that window's base, and the base is
 * the window's position in ::windows times the window size.
 *
 * Window size is not stored because it is not a choice: ::kFrameTiles divided
 * by however many windows there are. That is what "equally sized" means here,
 * and it is why the count has to divide 256.
 *
 * Not to be confused with proj::Frame, which is one frame of an ANIMATION --
 * an unrelated sense of the word that predates this one. The two never appear
 * unqualified in the same scope.
 */
struct Frame {
    std::string name;   ///< What a sprite names to draw from it.

    /**
     * @brief PPU address of the pattern table the frame's ids live in: 0x0000
     *        or 0x1000.
     *
     * Only an 8x16 sprite needs it, where the pattern byte's bit 0 selects the
     * table (see Cell::patternByte). An 8x8 sprite takes its table from
     * PPUCTRL instead, so this is inert for one.
     */
    std::uint32_t patternTable = 0;

    /// The entry banked in each window, in id order -- window 0 holds the
    /// lowest ids. An empty name is a window the game leaves to something
    /// else; it reserves the ids without offering any art.
    std::vector<std::string> windows;
};

/// One entry table of the manifest, in manifest (toml key) terms.
struct Entry {
    std::string   name;        ///< The toml key.
    std::uint32_t lma = 0;     ///< Offset inside the blob.
    std::uint32_t size = 0;    ///< Bytes the entry reserves.
    std::uint32_t filled = 0;  ///< Bytes its includes actually fill.
};

/// ::Cell::id of a cell no frame reaches, which therefore has no id at all.
/// Not a legal tile id, so it cannot be confused with one.
constexpr int kNoId = -1;

/// One drawable unit: an 8x8 tile, or an 8x16 pair of them.
struct Cell {
    /**
     * @brief PPU tile id; the TOP tile's id when 8x16. ::kNoId when the table
     *        was loaded with no frames.
     *
     * A tile has no id of its own: an id is where a FRAME puts it (::frame's
     * window base plus ::tileInEntry). So a cell loaded without frames is
     * genuinely unnumbered rather than numbered from somewhere else -- there
     * is nowhere else. Such a cell can be looked at but not stamped, since a
     * sprite with no pattern byte is not one anything could export.
     */
    int         id = kNoId;

    /// The byte OAM takes for this cell, which is NOT always ::id, and is
    /// ::kNoId whenever ::id is.
    ///
    /// In 8x16 mode the pattern byte's bit 0 selects the pattern table and bits
    /// 7-1 select the pair, so the half of the pattern table its frame sits in
    /// has to be folded into the id -- the frame is the only thing that knows
    /// it, and a consumer handed a bare id can only guess. An 8x8 cell's half
    /// comes from PPUCTRL instead, so there it is ::id unchanged.
    int         patternByte = kNoId;
    std::size_t entry = 0;          ///< Index into Table::entries.

    /// Frame whose window the art is in, by name, or empty when the table was
    /// loaded with no frames at all (see ::loadManifest). A sprite records it,
    /// because the same art reached through a different frame is a different
    /// id -- so it is part of what identifies a cell, not decoration.
    std::string frame;

    /// Which window of ::frame, i.e. which slice of the id range the art is
    /// in. -1 when the table has no frames.
    int         window = -1;

    std::string include;            ///< Stem of the .chr file it came from.
    int         tileInInclude = 0;  ///< 0-based tile index within that include.

    /// 0-based tile index within the ENTRY, across its includes. The quantity
    /// a frame is built on -- an id is this plus the base of the window the
    /// entry sits in -- and what lets a tile be found without knowing which
    /// include it fell in.
    int         tileInEntry = 0;
    int         heightTiles = 1;    ///< 1 (8x8) or 2 (8x16).

    /// 2bpp palette indices (0-3, coloured by a pal::Palette), row-major,
    /// kTilePixels wide by kTilePixels * heightTiles tall.
    std::vector<std::uint8_t> pixels;

    /// Whether a frame reached this cell, i.e. whether ::id and ::patternByte
    /// mean anything.
    [[nodiscard]] bool numbered() const { return id != kNoId; }

    [[nodiscard]] int widthPx() const { return kTilePixels; }
    [[nodiscard]] int heightPx() const { return kTilePixels * heightTiles; }
};

/// Everything one manifest resolves to.
struct Table {
    std::filesystem::path    path;         ///< The manifest that was read.
    std::uint32_t            blobSize = 0; ///< [manifest] size.
    TileMode                 mode = TileMode::Size8x8;
    std::vector<Frame>       frames;       ///< As given to ::loadManifest, in declaration order.
    std::vector<Entry>       entries;      ///< In blob order (sorted by LMA), as blobman emits them.
    std::vector<Cell>        cells;        ///< Entry order, then include pack order.
    std::vector<std::string> warnings;     ///< Non-fatal oddities worth surfacing.
};

/**
 * @brief Reads a blobman manifest and decodes the tiles @p frames reach.
 *
 * Validation mirrors blobman's own, so a manifest the build accepts loads here
 * and one it rejects is rejected here for the same stated reason: LMA and size
 * must be present and positive, an entry must not overflow the blob, entries
 * must not overlap, and an include must start on a tile boundary and fit in
 * its entry.
 *
 * Nothing here reads a `VMA`. Where the PPU sees an entry is not a property of
 * how it was packed -- it is what a frame decides -- so an entry contributes
 * its LMA, its size and its art, and nothing about placement.
 *
 * @p frames is what decides WHICH tiles become cells and what id each one
 * gets. An entry no frame has a window for yields no cells: a sprite draws
 * from a frame, not from the whole of character ROM, so art that nothing
 * intends to bank in is not art a sprite can be given. An entry in two frames
 * yields its tiles twice, once per frame, with the ids each frame gives them
 * -- which is the same art genuinely being two different sprites.
 *
 * Frames are checked here rather than trusted: the window count must divide
 * ::kFrameTiles, a window must name an entry the manifest has, that entry must
 * fit the window, no entry may hold two windows of one frame, and the pattern
 * table must be one of the two. An entry smaller than its window is allowed
 * and warned about -- the remaining ids belong to whatever the game banks
 * there, which is not this tool's business to know.
 *
 * Passing no frames at all yields every entry's tiles as UNNUMBERED cells
 * (::kNoId): art and provenance, drawable and inspectable, with no id and no
 * pattern byte, because without a frame there is no id to give them. That is
 * for browsing a manifest with no project open; a project always declares its
 * frames.
 *
 * Include paths resolve against the manifest's own directory -- which is what
 * blobman effectively does too, since the build runs it with the manifest's
 * directory as the working directory (see the root CMakeLists.txt). The
 * process working directory is tried as a fallback.
 *
 * @param path   Manifest (.toml) to read.
 * @param mode   Whether to group tiles into 8x16 pairs.
 * @param frames The frames tiles are reached through; empty for the
 *               unnumbered load described above.
 * @param error  Set to a human-readable reason when the result is empty;
 *               untouched on success.
 */
std::optional<Table> loadManifest(const std::filesystem::path& path, TileMode mode,
                                  const std::vector<Frame>& frames, std::string& error);

/// Overload for a frameless, unnumbered load; see ::loadManifest.
std::optional<Table> loadManifest(const std::filesystem::path& path, TileMode mode,
                                  std::string& error);

} // namespace chr
