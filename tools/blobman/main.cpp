#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>
#include <toml++/toml.h>

#ifdef _WIN32
    #include <io.h>
    #define BLOBMAN_ACCESS  _access
    #define BLOBMAN_W_OK    2
#else
    #include <unistd.h>
    #define BLOBMAN_ACCESS  access
    #define BLOBMAN_W_OK    W_OK
#endif

namespace {
    // one include file packed into an entry, kept for the generated header
    struct blobInclude {
        std::string name;       // symbol taken from the include filename
        std::string path;       // source path, kept for conflict reporting
        int         offset{};   // byte offset inside the entry
        int         size{};     // bytes taken from the file
    };

    struct entry {
        std::string_view            name;         // must be a real string
        int                         LMA{};        // pos in chr rom
        int                         VMA{};        // used for rt accessing
        int                         size{};       // size of entry
        char*                       data{};       // temp of buffer data
        std::string_view*           includes{};   // includes for tile naming
        std::vector<blobInclude>    files{};      // resolved includes, in pack order
    };

    enum eCommand {
        Bad,
        Help,
        Input,
        Output,
        Verbose,
        SetIncludeLang,
        End
    };

    enum eLanguage {
        noLang,
        ca65,
        c
    };

    // chr tiles are 16 bytes, so a byte offset divides down into a tile index
    constexpr int tileSize = 16;

    // a pattern table holds 256 tiles, which is the range of a tile index
    constexpr int patternTableSize = tileSize * 256;
}

static toml::table              config;
static bool                     verbose;
static char*                    buffer;
static size_t                   bufferSize;
static bool                     ProcessConfig();
static bool                     WriteBlob();
static entry*                   entries;
static size_t                   entryCount;
static std::filesystem::path    outputFile;
static std::filesystem::path    headerFile;
static const char*              manifestPath;

static eCommand     GetCommand(const char* token);
static void         ShowHelpMessage();
static eLanguage    GetIncludeLang(const char* token);
static bool         ResolveWriteTarget(const char* token, const char* label,
                                       std::filesystem::path& dest);
static bool         GenerateHeader();

int main(const int argc, char *argv[]) {
    if (argc < 2) {
        std::cout << "blobman requires a input toml file and an output file\n";
        ShowHelpMessage();
        return -1;
    }

    eLanguage includeLang = noLang;
    for (int i = 1; i < argc; i++) {
        switch (GetCommand(argv[i])) {
            case Bad:
                std::cout << "Expected valid command, but got: " << argv[i] << '\n';
                return -1;

            case Help:
                ShowHelpMessage();
                return 0;

            case Input:
                if (++i >= argc) {
                    std::cout << "input command must be followed by a filepath\n";
                    return -1;
                }

                if (manifestPath) {
                    std::cout << "input manifest specified more than once\n";
                    return -1;
                }

                manifestPath = argv[i];
                try {
                    config = toml::parse_file(manifestPath);
                } catch (const toml::parse_error& err) {
                    std::cerr << "failed to parse " << manifestPath << ":\n"
                              << err.description() << "\n  (" << err.source().begin << ")\n";
                    return -1;
                }

                break;

            case Output:
                if (++i >= argc) {
                    std::cout << "output command must be followed by a filepath\n";
                    return -1;
                }

                if (!outputFile.empty()) {
                    std::cout << "output file specified more than once\n";
                    return -1;
                }

                if (!ResolveWriteTarget(argv[i], "output file", outputFile)) return -1;
                break;

            case End:
                std::cout << "GetCommand returned enum 'END' this should not be possible.\n";
                return -1;

            case Verbose:
                verbose = true;
                break;

            case SetIncludeLang:
                if (++i >= argc) {
                    std::cout << "language command must be followed by a language specifier\n";
                    return -1;
                }

                if (includeLang = GetIncludeLang(argv[i]); includeLang == noLang) {
                    std::cout << "bad include language specifier\n";
                    return -1;
                }
                break;
        }
    }

    if (includeLang == noLang) {
        std::cout << "requires at least one language for exporting\n";
        return -1;
    }

    if (!manifestPath) {
        std::cout << "no input manifest provided, see --help\n";
        return -1;
    }

    // the manifest is only walked once the whole command line is known, so
    // flags like -v apply no matter where they sit in argv
    if (!ProcessConfig()) return -1;

    if (entries == nullptr || entryCount == 0) {
        std::cout << "manifest declares no entries: " << manifestPath << '\n';
        return -1;
    }

    if (outputFile.empty()) {
        std::cout << "no output file provided, see --help\n";
        return -1;
    }

    // the header always sits beside the blob, same stem
    std::filesystem::path derived = outputFile;
    derived.replace_extension(".h");
    if (!ResolveWriteTarget(derived.string().c_str(), "generated header", headerFile)) return -1;

    if (std::filesystem::path(outputFile).lexically_normal() ==
        std::filesystem::path(headerFile).lexically_normal()) {
        std::cout << "blob and generated header resolve to the same file: "
                  << outputFile.string() << '\n';
        return -1;
    }

    if (!WriteBlob()) return -1;

    switch (includeLang) {
        case c:
            if (!GenerateHeader()) return -1;
            break;

        case ca65:
            std::cout << "ca65 include generation is not implemented yet\n";
            return -1;

        case noLang:
            break;
    }

    std::cout << "All jobs done\n";
    return 0;
}

void ShowHelpMessage() {
    std::cout <<
        "blobman -- packs files into fixed-size binary blobs from a toml manifest\n"
        "\n"
        "usage: blobman -i <manifest.toml> -o <blob.bin> -l <lang> [-v]\n"
        "\n"
        "  -h  --help     prints this help message\n"
        "  -i  --input    pass input toml file for processing\n"
        "  -o  --output   output blob file, created or completely replaced\n"
        "                 (the housing directory must already exist)\n"
        "  -l  --lang     language used for generated includes, written beside the\n"
        "                 blob with a language appropriate extension\n"
        "  -v  --verbose  enable verbose mode\n";
}

static const char* shortFormMatches[] = {
    "-h",   // help
    "-i",   // input
    "-o",   // output
    "-v",   // verbose
    "-l"    // set include language
};

static const char* longFormMatches[] = {
    "--help",       // help
    "--input",      // input
    "--output",     // output
    "--verbose",    // verbose
    "--lang"        // set include language
};

// the match tables are indexed from the first real command, not from Bad
static constexpr int commandMatchCount = End - Help;
static_assert(std::size(shortFormMatches) == commandMatchCount);
static_assert(std::size(longFormMatches)  == commandMatchCount);

eCommand GetCommand(const char* token) {
    for (int i = 0; i < commandMatchCount; i++) {
        if (strcmp(token, shortFormMatches[i]) == 0 ||
            strcmp(token, longFormMatches[i])  == 0) {
            return static_cast<eCommand>(i + Help);
        }
    }
    return Bad;
}

// a write target names a single file that is created or replaced outright, but
// the directory holding it has to exist already.
bool ResolveWriteTarget(const char* token, const char* label, std::filesystem::path& dest) {
    if (token[0] == '\0') {
        std::cout << label << " path is empty\n";
        return false;
    }

    const std::filesystem::path out = token;
    if (!out.has_filename()) {
        std::cout << label << ": " << token << " does not name a file\n";
        return false;
    }

    std::filesystem::path parent = out.parent_path();
    if (parent.empty()) parent = ".";

    std::error_code ec;
    const auto parentSt = std::filesystem::status(parent, ec);
    if (ec || parentSt.type() == std::filesystem::file_type::not_found) {
        std::cout << label << " directory does not exist: " << parent.string();
        if (ec) std::cout << ": " << ec.message();
        std::cout << '\n';
        return false;
    }

    if (!std::filesystem::is_directory(parentSt)) {
        std::cout << label << " directory: " << parent.string() << " is not a directory\n";
        return false;
    }

    const auto outSt = std::filesystem::status(out, ec);
    if (ec && outSt.type() != std::filesystem::file_type::not_found) {
        std::cout << label << ": " << out.string() << ": " << ec.message() << '\n';
        return false;
    }

    switch (outSt.type()) {
        case std::filesystem::file_type::not_found:
            // nothing to replace, only the housing directory has to be writable
            if (BLOBMAN_ACCESS(parent.string().c_str(), BLOBMAN_W_OK) != 0) {
                std::cout << "cannot write into " << label << " directory: " << parent.string()
                          << ": " << std::strerror(errno) << '\n';
                return false;
            }
            break;

        case std::filesystem::file_type::regular:
            if (BLOBMAN_ACCESS(out.string().c_str(), BLOBMAN_W_OK) != 0) {
                std::cout << "cannot replace " << label << ": " << out.string()
                          << ": " << std::strerror(errno) << '\n';
                return false;
            }
            break;

        case std::filesystem::file_type::directory:
            std::cout << label << ": " << out.string() << " is a directory, not a file\n";
            return false;

        default:
            std::cout << label << ": " << out.string()
                      << " is not a regular file, refusing to replace it\n";
            return false;
    }

    dest = out;
    return true;
}

bool ProcessConfig() {
    // toml tables iterate in key order, not source order, so the manifest is
    // looked up by name rather than taken off the front of the table
    const auto manifest = config["manifest"].as_table();
    if (!manifest) {
        std::cout << "requires a [manifest] table holding the blob metadata\n";
        return false;
    }

    const int sBuffer = (*manifest)["size"].value_or(0);
    if (sBuffer <= 0) {
        std::cout << "blob size must be a non-zero positive integer\n";
        return false;
    }

    bufferSize = static_cast<size_t>(sBuffer);
    buffer = new char[bufferSize]{};    // gaps between entries stay zeroed

    entries = new entry[config.size()];

    int i = 0;
    for (auto&& [key, node] : config) {
        if (key.str() == "manifest") continue;  // metadata, not an entry

        const auto& entry = node.as_table();
        if (!entry) {
            std::cout << "Top level may only contain entry tables\n";
            return false;
        }
        entries[i].name = key.str();

        const int LMA = (*entry)["LMA"].value_or(-1);
        if (LMA < 0) {
            std::cout << "positive integer member 'LMA' missing from entry: " << key.str() << '\n';
            return false;
        }
        entries[i].LMA = LMA;


        const int VMA = (*entry)["VMA"].value_or(-1);
        if (VMA < 0) {
            std::cout << "positive integer member 'VMA' missing from entry: " << key.str() << '\n';
            return false;
        }
        if (VMA % tileSize) {
            std::cout << "VMA for entry: " << key.str() << " is not on a tile boundary\n";
            return false;
        }
        entries[i].VMA = VMA;

        const int size = (*entry)["size"].value_or(-1);
        if (size <= 0) {
            std::cout << "positive non-zero integer member 'size' missing from entry: " << key.str() << '\n';
            return false;
        }

        // tile indices are a byte, so an entry has to stay inside the one
        // pattern table its VMA lands in
        if (VMA / patternTableSize != (VMA + size - 1) / patternTableSize) {
            std::cout << "entry: " << key.str()
                      << " straddles a pattern table boundary at its VMA\n";
            return false;
        }
        if (static_cast<size_t>(LMA) + static_cast<size_t>(size) > bufferSize) {
            std::cout << "entry: " << key.str() << " does not fit inside the blob\n";
            return false;
        }
        entries[i].size = size;
        entries[i].data = new char[size]{};   // ready buffer

        if (const auto includes = (*entry)["includes"].as_array()) {
            size_t blobProgress = 0;
            for (const auto& include : *includes) {
                const auto path = include.value<std::string>();
                if (!path) {
                    std::cout << "all includes must be a filepath, in entry: " << key.str() << '\n';
                    return false;
                }
                auto includeFile = std::ifstream(*path, std::ios::binary);
                if (!includeFile) {
                    std::cout << "include file not found: " << *path << '\n';
                    return false;
                }

                includeFile.seekg(0, std::ios::end);
                const std::streamoff includeSize = includeFile.tellg();
                if (includeSize < 0) {
                    std::cout << "could not measure include file: " << *path << '\n';
                    return false;
                }

                const size_t begin = blobProgress;
                if (begin + static_cast<size_t>(includeSize) > static_cast<size_t>(size)) {
                    std::cout << "overflowed blob: " << entries[i].name << " at include " << *path << '\n';
                    return false;
                }

                includeFile.seekg(0, std::ios::beg);
                includeFile.read(entries[i].data + begin, includeSize);
                if (includeFile.gcount() != includeSize) {
                    std::cout << "failed reading include file: " << *path << '\n';
                    return false;
                }

                entries[i].files.push_back({
                    std::filesystem::path(*path).stem().string(),
                    *path,
                    static_cast<int>(begin),
                    static_cast<int>(includeSize)
                });

                blobProgress += static_cast<size_t>(includeSize);
            }

            if (verbose) {
                std::cout << "entry: " << entries[i].name << " filled " << blobProgress
                          << " of " << size << " bytes\n";
            }
        } else if (verbose) {
            std::cout << "No includes for entry: " << key.str() << '\n';
        }

        memcpy(buffer + LMA, entries[i].data, entries[i].size);
        i++;
    }

    entryCount = static_cast<size_t>(i);

    // sort into rom order so the overlap check only has to look at neighbours,
    // and so the generated include reads in the same order as the blob
    std::sort(entries, entries + entryCount,
              [](const entry& a, const entry& b) { return a.LMA < b.LMA; });

    for (size_t n = 1; n < entryCount; n++) {
        const entry& prev = entries[n - 1];
        if (prev.LMA + prev.size > entries[n].LMA) {
            std::cout << "entry: " << entries[n].name << " overlaps entry: "
                      << prev.name << '\n';
            return false;
        }
    }

    return true;
}

// the whole buffer lands in one file, so the entries only get name checked here
// for the include generators that read them back out
bool WriteBlob() {
    for (size_t i = 0; i < entryCount; i++) {
        const entry& e = entries[i];

        // entry names come straight from toml keys, so keep them usable as symbols
        const std::filesystem::path name = std::string(e.name);
        if (name.empty() || name.has_parent_path() || !name.has_filename()) {
            std::cout << "entry name is not a usable identifier: " << e.name << '\n';
            return false;
        }
    }

    auto outFile = std::ofstream(outputFile, std::ios::binary | std::ios::trunc);
    if (!outFile) {
        std::cout << "could not open output file: " << outputFile.string()
                  << ": " << std::strerror(errno) << '\n';
        return false;
    }

    outFile.write(buffer, static_cast<std::streamsize>(bufferSize));
    outFile.close();
    if (!outFile) {
        std::cout << "failed writing output file: " << outputFile.string() << '\n';
        return false;
    }

    if (verbose) {
        std::cout << "written file: " << outputFile.string()
                  << " (" << bufferSize << " bytes, " << entryCount << " entries)\n";
    }

    return true;
}

// toml keys and include filenames are free form text, so fold them into
// something that survives being pasted into a header
static std::string MakeIdentifier(const std::string_view raw) {
    std::string id;
    id.reserve(raw.size());
    for (const char ch : raw) {
        id += (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_') ? ch : '_';
    }

    if (id.empty()) return id;
    if (std::isdigit(static_cast<unsigned char>(id.front()))) id.insert(id.begin(), '_');
    return id;
}

static std::string Hex(const unsigned long value, const int width) {
    std::ostringstream hex;
    hex << "0x" << std::hex << std::nouppercase << std::setfill('0') << std::setw(width) << value;
    return hex.str();
}

// one namespace per entry, one tile index per include, plus the entry load
// address for BankOf()
bool GenerateHeader() {
    static constexpr std::string_view addrSymbol = "__addr";

    std::ostringstream header;
    header << "#pragma once\n\n"
              "// generated by blobman";
    if (manifestPath) header << " from " << manifestPath;
    header << " -- do not edit\n"
              "namespace blob {\n";

    for (size_t i = 0; i < entryCount; i++) {
        const entry& e = entries[i];

        const std::string entryName = MakeIdentifier(e.name);
        if (entryName.empty()) {
            std::cout << "entry name does not fold into an identifier: " << e.name << '\n';
            return false;
        }

        // resolve every symbol first so the '=' column can be padded to match
        struct genSymbol {
            std::string         name;   // folded identifier
            int                 tile{}; // tile index it resolves to
            const blobInclude*  from{}; // include that produced it
        };

        std::vector<genSymbol> symbols;
        symbols.reserve(e.files.size());
        size_t widest = addrSymbol.size();

        for (const blobInclude& f : e.files) {
            const std::string symbol = MakeIdentifier(f.name);
            if (symbol.empty()) {
                std::cout << "include name does not fold into an identifier: " << f.name
                          << ", in entry: " << e.name << '\n';
                return false;
            }

            if (symbol == addrSymbol) {
                std::cout << "include symbol collides with " << addrSymbol << ": " << f.name
                          << ", in entry: " << e.name << '\n';
                return false;
            }

            // two includes folding onto one symbol would silently shadow a tile,
            // so the manifest has to resolve it rather than blobman guessing
            for (const genSymbol& taken : symbols) {
                if (taken.name == symbol) {
                    std::cout << "naming conflict in entry: " << e.name << ": '"
                              << taken.from->path << "' and '" << f.path
                              << "' both name the symbol '" << symbol << "'\n";
                    return false;
                }
            }

            if (f.offset % tileSize) {
                std::cout << "include " << f.name << " in entry: " << e.name
                          << " does not start on a tile boundary\n";
                return false;
            }

            // VMA is a ppu byte address, so it divides down into the tile index
            // the hardware sees inside whichever pattern table it selects
            const int tile = (e.VMA + f.offset) / tileSize & 0xFF;

            symbols.push_back({ symbol, tile, &f });
            widest = std::max(widest, symbol.size());
        }

        header << "\n    // " << e.name << " entry\n"
                  "    namespace " << entryName << " {\n";

        for (const genSymbol& s : symbols) {
            header << "        constexpr unsigned char " << std::left << std::setw(static_cast<int>(widest))
                   << s.name << " = " << Hex(static_cast<unsigned long>(s.tile), 2) << ";\n";
        }

        header << "        constexpr unsigned long " << std::left << std::setw(static_cast<int>(widest))
               << addrSymbol << " = " << Hex(static_cast<unsigned long>(e.LMA), 4)
               << "; // used for BankOf()\n"
                  "    }\n";
    }

    header << "}\n";

    auto outFile = std::ofstream(headerFile, std::ios::trunc);
    if (!outFile) {
        std::cout << "could not open generated header: " << headerFile.string()
                  << ": " << std::strerror(errno) << '\n';
        return false;
    }

    outFile << header.str();
    outFile.close();
    if (!outFile) {
        std::cout << "failed writing generated header: " << headerFile.string() << '\n';
        return false;
    }

    if (verbose) {
        std::cout << "written header: " << headerFile.string()
                  << " (" << entryCount << " entries)\n";
    }

    return true;
}

static const char* includeLanguages_C[] = {
    "C", "c", "C++", "c++", "cpp", "Cpp", "CPP"
};

static const char* includeLanguages_S[] = {
    "S", "s", "asm", "Asm", "ASM", "ca65", "CA65"
};

eLanguage GetIncludeLang(const char* token) {
    for (const char* comp : includeLanguages_C) {
        if (strcmp(token, comp) == 0) {
            return c;
        }
    }

    for (const char* comp : includeLanguages_S) {
        if (strcmp(token, comp) == 0) {
            return ca65;
        }
    }

    return noLang;
}
