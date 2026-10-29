/**
 * @file exporter_main.cpp
 * @brief `animator-export`: the designer's export, with no designer attached.
 *
 * The whole binary is exporter::run plus argv -- no Qt, no window, no display
 * server, which is the entire reason it exists as a target of its own: the
 * build regenerates demo/animations/gen/ at CONFIGURE time (see the
 * animator-export step in the root CMakeLists.txt), and most of the cross
 * build images have no Qt to link the GUI against. Everything the export
 * touches (proj, chr, codegen) is deliberately Qt-free, so the only dependency
 * here is toml++.
 *
 * The GUI binary takes the same switches and runs the same exporter::run (see
 * tools/animator/main.cpp), so `animator --export-all` and `animator-export`
 * write the same headers.
 */
#include "exporter.hpp"

int main(int argc, char** argv) {
    exporter::Options options;
    // Exporting is implied: a bare project path is the usual invocation, and
    // -E/--export-all is accepted only so the two binaries' command lines are
    // interchangeable.
    switch (exporter::parseArgs(argc, argv, options, /*exportImplied=*/true)) {
        case exporter::Parse::Export:
            return exporter::run(options);
        case exporter::Parse::Help:
            return 0;
        case exporter::Parse::Error:
        case exporter::Parse::NoExport:
            break;
    }
    return 1;
}
