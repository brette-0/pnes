// log() (include/platform-nes/logger.hpp) is entirely a macro, on every
// platform -- zero-footprint data declarations on TARGET_NES, an immediate
// std::cout write everywhere else. Nothing here needs a runtime definition.
//
// This file deliberately defines nothing else either: an earlier version
// tried to give silentHeapAmount/silentStackAmount (logger.hpp's own
// comments) weak default definitions here, on the theory that a project's
// own override in main.hpp would beat them at link time. Confirmed the hard
// way that this never even gets the chance to matter: platform-nes is an
// ordinary static archive (plain `add_library(platform-nes STATIC ...)`),
// and archive linking only pulls in a .o member that resolves some
// currently-unresolved reference -- since nothing in the whole program ever
// calls into or otherwise references this file, it's never linked in at
// all, override or not. Both symbols are simply ABSENT from a build with no
// override, and tools/luaLogBuilder/main.cpp's own fallback (not anything
// here) is what actually supplies a "fully silent by default" value in that
// case -- see its own comments on silentHeapAmount/silentStackAmount.
