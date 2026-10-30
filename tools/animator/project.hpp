/**
 * @file project.hpp
 * @brief The .anim project file and its sidecar .msprite metasprite manifests.
 *
 * An .anim is a small TOML file naming two things: the blobman manifest the
 * tiles come from, and the sidecar folder holding the metasprites. Each
 * metasprite is its own .msprite manifest in that folder -- one file per
 * metasprite, listing its frames in play order, and each frame's sprites with
 * their slot, tile id, mirroring and the include the art came from.
 *
 * Both are TOML because everything else in this project's tooling already is
 * (blobman's manifests, demo/chr/config.toml), so one parser covers the lot and
 * the files stay hand-editable and diffable.
 *
 * Deliberately Qt-free -- plain std types only, so the format is independent of
 * the GUI that fills it.
 */
#pragma once

#include "chr_table.hpp"
#include "palette.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace proj {

/// File extensions. The sidecar folder's default name is relative to the .anim.
constexpr const char* kProjectExtension     = ".anim";
constexpr const char* kMetaspriteExtension  = ".msprite";
constexpr const char* kDefaultMetaspritesDir = "metasprites";

/// One placed sprite of one frame.
struct Sprite {
    int         col = 0;     ///< Slot column.
    int         row = 0;     ///< Slot row; a slot is one tile tall, 8 or 16 pixels by mode.
    int         id = 0;      ///< PPU tile id: an index within its pattern table.
    int         patternByte = 0;  ///< What a consumer puts in OAM for it, half bit and all.
    std::string entry;       ///< blobman entry the art came from.
    std::string include;     ///< Include (filename stem) within that entry.
    int         tile = 0;    ///< Tile index within that include.
    bool        flipH = false;  ///< Mirrored horizontally.
    bool        flipV = false;  ///< Mirrored vertically.
};

/**
 * @brief One animation frame.
 *
 * Frames are contiguous: the file's order IS play order, the last frame loops
 * back to the first, and a @ref timer of 0 ends the animation on that frame.
 * Nothing links one frame to another, by design.
 */
struct Frame {
    std::string         name;
    int                 timer = 0;  ///< NES frames to hold; 0 stops the animation here.

    /// Expose this frame's bytes in the generated header, where the frame table
    /// is otherwise private (see codegen.hpp). For an animation nothing plays
    /// -- a still pose, a frame picked by game state rather than by a timer --
    /// going through the frame index is ceremony, so the frame gets a name of
    /// its own that consumers can take the address of directly.
    bool                isPublic = false;

    std::vector<Sprite> sprites;    ///< In placement order, which is draw order.
};

/// One metasprite: a .msprite file.
struct Metasprite {
    std::string        name;

    /// Name of the palette it is drawn with, empty for the placeholder
    /// colours. A name, not an index, so adding or removing a palette cannot
    /// repoint a metasprite at a different one.
    std::string        palette;

    /// The file stem this metasprite was last written as, empty if never. A
    /// save RENAMES that file when the name has changed since, so renaming a
    /// metasprite moves its manifest instead of leaving a stale copy beside the
    /// new one. ::save updates it to what it wrote.
    std::string        savedStem;

    int                tilesX = 1;  ///< Extent in tiles, indifferent to 8x8 vs 8x16.
    int                tilesY = 1;
    std::vector<Frame> frames;      ///< In play order.
};

/// A whole project: the .anim plus everything its sidecar folder holds.
struct Project {
    /// The .anim's own path. Not written into the file -- every path inside it
    /// is relative to this one's directory.
    std::filesystem::path path;

    std::string chrManifest = "";                          ///< Path to the blobman manifest, as written.
    std::string metaspritesDir = kDefaultMetaspritesDir;   ///< Sidecar folder, as written.
    chr::TileMode mode = chr::TileMode::Size8x8;           ///< Tile grouping the project was authored in.

    /// Named palettes, shared by every metasprite in the project. They live in
    /// the .anim rather than per metasprite because that is what sharing means.
    std::vector<pal::Palette> palettes;

    std::vector<Metasprite>  metasprites;  ///< In the order the .anim lists them.
    std::vector<std::string> warnings;     ///< Non-fatal problems found while loading.
};

/**
 * @brief Writes @p project: the .anim, and one .msprite per metasprite.
 *
 * The sidecar folder is created if it does not exist. A metasprite whose name
 * has changed since its last save is MOVED to the new name (see
 * Metasprite::savedStem): the new file is written and the old stem is removed,
 * so the folder never collects a stale copy under the old name. Only a stem
 * this project wrote before, and that nothing claims now, is removed -- a save
 * never touches a file it did not write, including the manifest of a metasprite
 * that has since been closed.
 *
 * @p project is updated in place: every metasprite's savedStem becomes the stem
 * it was written as, so a second save renames from the right place.
 *
 * @param error Set to a human-readable reason on failure.
 */
bool save(Project& project, std::string& error);

/**
 * @brief Reads an .anim and every .msprite it lists.
 *
 * A metasprite file that is missing or malformed is skipped and recorded in
 * Project::warnings; only the .anim itself failing is an error.
 *
 * @param error Set to a human-readable reason when the result is empty.
 */
std::optional<Project> load(const std::filesystem::path& path, std::string& error);

/// @p relative resolved against @p project's own directory. An absolute
/// @p relative is returned unchanged.
std::filesystem::path resolve(const Project& project, const std::string& relative);

/// @p name folded into a filename-safe stem, for a .msprite. Mirrors blobman's
/// own identifier folding so the two tools agree on what a name may contain.
std::string fileStem(const std::string& name);

/**
 * @brief The cell of @p table holding the art a sprite names, or nullptr.
 *
 * Matched on the art itself -- @p entry, @p include and @p tile, the tile's
 * index within that include -- because that is what survives a CHR layout
 * change: moving an entry's VMA or repacking an include renumbers every id in
 * it while the sprite still means the same picture. @p id is only the
 * fallback, for a .msprite naming art no include of this manifest has.
 *
 * Shared by the editor's own load path and the headless export (exporter.cpp)
 * so the two cannot disagree about which cell -- and therefore which pattern
 * byte -- a saved sprite resolves to.
 */
const chr::Cell* findCell(const chr::Table& table, const std::string& entry,
                          const std::string& include, int tile, int id);

} // namespace proj
