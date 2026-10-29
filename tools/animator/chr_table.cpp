#include "chr_table.hpp"

#include <toml++/toml.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <ios>
#include <system_error>
#include <utility>

namespace chr {
namespace {

/// One include file's bytes, resolved and read.
struct Include {
    std::string               name;    ///< Filename stem, as blobman derives its symbols.
    std::string               path;    ///< As written in the manifest, for messages.
    std::uint32_t             offset;  ///< Byte offset inside the entry.
    std::vector<std::uint8_t> data;
};

std::string hex(std::uint64_t value) {
    char text[32];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

/**
 * @brief Decodes @p tiles consecutive tiles starting at @p byteOffset into
 *        palette indices, row-major and kTilePixels wide.
 *
 * 2bpp planar: for tile row y, the low bit of every pixel comes from byte
 * base+y and the high bit from base+y+8, MSB first (bit 7 is the leftmost
 * pixel). Bytes past the end of @p data read as 0, so a short final tile is
 * zero-padded rather than dropped.
 */
std::vector<std::uint8_t> decodeTiles(const std::vector<std::uint8_t>& data,
                                      std::size_t byteOffset,
                                      int tiles) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kTilePixels) * kTilePixels * tiles);

    for (int tile = 0; tile < tiles; ++tile) {
        const std::size_t base = byteOffset + static_cast<std::size_t>(tile) * kTileBytes;
        for (int y = 0; y < kTilePixels; ++y) {
            const std::size_t loIndex = base + static_cast<std::size_t>(y);
            const std::size_t hiIndex = loIndex + kTilePixels;
            const std::uint8_t lo = loIndex < data.size() ? data[loIndex] : 0;
            const std::uint8_t hi = hiIndex < data.size() ? data[hiIndex] : 0;

            const std::size_t row = static_cast<std::size_t>(tile) * kTilePixels + y;
            for (int x = 0; x < kTilePixels; ++x) {
                const int shift = kTilePixels - 1 - x;
                pixels[row * kTilePixels + x] =
                    static_cast<std::uint8_t>(((lo >> shift) & 1) | (((hi >> shift) & 1) << 1));
            }
        }
    }

    return pixels;
}

/// Reads @p path, resolved against the manifest's directory and then the
/// process working directory -- see loadManifest's note on blobman.
bool readInclude(const std::filesystem::path& manifestDir,
                 const std::string& written,
                 std::vector<std::uint8_t>& out,
                 std::string& error) {
    const std::filesystem::path candidates[] = {
        manifestDir / written,
        std::filesystem::path(written),
    };

    for (const auto& candidate : candidates) {
        std::ifstream in(candidate, std::ios::binary | std::ios::ate);
        if (!in) continue;

        const std::streamoff size = in.tellg();
        if (size < 0) {
            error = "could not measure include file: " + candidate.string();
            return false;
        }
        out.resize(static_cast<std::size_t>(size));
        in.seekg(0, std::ios::beg);
        in.read(reinterpret_cast<char*>(out.data()), size);
        if (in.gcount() != size) {
            error = "failed reading include file: " + candidate.string();
            return false;
        }
        return true;
    }

    error = "include file not found: " + written + " (looked in " + manifestDir.string() +
            " and the working directory)";
    return false;
}

} // namespace

std::optional<Table> loadManifest(const std::filesystem::path& path, TileMode mode, std::string& error) {
    toml::table config;
    try {
        config = toml::parse_file(path.string());
    } catch (const toml::parse_error& err) {
        // description() is a string_view, and the source position is worth
        // keeping: it is the only pointer at WHERE the manifest is malformed.
        std::ostringstream message;
        message << "failed to parse: " << err.description() << " (" << err.source().begin << ")";
        error = message.str();
        return std::nullopt;
    }

    const auto manifest = config["manifest"].as_table();
    if (!manifest) {
        error = "requires a [manifest] table holding the blob metadata";
        return std::nullopt;
    }

    const std::int64_t blobSize = (*manifest)["size"].value_or<std::int64_t>(0);
    if (blobSize <= 0) {
        error = "blob size must be a non-zero positive integer";
        return std::nullopt;
    }

    Table table;
    table.path = path;
    table.mode = mode;
    table.blobSize = static_cast<std::uint32_t>(blobSize);

    std::filesystem::path manifestDir = path.parent_path();
    if (manifestDir.empty()) manifestDir = ".";

    // Entry -> its includes, built alongside table.entries so both can be
    // reordered into blob order together before any cell is emitted (blobman
    // sorts by LMA, and the generated header reads in that order).
    std::vector<std::vector<Include>> entryIncludes;

    for (auto&& [key, node] : config) {
        if (key.str() == "manifest") continue;  // metadata, not an entry

        const auto entryTable = node.as_table();
        if (!entryTable) {
            error = "top level may only contain entry tables, but '" + std::string(key.str()) +
                    "' is not one";
            return std::nullopt;
        }

        const std::string name(key.str());

        const std::int64_t lma = (*entryTable)["LMA"].value_or<std::int64_t>(-1);
        if (lma < 0) {
            error = "positive integer member 'LMA' missing from entry: " + name;
            return std::nullopt;
        }

        const std::int64_t vma = (*entryTable)["VMA"].value_or<std::int64_t>(-1);
        if (vma < 0) {
            error = "positive integer member 'VMA' missing from entry: " + name;
            return std::nullopt;
        }
        if (vma % kTileBytes) {
            error = "VMA for entry: " + name + " is not on a tile boundary";
            return std::nullopt;
        }

        const std::int64_t size = (*entryTable)["size"].value_or<std::int64_t>(-1);
        if (size <= 0) {
            error = "positive non-zero integer member 'size' missing from entry: " + name;
            return std::nullopt;
        }

        // Tile ids are a single byte, so an entry has to stay inside the one
        // pattern table its VMA lands in -- blobman rejects this too.
        if (vma / kPatternTableBytes != (vma + size - 1) / kPatternTableBytes) {
            error = "entry: " + name + " straddles a pattern table boundary at its VMA";
            return std::nullopt;
        }
        if (lma + size > blobSize) {
            error = "entry: " + name + " does not fit inside the blob";
            return std::nullopt;
        }

        Entry entry;
        entry.name = name;
        entry.lma  = static_cast<std::uint32_t>(lma);
        entry.vma  = static_cast<std::uint32_t>(vma);
        entry.size = static_cast<std::uint32_t>(size);

        std::vector<Include> includes;
        if (const auto array = (*entryTable)["includes"].as_array()) {
            std::uint32_t progress = 0;
            for (const auto& node2 : *array) {
                const auto written = node2.value<std::string>();
                if (!written) {
                    error = "all includes must be a filepath, in entry: " + name;
                    return std::nullopt;
                }

                std::vector<std::uint8_t> data;
                if (!readInclude(manifestDir, *written, data, error)) return std::nullopt;

                if (progress + data.size() > static_cast<std::size_t>(size)) {
                    error = "overflowed blob: " + name + " at include " + *written;
                    return std::nullopt;
                }
                // blobman rejects this because the include's symbol would not
                // name a whole tile; the same applies to a cell here.
                if (progress % kTileBytes) {
                    error = "include " + *written + " in entry: " + name +
                            " does not start on a tile boundary";
                    return std::nullopt;
                }
                if (data.size() % kTileBytes) {
                    table.warnings.push_back(*written + " is " + hex(data.size()) +
                                             " bytes -- not a whole number of tiles, the last is zero-padded");
                }

                includes.push_back({
                    std::filesystem::path(*written).stem().string(),
                    *written,
                    progress,
                    std::move(data),
                });
                progress += static_cast<std::uint32_t>(includes.back().data.size());
            }
            entry.filled = progress;
        }

        table.entries.push_back(std::move(entry));
        entryIncludes.push_back(std::move(includes));
    }

    if (table.entries.empty()) {
        error = "manifest declares no entries";
        return std::nullopt;
    }

    // toml tables iterate in key order, not source order, so sort into blob
    // order -- which also makes the overlap check below a neighbour test.
    std::vector<std::size_t> order(table.entries.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return table.entries[a].lma < table.entries[b].lma;
    });

    std::vector<Entry>                sortedEntries;
    std::vector<std::vector<Include>> sortedIncludes;
    sortedEntries.reserve(order.size());
    sortedIncludes.reserve(order.size());
    for (const std::size_t i : order) {
        sortedEntries.push_back(std::move(table.entries[i]));
        sortedIncludes.push_back(std::move(entryIncludes[i]));
    }
    table.entries = std::move(sortedEntries);
    entryIncludes = std::move(sortedIncludes);

    for (std::size_t n = 1; n < table.entries.size(); ++n) {
        const Entry& prev = table.entries[n - 1];
        if (prev.lma + prev.size > table.entries[n].lma) {
            error = "entry: " + table.entries[n].name + " overlaps entry: " + prev.name;
            return std::nullopt;
        }
    }

    // One cell per tile, or per even-aligned tile pair in 8x16 mode.
    for (std::size_t e = 0; e < table.entries.size(); ++e) {
        const Entry& entry = table.entries[e];

        for (const Include& include : entryIncludes[e]) {
            // Round up: a short final tile is shown zero-padded (warned above).
            const int tiles = static_cast<int>((include.data.size() + kTileBytes - 1) / kTileBytes);

            for (int tile = 0; tile < tiles;) {
                // The VMA is a PPU byte address, so it divides down into the
                // tile id the hardware sees inside whichever pattern table it
                // selects -- exactly how blobman derives its symbols.
                const std::uint32_t tileVma =
                    entry.vma + include.offset + static_cast<std::uint32_t>(tile) * kTileBytes;
                const int id = static_cast<int>((tileVma / kTileBytes) & 0xFF);

                // Which pattern table that VMA lands in. The mask above drops
                // it -- an id is an index WITHIN a table, which is what the
                // sheet shows and what blobman's symbols are -- so it is kept
                // here and folded into the pattern byte below.
                const int half = static_cast<int>(tileVma / kPatternTableBytes) & 1;

                // In 8x16 sprite mode the pattern byte's bit 0 selects the
                // pattern table and bits 7-1 select the pair, so a pair is
                // always an EVEN id followed by that id | 1. A pair can
                // therefore only start on an even tile, and an include whose
                // tiles do not pair up cleanly gets its odd tile shown on its
                // own rather than silently mispaired.
                const bool pairable = mode == TileMode::Size8x16 && (id % 2) == 0 && tile + 1 < tiles;
                const int height = pairable ? 2 : 1;

                if (mode == TileMode::Size8x16 && !pairable) {
                    table.warnings.push_back(
                        include.name + " tile " + std::to_string(tile) + " (id " + hex(static_cast<std::uint64_t>(id)) +
                        ") has no 8x16 partner" + ((id % 2) ? " -- it starts on an odd tile id" : " -- odd tile count") +
                        ", shown as 8x8");
                }

                Cell cell;
                cell.id             = id;
                // 8x16: bits 7-1 are the pair (always even, per the pairable
                // test) and bit 0 is the half, so the VMA survives into OAM.
                // 8x8: PPUCTRL picks the half, so the id goes out as-is.
                cell.patternByte    = height == 2 ? ((id & 0xFE) | half) : id;
                cell.entry          = e;
                cell.include        = include.name;
                cell.tileInInclude  = tile;
                cell.heightTiles    = height;
                cell.pixels         = decodeTiles(include.data,
                                                  static_cast<std::size_t>(tile) * kTileBytes,
                                                  height);
                table.cells.push_back(std::move(cell));

                tile += height;
            }
        }
    }

    return table;
}

} // namespace chr
