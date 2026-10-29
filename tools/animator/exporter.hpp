/**
 * @file exporter.hpp
 * @brief Headless project export: a whole .anim to generated headers.
 *
 * What File > Export All Headers does in the GUI, driven by argv instead of
 * dialogs -- the build's own way of keeping demo/animations/gen/ in step with
 * the .anim and its .msprite sidecars (see the animator-export step in the
 * root CMakeLists.txt), and CI's way of proving the committed headers still
 * match what the project says.
 *
 * Deliberately Qt-free -- plain std types only, like proj, chr and codegen:
 * every piece of the export pipeline already is, so this needs no window, no
 * display server and no Qt at all. That is what lets ::run be linked into a
 * standalone `animator-export` binary (exporter_main.cpp) that the Docker
 * build images can build without installing Qt, as well as being reachable
 * through the GUI binary's own --export-all switch.
 *
 * Pattern bytes are RE-RESOLVED against the blobman manifest here rather than
 * taken from the .msprite files: the manifest is what knows which
 * pattern-table half an entry's VMA banks the art into, so re-reading it is
 * what makes a CHR layout change show up in the generated headers at all. It
 * is also exactly what the GUI does on load (every placed sprite is rebuilt
 * from the manifest's cell), so a headless export and a File > Export All of
 * the same project write the same bytes.
 */
#pragma once

#include <filesystem>

namespace exporter {

/// Default output folder, relative to the project file's own directory --
/// where the GUI's export dialog starts too.
constexpr const char* kDefaultOutputDir = "gen";

/// What ::run was asked to do.
struct Options {
    /// The .anim to read. Required.
    std::filesystem::path project;

    /// Directory the headers are written into, created if missing. Empty means
    /// ::kDefaultOutputDir beside the project.
    std::filesystem::path output;

    /// Log every frame's pattern bytes, and every sprite whose stored byte the
    /// manifest disagreed with, rather than one line per header.
    bool verbose = false;
};

/// What ::parseArgs found in argv.
enum class Parse {
    NoExport,  ///< argv does not ask for an export: carry on with the GUI.
    Export,    ///< Run ::run with the filled-in Options.
    Help,      ///< Usage was asked for and printed; exit 0.
    Error,     ///< Usage was wrong and the reason printed; exit 1.
};

/**
 * @brief Pulls the export switches out of argv, printing usage problems.
 *
 *   -E, --export-all   export every metasprite of the project
 *   -i, --input        the .anim to read (or give it positionally)
 *   -o, --output       directory to write the headers into
 *   -v, --verbose      log every frame's pattern bytes
 *   -h, --help         print the usage and stop
 *
 * Shared by the GUI binary's own main (which must decide whether to open a
 * window before Qt ever touches argv) and the standalone exporter, so the two
 * take the same switches and spell the same mistakes the same way.
 *
 * @param exportImplied True for the standalone exporter, where exporting is
 *                      the only thing the binary does, so a project on its own
 *                      is enough and -E/--export-all is merely allowed.
 * @param extraUsage    Appended to the usage text, already laid out as lines:
 *                      switches of the calling binary that are none of this
 *                      module's business, so that --help from the GUI still
 *                      describes the GUI. Optional.
 */
Parse parseArgs(int argc, char** argv, Options& options, bool exportImplied,
                const char* extraUsage = nullptr);

/// The usage text ::parseArgs prints, on stdout. @p program names the binary,
/// @p extraUsage is the caller's own switches as above.
void usage(const char* program, const char* extraUsage = nullptr);

/**
 * @brief Exports every metasprite of @p options.project as a header.
 *
 * Logs what it is exporting as it goes -- the project, the manifest, and one
 * line per header with the sizes baked into it -- on stdout, with problems on
 * stderr. The log is the point: a configure-time export that said nothing
 * would be the one step of the build nobody could tell had gone stale.
 *
 * A metasprite with no frames is skipped and named, as codegen::writeAll has
 * it. A sprite naming art the manifest does not have is NOT skipped: it would
 * silently leave codegen::kNoTile in a slot that is meant to draw something,
 * so it fails the export instead -- the project and the CHR layout genuinely
 * disagree, and that is for a human to resolve.
 *
 * @return 0 on success, 1 on failure (with the reason already on stderr).
 */
int run(const Options& options);

} // namespace exporter
