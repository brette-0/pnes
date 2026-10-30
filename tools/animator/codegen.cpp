#include "codegen.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <ios>
#include <sstream>
#include <system_error>

namespace codegen {
namespace {

std::string hex2(int value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%02x", value & 0xFF);
    return text;
}

/// A frame's name as a C++ comment can hold anything; a newline in it would
/// end the comment early, so it does not get the chance.
std::string oneLine(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        out += (ch == '\n' || ch == '\r') ? ' ' : ch;
    }
    return out;
}

bool writeFile(const std::filesystem::path& path, const std::string& contents, std::string& error) {
    std::error_code ec;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            error = "could not create " + parent.string() + ": " + ec.message();
            return false;
        }
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        error = "could not open for writing: " + path.string();
        return false;
    }
    out << contents;
    out.close();
    if (!out) {
        error = "failed writing: " + path.string();
        return false;
    }
    return true;
}

/**
 * @brief One frame's slots as pattern bytes, row-major, kNoTile where empty.
 *
 * The editor stores sprites by slot in PLACEMENT order, which is draw order
 * and not slot order, so this is where they are put back into the rectangle
 * the generated data is. One sprite per slot is the field's own rule, so a
 * later sprite in the same slot can only mean a file edited by hand -- it
 * wins, which is what the field does when something is stamped twice.
 * A sprite outside the extent is dropped: the editor drops those on a resize,
 * and a hand-written .msprite is not a reason to emit out-of-bounds data.
 */
std::vector<int> slots(const proj::Frame& frame, int tilesX, int tilesY) {
    std::vector<int> tiles(static_cast<std::size_t>(tilesX) * static_cast<std::size_t>(tilesY),
                           kNoTile);
    for (const proj::Sprite& sprite : frame.sprites) {
        if (sprite.col < 0 || sprite.col >= tilesX || sprite.row < 0 || sprite.row >= tilesY) continue;
        tiles[static_cast<std::size_t>(sprite.row) * static_cast<std::size_t>(tilesX) +
              static_cast<std::size_t>(sprite.col)] = sprite.patternByte;
    }
    return tiles;
}

/// A frame's hold, as the timer's own range allows it. The editor clamps what
/// it takes, so this only bites on a hand-edited .msprite -- where emitting
/// something a byte of ticks cannot hold would be worse than clamping it.
int timerOf(const proj::Frame& frame) {
    return std::max(0, std::min(255, frame.timer));
}

/// Frame-index constant names, one per frame, in play order. Two frames may
/// share a name -- the editor does not stop that -- so a repeat (or a name
/// that folds to nothing) is numbered instead of silently redeclaring.
std::vector<std::string> frameConstants(const proj::Metasprite& metasprite) {
    std::vector<std::string> names;
    names.reserve(metasprite.frames.size());

    for (std::size_t i = 0; i < metasprite.frames.size(); ++i) {
        std::string candidate = identifier(metasprite.frames[i].name);
        const bool taken = std::find(names.begin(), names.end(), candidate) != names.end();
        if (taken || candidate == "_") {
            candidate += "_" + std::to_string(i);
        }
        names.push_back(std::move(candidate));
    }
    return names;
}

} // namespace

std::string identifier(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char ch : name) {
        out += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
    }
    if (out.empty()) out = "_";
    if (std::isdigit(static_cast<unsigned char>(out.front()))) out.insert(out.begin(), '_');
    return out;
}

std::string header(const proj::Metasprite& metasprite, const std::string& source) {
    if (metasprite.frames.empty()) return {};

    const std::string name = identifier(metasprite.name);
    const int tilesX = std::max(1, metasprite.tilesX);
    const int tilesY = std::max(1, metasprite.tilesY);
    const std::size_t size = static_cast<std::size_t>(tilesX) * static_cast<std::size_t>(tilesY);
    const std::vector<std::string> constants = frameConstants(metasprite);
    const bool anyPublic = std::any_of(metasprite.frames.begin(), metasprite.frames.end(),
                                       [](const proj::Frame& frame) { return frame.isPublic; });

    std::ostringstream out;
    // Two lines rather than one: the metasprite's name and the file it came
    // from are both as long as someone made them, and a banner that wraps
    // wherever the names happen to end is harder to read than a fixed one.
    out << "#pragma once\n\n"
           "// Generated by the 'pnes' Animation Designer -- do not edit by hand.\n"
        << "// Metasprite \"" << oneLine(metasprite.name) << "\"";
    if (!source.empty()) out << ", of " << oneLine(source);
    out << ".\n"
           "//\n"
           "// Pattern bytes only: one row below per animation frame, holding this\n"
           "// metasprite's slots row-major, each byte ready for OAM as it stands. An 8x16\n"
           "// slot's bit 0 is the pattern-table half the blobman entry's VMA banks the art\n"
           "// into, folded in here because the manifest is the only thing that knows it;\n"
           "// an 8x8 slot has no such bit, PPUCTRL picking the half instead. The palette\n"
           "// and the mirroring are deliberately not here. A slot nothing was stamped into\n"
           "// holds ::kNoTile.\n"
           "\n"
           "#include <intsh>\n"
           "#include <pnes/types.hpp>\n"
           "\n"
           "using namespace br0::intsh;\n"
           "\n"
        << "namespace " << kNamespace << " {\n\n";

    out << "class " << name << " {\n"
           "public:\n"
           "    // Tables, not an object: there is nothing to construct.\n"
        << "    " << name << "() = delete;\n\n"
           "    /// Extent in tiles. A slot is one tile -- 8x8 or 8x16, whichever sprite\n"
           "    /// height is selected when it is drawn.\n"
        << "    static constexpr u8  kWidth  = " << tilesX << ";\n"
        << "    static constexpr u8  kHeight = " << tilesY << ";\n\n"
           "    /// Slots one frame holds -- the metasprite's total size, which is also how\n"
           "    /// many hardware sprites drawing it takes.\n"
        << "    static constexpr u16 kSize   = kWidth * kHeight;\n\n"
           "    /// Frames, in play order: the last one loops back to the first.\n"
        << "    static constexpr u8  kFrames = " << metasprite.frames.size() << ";\n\n"
           "    /// Bytes the whole animation occupies.\n"
           "    static constexpr u16 kBytes  = kFrames * kSize;\n\n"
           "    /// A slot with no sprite in it. A legal pattern byte, so this is a\n"
           "    /// convention of this data and not of the PPU: skip the slot rather than\n"
           "    /// write it to OAM.\n"
        << "    static constexpr u8  kNoTile = " << hex2(kNoTile) << ";\n\n"
           "    /// Frame indices, named after the frames themselves.\n";
    for (std::size_t i = 0; i < constants.size(); ++i) {
        out << "    static constexpr u8  kFrame_" << constants[i] << " = " << i << ";\n";
    }

    // The timers are frame data like the tiles are -- the playback convention
    // is counted down in NES frames, and 0 is what ends an animation -- so they
    // are emitted beside them rather than left for a consumer to repeat by
    // hand. They are their own table: nothing walking the tiles wants a timer
    // byte in the middle of a frame.
    out << "\n    /// NES frames each frame holds. A timer of 0 ends the animation on that\n"
           "    /// frame instead of advancing, so a one-frame animation with timer 0 is a\n"
           "    /// still pose.\n"
           "    static constexpr u8 kTimers[kFrames] = {";
    for (std::size_t i = 0; i < metasprite.frames.size(); ++i) {
        out << (i ? ", " : " ") << timerOf(metasprite.frames[i]);
    }
    out << " };\n\n";

    out << "    /// The animation's first frame. Frames are contiguous, so the frame after\n"
           "    /// @p frame starts at Get(frame) + kSize.\n"
           "    static constexpr const u8* Get() { return &kTiles[0][0]; }\n\n"
           "    /// Frame @p frame's slots, row-major.\n"
           "    static constexpr const u8* Get(const u8 frame) { return &kTiles[frame][0]; }\n\n"
           "private:\n"
           "    static constexpr u8 kTiles[kFrames][kSize] = {\n";

    for (std::size_t f = 0; f < metasprite.frames.size(); ++f) {
        const proj::Frame& frame = metasprite.frames[f];
        const std::vector<int> tiles = slots(frame, tilesX, tilesY);

        const int timer = timerOf(frame);
        out << "        // " << f << ": \"" << oneLine(frame.name) << "\", timer " << timer
            << (timer == 0 ? " -- the animation stops here" : "")
            << (frame.isPublic ? " -- public, named below" : "") << "\n";

        // A single-row metasprite is one line; taller ones get a line per row,
        // so the braces read like the field the sprites were stamped on.
        if (tilesY == 1) {
            out << "        {";
            for (std::size_t i = 0; i < size; ++i) out << (i ? ", " : " ") << hex2(tiles[i]);
            out << " },\n";
            continue;
        }

        out << "        {\n";
        for (int row = 0; row < tilesY; ++row) {
            out << "           ";
            for (int col = 0; col < tilesX; ++col) {
                out << " " << hex2(tiles[static_cast<std::size_t>(row) *
                                         static_cast<std::size_t>(tilesX) +
                                     static_cast<std::size_t>(col)]) << ",";
            }
            out << "  // row " << row << "\n";
        }
        out << "        },\n";
    }

    out << "    };\n";

    // The public frames come after the table because they NAME rows of it: a
    // reference has to be initialised with something already declared, and the
    // sizes above have to be declared before the table that is sized by them.
    // Two public sections is the price of saying both in one class.
    if (anyPublic) {
        out << "\npublic:\n"
               "    /// Frames marked public in the designer. The table stays private --\n"
               "    /// each of these is a reference to one of its rows, so it is the same\n"
               "    /// bytes under a name of their own and costs nothing. For an animation\n"
               "    /// no timer drives -- a still pose, or a frame picked by game state --\n"
               "    /// that name is the whole interface, and Get() need never be called.\n";
        for (std::size_t i = 0; i < metasprite.frames.size(); ++i) {
            if (!metasprite.frames[i].isPublic) continue;
            out << "    static constexpr const u8 (&kTiles_" << constants[i] << ")[kSize] = kTiles["
                << "kFrame_" << constants[i] << "];\n";
        }
    }

    out << "};\n\n"
        << "} // namespace " << kNamespace << "\n";
    return out.str();
}

bool writeHeader(const proj::Metasprite& metasprite, const std::filesystem::path& file,
                 const std::string& source, std::string& error) {
    const std::string contents = header(metasprite, source);
    if (contents.empty()) {
        error = "'" + metasprite.name + "' has no frames -- there is no animation to export";
        return false;
    }
    return writeFile(file, contents, error);
}

bool writeAll(const proj::Project& project, const std::filesystem::path& dir,
              std::vector<std::string>& skipped, std::string& error) {
    // The identifier is both the filename and the class name, so a collision
    // is two headers trying to be one file -- refused rather than letting the
    // second overwrite the first, exactly as proj::save refuses it for the
    // .msprite stems.
    std::vector<std::string> names;
    names.reserve(project.metasprites.size());
    for (const proj::Metasprite& metasprite : project.metasprites) {
        const std::string name = identifier(metasprite.name);
        if (std::find(names.begin(), names.end(), name) != names.end()) {
            error = "two metasprites both export as '" + name + kHeaderExtension +
                    "' -- rename one of them";
            return false;
        }
        names.push_back(name);
    }

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        error = "could not create " + dir.string() + ": " + ec.message();
        return false;
    }

    for (std::size_t i = 0; i < project.metasprites.size(); ++i) {
        const proj::Metasprite& metasprite = project.metasprites[i];
        if (metasprite.frames.empty()) {
            skipped.push_back("'" + metasprite.name + "' has no frames -- not exported");
            continue;
        }

        const std::filesystem::path file = dir / (names[i] + kHeaderExtension);
        if (!writeFile(file, header(metasprite, project.path.filename().string()), error)) return false;
    }

    return true;
}

} // namespace codegen
