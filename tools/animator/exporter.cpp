#include "exporter.hpp"

#include "chr_table.hpp"
#include "codegen.hpp"
#include "project.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace exporter {
namespace {

/// Everything this prints is prefixed with the tool's name: a configure-time
/// export's output lands in the middle of CMake's own log, where an unlabelled
/// line is nobody's.
constexpr const char* kTag = "animator";

void info(const std::string& line) {
    std::fprintf(stdout, "%s: %s\n", kTag, line.c_str());
}

void problem(const std::string& line) {
    std::fprintf(stderr, "%s: %s\n", kTag, line.c_str());
}

std::string hex2(int value) {
    char text[8];
    std::snprintf(text, sizeof(text), "%02x", value & 0xFF);
    return text;
}

/// "1 frame" / "2 frames", because a log that says "1 frames" reads like a bug
/// in the thing being diagnosed. @p many is for the nouns an 's' does not fix.
std::string plural(std::size_t count, const std::string& noun, const std::string& many = {}) {
    if (count == 1) return "1 " + noun;
    return std::to_string(count) + " " + (many.empty() ? noun + "s" : many);
}

const char* modeName(const chr::TileMode mode) {
    return mode == chr::TileMode::Size8x16 ? "8x16" : "8x8";
}

/// One frame's pattern bytes in slot order, as the generated header will hold
/// them -- the whole point of the verbose log, since this is the data the
/// console actually reads and the one thing a stale header gets wrong.
std::string frameBytes(const proj::Frame& frame, int tilesX, int tilesY) {
    std::vector<int> slots(static_cast<std::size_t>(tilesX) * static_cast<std::size_t>(tilesY),
                           codegen::kNoTile);
    for (const proj::Sprite& sprite : frame.sprites) {
        if (sprite.col < 0 || sprite.col >= tilesX || sprite.row < 0 || sprite.row >= tilesY) continue;
        slots[static_cast<std::size_t>(sprite.row) * static_cast<std::size_t>(tilesX) +
              static_cast<std::size_t>(sprite.col)] = sprite.patternByte;
    }

    std::string out;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (i != 0) out += ' ';
        out += hex2(slots[i]);
    }
    return out;
}

} // namespace

void usage(const char* program, const char* extraUsage) {
    std::fprintf(stdout,
                 "usage: %s [-E] [-i <project.anim>] [-o <dir>] [-v] [<project.anim>]\n"
                 "\n"
                 "  -E, --export-all   export every metasprite of the project as a header\n"
                 "  -i, --input   <f>  the .anim to read; may also be given positionally\n"
                 "  -o, --output  <d>  directory to write the headers into\n"
                 "                     (default: '%s' beside the project)\n"
                 "  -v, --verbose      log every frame's pattern bytes\n"
                 "  -h, --help         print this and stop\n",
                 program, kDefaultOutputDir);
    if (extraUsage) std::fputs(extraUsage, stdout);
}

Parse parseArgs(int argc, char** argv, Options& options, const bool exportImplied,
                const char* extraUsage) {
    const char* program = argc > 0 ? argv[0] : kTag;
    bool wantsExport = exportImplied;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool wantsValue = arg == "-i" || arg == "--input" || arg == "-o" || arg == "--output";
        if (wantsValue && i + 1 >= argc) {
            problem(arg + " requires an argument");
            return Parse::Error;
        }

        if (arg == "-h" || arg == "--help") {
            usage(program, extraUsage);
            return Parse::Help;
        } else if (arg == "-E" || arg == "--export-all") {
            wantsExport = true;
        } else if (arg == "-i" || arg == "--input") {
            options.project = argv[++i];
        } else if (arg == "-o" || arg == "--output") {
            options.output = argv[++i];
        } else if (arg == "-v" || arg == "--verbose") {
            options.verbose = true;
        } else if (!arg.empty() && arg.front() == '-') {
            // Only in export mode: the GUI's own main has its own switches
            // (-t/--tall), and this parse runs ahead of it rather than
            // instead of it.
            if (!wantsExport) return Parse::NoExport;
            problem("unknown option '" + arg + "'");
            usage(program, extraUsage);
            return Parse::Error;
        } else if (options.project.empty()) {
            options.project = arg;
        } else if (wantsExport) {
            problem("unexpected argument '" + arg + "' -- one project at a time");
            return Parse::Error;
        }
    }

    if (!wantsExport) return Parse::NoExport;
    if (options.project.empty()) {
        problem("no project to export -- give an .anim, with -i/--input or positionally");
        usage(program, extraUsage);
        return Parse::Error;
    }
    return Parse::Export;
}

int run(const Options& options) {
    if (options.project.empty()) {
        problem("no project given -- nothing to export");
        return 1;
    }

    std::string error;
    auto project = proj::load(options.project, error);
    if (!project) {
        problem("cannot read " + options.project.string() + ": " + error);
        return 1;
    }
    for (const std::string& warning : project->warnings) {
        problem("warning: " + warning);
    }

    const std::filesystem::path output =
        options.output.empty() ? project->path.parent_path() / kDefaultOutputDir : options.output;
    // Normalized only for the log and for what is reported on failure: the
    // manifest path is a relative one joined onto the project's directory, so
    // it reads as "demo/animations/../chr/config.toml" otherwise. Purely
    // lexical, so it needs no filesystem and cannot fail.
    const std::filesystem::path manifest =
        proj::resolve(*project, project->chrManifest).lexically_normal();

    info("project " + options.project.string() + ": " +
         plural(project->metasprites.size(), "metasprite") + ", " +
         plural(project->palettes.size(), "palette") + ", " + modeName(project->mode) + " tiles");
    info("  manifest " + project->chrManifest + " -> " + manifest.string());
    info("  output   " + output.string());

    // The manifest is not optional here, even though every sprite already
    // carries a pattern byte from its .msprite: that byte is only as current
    // as the last save, and the CHR layout it came from may have moved since
    // (a new include, a changed VMA). Re-resolving is what makes a blobman
    // edit reach the generated headers.
    auto table = chr::loadManifest(manifest, project->mode, error);
    if (!table) {
        problem("cannot read the CHR manifest " + manifest.string() + ": " + error);
        return 1;
    }
    // The sheet's own warnings are about the CHR layout, not about anything
    // being exported -- an 8x16 project reading a manifest with odd-sized
    // background entries in it has a couple of dozen, every one of them
    // expected and none of them this export's business. Counted here and
    // listed under -v, rather than spilled into every configure's log.
    std::string sheet = "  sheet    " + plural(table->entries.size(), "entry", "entries") + ", " +
                        plural(table->cells.size(), "cell") + ", blob " +
                        std::to_string(table->blobSize) + " bytes";
    if (!table->warnings.empty()) {
        sheet += " (" + plural(table->warnings.size(), "cell warning") +
                 (options.verbose ? ")" : " -- -v lists them)");
    }
    info(sheet);
    if (options.verbose) {
        for (const std::string& warning : table->warnings) {
            info("    " + manifest.filename().string() + ": " + warning);
        }
    }

    std::size_t unresolved = 0;
    std::size_t repaired = 0;
    for (proj::Metasprite& metasprite : project->metasprites) {
        for (std::size_t f = 0; f < metasprite.frames.size(); ++f) {
            for (proj::Sprite& sprite : metasprite.frames[f].sprites) {
                const chr::Cell* cell =
                    proj::findCell(*table, sprite.entry, sprite.include, sprite.tile, sprite.id);
                if (!cell) {
                    ++unresolved;
                    problem("error: " + metasprite.name + " frame " + std::to_string(f) + " slot (" +
                            std::to_string(sprite.col) + "," + std::to_string(sprite.row) +
                            ") names art this manifest does not have: entry '" + sprite.entry +
                            "', include '" + sprite.include + "', tile " + std::to_string(sprite.tile));
                    continue;
                }
                if (cell->patternByte != sprite.patternByte) {
                    // A stored byte equal to the stored id is what a .msprite
                    // written before the pattern key existed reads back as
                    // (proj::load falls back to the id), so it says nothing
                    // about the CHR layout having moved -- it only means the
                    // project has not been saved since. Noted under -v, but
                    // kept out of the count, or every build of a project
                    // nobody has re-saved would carry a line about it.
                    const bool legacy = sprite.patternByte == sprite.id;
                    if (!legacy) ++repaired;
                    if (options.verbose) {
                        info(std::string("  note: ") + metasprite.name + " frame " +
                             std::to_string(f) + " slot (" + std::to_string(sprite.col) + "," +
                             std::to_string(sprite.row) + ") stored pattern $" +
                             hex2(sprite.patternByte) + ", manifest says $" +
                             hex2(cell->patternByte) + " -- exporting the manifest's" +
                             (legacy ? " (the .msprite predates the pattern key)" : ""));
                    }
                }
                sprite.id          = cell->id;
                sprite.patternByte = cell->patternByte;
            }
        }
    }
    if (unresolved != 0) {
        problem("cannot export " + options.project.string() + ": " +
                plural(unresolved, "sprite") + " name art " + manifest.filename().string() +
                " does not have (listed above). Exporting them would leave a hole where a "
                "sprite is meant to be, so the project and the CHR layout have to be put back "
                "in step first.");
        return 1;
    }
    if (repaired != 0 && !options.verbose) {
        info("  " + plural(repaired, "sprite") +
             " had a stored pattern byte the manifest disagreed with; the manifest's was "
             "exported (-v lists them)");
    }

    // What each header will hold, logged BEFORE the write: if the write then
    // fails, the log already says which header it was on.
    std::size_t bytes = 0;
    std::size_t headers = 0;
    for (const proj::Metasprite& metasprite : project->metasprites) {
        const std::string file = codegen::identifier(metasprite.name) + codegen::kHeaderExtension;
        const int tilesX = metasprite.tilesX > 0 ? metasprite.tilesX : 1;
        const int tilesY = metasprite.tilesY > 0 ? metasprite.tilesY : 1;
        const std::size_t slots = static_cast<std::size_t>(tilesX) * static_cast<std::size_t>(tilesY);

        if (metasprite.frames.empty()) {
            info("  " + file + ": no frames -- skipped");
            continue;
        }

        ++headers;
        bytes += slots * metasprite.frames.size();
        std::string line = "  " + file + ": " + std::to_string(tilesX) + "x" + std::to_string(tilesY) +
                           " tiles, " + plural(slots, "slot") + ", " +
                           plural(metasprite.frames.size(), "frame") + ", " +
                           std::to_string(slots * metasprite.frames.size()) + " bytes";
        if (!metasprite.palette.empty()) line += ", palette '" + metasprite.palette + "'";
        info(line);

        if (!options.verbose) continue;
        for (std::size_t f = 0; f < metasprite.frames.size(); ++f) {
            const proj::Frame& frame = metasprite.frames[f];
            info("    frame " + std::to_string(f) + " \"" + frame.name + "\" timer " +
                 std::to_string(frame.timer) + (frame.isPublic ? " public" : "") + ": " +
                 frameBytes(frame, tilesX, tilesY));
        }
    }

    std::vector<std::string> skipped;
    if (!codegen::writeAll(*project, output, skipped, error)) {
        problem("export failed: " + error);
        return 1;
    }
    // codegen::writeAll's own skip list, not the frameless ones already logged
    // above -- kept separate so a reason this tool does not know about still
    // reaches the log.
    for (const std::string& line : skipped) {
        problem("warning: " + line);
    }

    info("exported " + plural(headers, "header") + ", " + std::to_string(bytes) +
         " bytes of frame data, into " + output.string());
    return 0;
}

} // namespace exporter
