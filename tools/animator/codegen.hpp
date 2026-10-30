/**
 * @file codegen.hpp
 * @brief Emits a metasprite's frames as a generated C++ header.
 *
 * The export side of the designer: one header per metasprite, holding nothing
 * but PATTERN BYTES -- a frame is the metasprite's slots, row-major, each the
 * byte OAM takes for the sprite stamped there. That is the sheet's tile id with
 * the pattern-table half its blobman entry's VMA puts the art in folded into
 * bit 0 when the slot is 8x16, which is where the hardware reads it from. The
 * manifest is the only thing that knows that half, so emitting the bare id
 * would hand every consumer a byte it has to repair by hand -- and one that
 * silently draws whatever lives at the same index in the other table if it
 * does not. What is deliberately NOT in there:
 *
 *  * the palette, which is a per-metasprite editor attribute but an OAM
 *    attribute byte and a PPU palette write on the console -- not frame data;
 *  * mirroring, which the editor stores per sprite but nothing emits yet;
 *  * whether a slot is 8x8 or 8x16, which is PPUCTRL's business -- the pattern
 *    byte is written for the height the cell was built at, and an 8x8 cell has
 *    no half bit to carry because PPUCTRL supplies it.
 *
 * What it does add is the derived sizes a consumer would otherwise hand-count
 * and get wrong: the extent, the slot count (the metasprite's total size, i.e.
 * the hardware sprites it takes to draw), the frame count and the byte total.
 *
 * The frame table is private -- a frame is reached through Get(), by index. A
 * frame the designer marks public (proj::Frame::isPublic) additionally gets a
 * public `kTiles_<frame>` naming that row of it: a reference, so it is the same
 * bytes rather than a second copy. That is for the animations nothing plays --
 * a still pose, a frame picked by game state -- where the timer and the frame
 * index are both ceremony and the bytes themselves are what a consumer wants.
 *
 * Deliberately Qt-free -- plain std types only, like proj and chr: what a
 * header looks like is independent of the GUI that filled it in.
 */
#pragma once

#include "project.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace codegen {

/// Extension of a generated header.
constexpr const char* kHeaderExtension = ".hpp";

/// Namespace the generated classes live in, matching uitk's generated output.
constexpr const char* kNamespace = "gen";

/// Pattern byte standing in for a slot no sprite was stamped into. 0xff is a
/// legal one, so this is a convention the generated header states rather than
/// something the hardware reserves -- a metasprite with a hole in it has to
/// skip that slot instead of writing it to OAM.
constexpr int kNoTile = 0xff;

/**
 * @brief @p name folded into a C++ identifier.
 *
 * Near enough to proj::fileStem that a metasprite's header file and the class
 * inside it are named the same thing, except that '-' is a filename character
 * and not an identifier one, so it folds to '_' here.
 */
std::string identifier(const std::string& name);

/**
 * @brief The header text for @p metasprite.
 *
 * @param source The file the data came from -- the project, typically -- named
 *               in the do-not-edit banner after the metasprite itself.
 *               Optional; left out of the banner when empty.
 * @return The header, or an empty string when @p metasprite has no frames
 *         (there is no animation to emit, and a zero-length array is not a
 *         thing C++ will take).
 */
std::string header(const proj::Metasprite& metasprite, const std::string& source = {});

/// Writes codegen::header for @p metasprite to @p file, creating its directory.
/// @param source Named in the banner, as codegen::header takes it.
/// @param error  Set to a human-readable reason on failure.
bool writeHeader(const proj::Metasprite& metasprite, const std::filesystem::path& file,
                 const std::string& source, std::string& error);

/**
 * @brief Writes one header per metasprite into @p dir, named after the
 *        metasprite (`dir/<name>.hpp`).
 *
 * @p dir is created if it does not exist. Nothing is removed from it: a header
 * for a metasprite that has since been renamed or closed is left alone, since
 * the folder is an output directory this tool does not own -- unlike the
 * sidecar folder proj::save writes, where the project file says what belongs.
 *
 * A metasprite with no frames is skipped rather than failing the export, and
 * named in @p skipped. Two metasprites folding onto one identifier IS a
 * failure: one header would overwrite the other.
 *
 * @param skipped Appended to with a line per metasprite not written.
 * @param error   Set to a human-readable reason on failure.
 */
bool writeAll(const proj::Project& project, const std::filesystem::path& dir,
              std::vector<std::string>& skipped, std::string& error);

} // namespace codegen
