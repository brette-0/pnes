#include <iostream>
#include <fstream>
#include <toml++/toml.h>

static toml::table config;
static bool verbose;
static bool ProcessConfig();

enum eCommand {
    Bad,
    Help,
    Input,
    Output,
    End
};

static eCommand GetCommand(const char* token);
static void ShowHelpMessage();
int GetStringLength(const char* string);

int main(const int argc, char *argv[]) {
    if (argc == 0) {
        std::cout << "blobman requires a input toml file and an output directory\n";
        ShowHelpMessage();
        return -1;
    }

    char* tomlInputPath;
    char* exportOutputDir;

    for (int i = 1; i < argc; i++) {
        eCommand command = GetCommand(argv[i]);
        switch (command) {
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

                tomlInputPath = argv[i];
                config = toml::parse(tomlInputPath);
                try {
                    config = toml::parse_file(tomlInputPath);
                } catch (const toml::parse_error& err) {
                    std::cerr << "failed to parse " << tomlInputPath << ":\n"
                              << err.description() << "\n  (" << err.source().begin << ")\n";
                    return -1;
                }



                // want input file
                break;

            case Output:
                if (++i >= argc) {
                    std::cout << "output command must be followed by directory path\n";
                    return -1;
                }
                // want output directory
                break;

            case End:
                std::cout << "GetCommand returned enum 'END' this should not be possible.\n";
                break;
        }
    }
}

void ShowHelpMessage() {
    /*
     *  -h  --help      == prints help message
     *  -i  --input     == pass input toml file for processing
     *  -o  --output    == specify output directory for generated files
     *  -v  --verbose   == enable verbose mode
     */
}

static const char* shortFormMatches[] = {
    "-h",   // help
    "-i",   // input
    "-o"    // output
};

static const char* longFormMatches[] = {
    "--help",   // help
    "--input",  // input
    "--output"  // output
};

eCommand GetCommand(const char* token) {
    auto i = 0;
    for (;
        i < End                                         ||
            strcmp(token, shortFormMatches[i])  != 0    ||
            strcmp(token, longFormMatches[i])   != 0
        ; i++) { }
    if (i == End) return Bad;
    return static_cast<eCommand>(i);
}

struct entry {
    std::string_view    name;
    int                 offset;
    int                 size;
    char*               data;
};

bool ProcessConfig() {
    auto* entries = new entry[config.size()];
    int i = 0;
    for (auto&& [key, node] : config) {
        const auto& entry = node.as_table();
        if (!entry) {
            std::cout << "Top level may only contain entry tables\n";
            return false;
        }
        entries[i].name = key.str();
        const int offset = (*entry)["offset"].value_or(-1);
        if (!offset) {
            std::cout << "positive integer member 'offset' missing from entry: " << key.str() << '\n';
            return false;
        }
        entries[i].offset = offset;

        entries[i].name = key.str();
        const int size = (*entry)["size"].value_or(-1);
        if (!size) {
            std::cout << "positive integer member 'size' missing from entry: " << key.str() << '\n';
            return false;
        }
        entries[i].size = size;
        entries[i].data = new char[size];   // ready buffer

        const auto includes = (*entry)["includes"].as_array();
        if (!includes && verbose) {
            std::cout << "No includes for entry: " << key.str() << '\n';
            continue;
        }

        // if first entry (and therefore all) is not a string
        if (!includes->is_string()) {
            std::cout << "all includes must be a filepath\n";
            return false;
        }

        size_t blobProgress = 0;
        for (const auto& include : *includes) {
            auto includeFile = std::ifstream("path");
            if (!includeFile) {
                std::cout << "include file not found: " << "path" << '\n';
                return false;
            }

            const size_t begin = blobProgress;
            includeFile.seekg(std::ifstream::end);
            if (begin + includeFile.tellg() >= size) {
                std::cout << "overflowed blob: " << entries[i].name << " at include " << "path" << '\n';
                return false;
            }
            includeFile.seekg(std::ifstream::beg);
            // dump file from begin until exhaust context into entries[i].data
            blobProgress += includeFile.tellg();
        }
    }
}