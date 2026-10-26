cmake_minimum_required(VERSION 3.21)

# Per-developer overrides (paths to SDKs, custom cache vars, etc.).
# Copy local.cmake.example to local.cmake and edit for your machine.
# local.cmake is gitignored.
include("${CMAKE_SOURCE_DIR}/local.cmake" OPTIONAL)

# REGION is a separate axis from TARGET_PLATFORM: it distinguishes NTSC (0,
# the default for every target) from PAL (1) console timing/note tables, not
# which platform's backend gets built. TARGET_PLATFORM=nes_pal is the only
# value that sets REGION=1 -- it otherwise builds exactly the "nes" branch
# below (PLATFORM_DEFINE stays TARGET_NES; there is no separate TARGET_NES_PAL
# define) since a PAL NES is still an NES as far as every non-audio-timing
# backend concern goes.
set(REGION 0)

# The NES target cross-compiles with llvm-mos. Everything it needs -- clang,
# ld.lld, llvm-readelf -- lives inside the SDK, so the build resolves those from
# the SDK root, never the shell PATH. Point LLVM_MOS_SDK at your SDK install in
# local.cmake (the single machine-specific path); from it we derive the
# toolchain file and seed CMAKE_PREFIX_PATH for find_package(llvm-mos-sdk).
# Runs before project(). An explicitly supplied toolchain
# (-DCMAKE_TOOLCHAIN_FILE=... or an IDE preset) takes precedence and skips this.
# Keeping the SDK off PATH lets a bare clang/clang++ resolve to the host system
# compiler for the desktop builds instead of the 6502 cross-compiler.
if((TARGET_PLATFORM STREQUAL "nes" OR TARGET_PLATFORM STREQUAL "nes_pal") AND NOT CMAKE_TOOLCHAIN_FILE)
    if(NOT LLVM_MOS_SDK)
        message(FATAL_ERROR
            "NES build requires the llvm-mos-sdk location, but LLVM_MOS_SDK is "
            "unset. Copy local.cmake.example to local.cmake and set it to your "
            "SDK install root, e.g.\n"
            "    set(LLVM_MOS_SDK \"/path/to/llvm-mos\" CACHE PATH \"\" FORCE)\n"
            "The SDK does NOT need to be on your PATH.")
    endif()
    set(CMAKE_TOOLCHAIN_FILE
        "${LLVM_MOS_SDK}/lib/cmake/llvm-mos-sdk/llvm-mos-toolchain.cmake")
    # find_package(llvm-mos-sdk) below resolves through CMAKE_PREFIX_PATH; keep
    # it derived from the one source of truth rather than asking for it twice.
    set(CMAKE_PREFIX_PATH "${LLVM_MOS_SDK}"
        CACHE PATH "llvm-mos-sdk root (derived from LLVM_MOS_SDK)" FORCE)
endif()

# Mapper 4 (MMC3), not NROM: linked against the generic nes-platform crt0
# (LLVM_MOS_PLATFORM=nes, not nes-nrom), since nes-nrom's own crt0.o/link.ld
# hardcode NROM-specific __prg_rom_size/__chr_rom_size defaults that this
# MMC3 build must supply itself (see mmc3-helper.ld's COROLLARY-equivalent
# reasoning, carried over from vrc1-helper.ld).
#
# The generic nes platform has no default link.ld (unlike nes-nrom and the
# other mapper subtargets), so -T must be supplied explicitly. This has to
# happen *before* ANY CXX compiler probe -- not just project() below, but
# also enable_language(CXX) further down (ahead of find_package(Qt...) for
# the uitk-export step), which already binds CXX to this same llvm-mos cross
# compiler once CMAKE_TOOLCHAIN_FILE is set above. Both perform real links
# for compiler identification/ABI detection, and without -T those fail
# before any of this project's own code is ever touched ("cannot find linker
# script link.ld", then a harder-to-place "No known features for CXX
# compiler" once identification itself silently comes back empty) -- so this
# has to sit here, immediately after CMAKE_TOOLCHAIN_FILE is set above, not
# down by the "NES target" project() block where it used to live.
#
# CMAKE_CXX_FLAGS, not CMAKE_EXE_LINKER_FLAGS: CMake's own
# CMAKE_DETERMINE_COMPILER_ID probe builds its test invocation from
# CMAKE_<LANG>_FLAGS specifically (see CMakeDetermineCompilerId.cmake) and
# does not consult CMAKE_EXE_LINKER_FLAGS at all, so -T placed there is
# invisible to that one probe even though it reaches every ordinary target's
# link step. Compile-only steps harmlessly warn that -T is unused; silenced
# below rather than left to spam every translation unit.
if(TARGET_PLATFORM STREQUAL "nes" OR TARGET_PLATFORM STREQUAL "nes_pal")
    # BANKED CALL, Phase 5: TOPLEVEL_LD (originally VRC1_LD, renamed when the
    # demo converted from VRC1 to MMC3 -- the variable was never actually
    # mapper-specific, just named after whichever mapper existed first) is
    # not hardcoded into the library's own tree -- the library only ever
    # supplies a HELPER script (src/nes/mappers/mmc3-helper.ld: hardware
    # facts and mechanical plumbing), never the actual, final linker script a
    # real project links against (see BANKED_CALL_THEORY.txt's "THE FINAL
    # LINKER SCRIPT MUST BE USER-MADE" section -- written against VRC1, but
    # the principle is mapper-agnostic). The demo supplies its own top-level
    # script (demo/link.ld) via local.cmake, matching LLVM_MOS_SDK's own "no
    # fallback, missing is a hard configure-time error" convention just
    # above -- see local.cmake.example.
    if(NOT TOPLEVEL_LD)
        message(FATAL_ERROR
            "NES build requires a top-level linker script, but TOPLEVEL_LD is "
            "unset. Copy local.cmake.example to local.cmake and set it to "
            "your own project's top-level script, e.g.\n"
            "    set(TOPLEVEL_LD \"\${CMAKE_SOURCE_DIR}/demo/link.ld\" CACHE FILEPATH \"\" FORCE)\n"
            "That script should INCLUDE mmc3-helper.ld (src/nes/mappers) for "
            "the hardware/mechanical parts it doesn't need to reinvent.")
    endif()
    # -L: GNU ld/lld's INCLUDE directive does NOT search the directory of the
    # script doing the including -- only the linker's own -L search paths --
    # so a top-level script's `INCLUDE mmc3-helper.ld` needs this to resolve
    # at all.
    set(CMAKE_CXX_FLAGS "-T ${TOPLEVEL_LD} -L${CMAKE_SOURCE_DIR}/src/nes/mappers -Wno-unused-command-line-argument")

    # Also needs to be this early, same reasoning as CMAKE_CXX_FLAGS above:
    # the llvm-mos toolchain file (llvm-mos-toolchain.cmake) reads
    # LLVM_MOS_PLATFORM to pick CMAKE_CXX_COMPILER -- mos-${LLVM_MOS_PLATFORM}-
    # clang++ when set, a generic (and for this project's purposes, wrong --
    # no NES-specific default include/library paths) mos-clang++ otherwise --
    # and sets CMAKE_CXX_COMPILER_FORCED, so whichever it picks at the FIRST
    # enable_language(CXX)/project() call is locked in for the rest of this
    # configure, cached, never re-resolved later. Previously this was only
    # set down in the "NES target" project() block, well after the
    # uitk-export step's own enable_language(CXX) already ran -- meaning
    # LLVM_MOS_PLATFORM was still unset at that point, and it silently locked
    # in the generic mos-clang++, which has no NES sysroot and can't even
    # find <cstdint>. Hardcoded to "nes" (mapper 4/MMC3, not NROM) here for
    # the same reason the later project() block's own copy of this line was
    # unconditional: TOPLEVEL_LD above is hardcoded to this one mapper too,
    # nothing here branches on a second one yet.
    set(LLVM_MOS_PLATFORM nes)
endif()

# Application / SDL3 window title, compiled in as the PROJECT_NAME define.
# Set PROJECT_NAME in local.cmake to override per-developer; a default keeps
# the window titled when local.cmake is absent or leaves it unset.
#
# NOTE: this must stay ABOVE the project() calls below -- project() sets its
# own PROJECT_NAME variable, which would otherwise clobber this value before
# the define is created.
if(NOT PROJECT_NAME)
    set(PROJECT_NAME "My pnes Game")
endif()

# Capture the title for the AppImage .desktop file's Name= (configure_file
# substitutes @GAME_NAME@ in docker/demo.desktop.in). This must happen here,
# before project() resets the PROJECT_NAME *variable* to "pnes":
# appimagetool derives the output filename's prefix from Name=, and an empty
# Name produces a "-x86_64.AppImage" argument that mksquashfs rejects as a flag.
# project() does not touch GAME_NAME, so it survives to the linux block below.
set(GAME_NAME "${PROJECT_NAME}")

# Nametable mirroring orientation: pnes.hpp requires exactly one of
# LANDSCAPE or PORTRAIT to be defined. Set ORIENTATION in local.cmake to
# override per-developer; defaults to LANDSCAPE when left unset.
if(NOT ORIENTATION)
    set(ORIENTATION "LANDSCAPE")
endif()
if(NOT ORIENTATION STREQUAL "LANDSCAPE" AND NOT ORIENTATION STREQUAL "PORTRAIT")
    message(FATAL_ERROR
        "ORIENTATION must be LANDSCAPE or PORTRAIT, got: \"${ORIENTATION}\"")
endif()
add_compile_definitions(${ORIENTATION})
add_compile_definitions(PROJECT_NAME="${PROJECT_NAME}")

# The library and demo are compiled as C++23. C++20 would cover the
# designated initialisers used throughout, but the SDL3 backend relies
# on <stdatomic.h>'s `atomic_int` (_vblank_flag), which only became
# usable from C++ in C++23. The gnu++ dialect keeps the GNU compound-literal
# extension the video/interrupt code uses. Both toolchains accept it:
# llvm-mos's clang++ and host GCC.
#
# Raw -std=gnu++23, not CMAKE_CXX_STANDARD 23 + CMAKE_CXX_STANDARD_REQUIRED
# ON + CMAKE_CXX_EXTENSIONS ON (what this used to be): that trio makes every
# target's CXX_STANDARD property go through CMake's own compile-feature
# verification (cmStandardLevelResolver::CompileFeatureKnown), which hard-
# errors ("No known features for CXX compiler ...") for a compiler whose
# reported version it doesn't recognize -- exactly llvm-mos's clang++, which
# reports itself as Clang 23.0.0 (tracking upstream LLVM trunk's current
# in-development major version, not a real numbered Clang release CMake's
# compiled-in version table has ever seen). PASS1_ELF's own raw compiler
# invocation further down already had to duplicate -std=gnu++23 explicitly
# for the same reason (bypasses CMAKE_CXX_STANDARD entirely, per its own
# comment) -- this just does that project-wide instead of trusting CMake's
# standard-injection machinery to work for this toolchain at all.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-std=gnu++23>)

# ----------------------------------------------------------------------
# br0::intsh — header-only integer type aliases, used via #include <intsh>.
# An INTERFACE target, so the same declaration serves both the NES and
# desktop builds; FetchContent_MakeAvailable is called inside each branch
# below (after project()), and the target is linked PUBLIC onto the
# pnes library so the include dir propagates to the demo.
# ----------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
        intsh
        GIT_REPOSITORY https://github.com/brette-0/intsh.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE
)

# ----------------------------------------------------------------------
# br0::tuple — header-only freestanding tuple, used via #include <br0/tuple>.
# SOURCE_SUBDIR points add_subdirectory at the repo's br0/ folder, which
# defines just the br0::tuple INTERFACE target. This deliberately skips the
# repo's top-level CMakeLists.txt, which hard-requires a machine-specific
# cmake/local.cmake and builds the upstream test suite — neither of which we
# want pulled into this build.
# ----------------------------------------------------------------------
FetchContent_Declare(
        tuple
        GIT_REPOSITORY https://github.com/brette-0/tuple.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  br0
)

# ----------------------------------------------------------------------
# br0::consh — header-only concept shorthands for integer types, used via
# #include <consh>. A required library for every target, so it is fetched in
# every TARGET_PLATFORM branch below -- including PS2/PS3, which shim intsh
# rather than fetching it: consh only declares concepts inside
# namespace br0::concepts, includes nothing but <type_traits> and carries no
# `using namespace` of its own, so it has none of the global-typedef
# collisions that made those shims necessary.
#
# Upstream is a single header at the repo root with no CMakeLists.txt, so
# FetchContent_MakeAvailable() just populates it -- there is no upstream
# target to link against. _make_consh_available() below wraps that
# population and defines the br0::consh INTERFACE target this project links
# instead; it is idempotent, since the same branch may reach it more than
# once.
# ----------------------------------------------------------------------
FetchContent_Declare(
        consh
        GIT_REPOSITORY https://github.com/brette-0/consh.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE
)

macro(_make_consh_available)
    FetchContent_MakeAvailable(consh)
    if(NOT TARGET consh_headers)
        add_library(consh_headers INTERFACE)
        target_include_directories(consh_headers INTERFACE ${consh_SOURCE_DIR})
        add_library(br0::consh ALIAS consh_headers)
    endif()
endmacro()

# intsh and br0::tuple each declare their own INTERFACE
# target_compile_features(... cxx_std_20) -- reasonable in isolation, but it
# routes through CMake's own compile-feature verification
# (cmStandardLevelResolver::CompileFeatureKnown) for whichever compiler ends
# up consuming them, and that hard-errors ("No known features for CXX
# compiler ...") for llvm-mos's clang++, which reports itself as Clang
# 23.0.0 -- a version CMake's compiled-in version table doesn't recognize
# (see this project's own -std=gnu++23 add_compile_options above, added for
# the exact same reason). Can't fix this upstream in either repo from here
# (they're fetched fresh into _deps/ on every clean configure, never part of
# this source tree) -- instead, strip the property right after each fetch
# makes the targets available: this project's own -std=gnu++23 already
# guarantees at least as much as cxx_std_20 ever would, so nothing is
# actually lost by no longer asking CMake to separately verify it.
macro(_fixup_fetched_cxx_std_targets)
    if(TARGET intsh)
        set_property(TARGET intsh PROPERTY INTERFACE_COMPILE_FEATURES "")
    endif()
    if(TARGET br0_tuple)
        set_property(TARGET br0_tuple PROPERTY INTERFACE_COMPILE_FEATURES "")
    endif()
endmacro()

# ----------------------------------------------------------------------
# uitk-export -- regenerates demo/gen/<name>/<target>/<name>.hpp|.cpp for
# EVERY demo/ui/<name>.uis, by building and running tools/uitk (see
# tools/uitk/README.md) once per scene file found by the glob below. The
# output directory always matches the .uis file's own basename -- add a new
# demo/ui/whatever.uis and it exports to demo/gen/whatever with no edit
# needed here; there is no per-scene special-casing (gameOptions.uis used to
# export to demo/gen/playOptions instead of demo/gen/gameOptions -- that
# exception is gone, and every TARGET_PLATFORM branch below now globs
# demo/gen/gameOptions/ to match). Runs here, at CONFIGURE time --
# deliberately, not as a build-time target `demo` depends on: demo/gen/ is
# gitignored, never committed, so every TARGET_PLATFORM branch's own
# `file(GLOB DEMO_TITLE_SOURCES ...)` below has to see the regenerated files
# BEFORE it globs, not after. A build-time custom target regenerates too
# late for that -- CMake fixes a GLOB's matched file list into build.ninja
# at configure time, so a custom target that adds/removes a title.cpp
# mid-build leaves that already-baked build graph pointing at a file that
# either isn't there yet or just disappeared. (That's exactly how this broke
# once NES stopped needing a title.cpp at all -- Draw_/Erase_ moved to AI,
# body merged into the header, so genCtTextBoxBody's NES output no longer
# has any .cpp-side content, see tools/uitk/main.cpp. Only a second
# `cmake --build`, which reconfigures and re-globs against the post-export
# directory state, would have picked that up.) Running the export before
# the globs, in the same configure pass, removes the need for that second
# invocation.
#
# uitk is a standalone Qt Widgets app (tools/uitk/CMakeLists.txt), built here
# via its own nested `cmake` invocation rather than add_subdirectory -- most
# importantly, that means it's never built through *this* project's own
# toolchain. That matters concretely for TARGET_PLATFORM=nes/nes_pal, which
# points CMAKE_TOOLCHAIN_FILE at the llvm-mos cross-compiler above: a nested
# `cmake -S tools/uitk -B ...` subprocess doesn't inherit that (it's never
# passed along), so it always configures against the host's own compiler,
# regardless of which platform this build itself targets. Placed here, ahead
# of every project() call below, so find_package(Qt...) below also always
# searches the host -- CMAKE_TOOLCHAIN_FILE's CMAKE_SYSTEM_NAME/sysroot
# settings (set above, for nes/nes_pal) aren't actually applied by CMake
# until the first project()/enable_language() call processes them.
#
# Qt (Widgets) is only actually required when demo/gen/ doesn't already have
# something to fall back to. With Qt: always regenerate, so a local build is
# never stale after editing any demo/ui/*.uis. Without Qt: warn and use
# whatever's already under demo/gen/ instead -- normally populated by CI's
# `codegen-check` job's demo-gen artifact (.github/workflows/ci.yml; every
# other platform job downloads it, deliberately so *those* never need Qt
# themselves, since most of their Docker images/runners don't have it at
# all), or by an earlier Qt-equipped local export. Only a genuinely empty
# demo/gen/ with no Qt available is a hard configure error -- nothing to
# build against at all.
#
# Re-runs whenever any demo/ui/*.uis or uitk's own sources change (see the
# CMAKE_CONFIGURE_DEPENDS below) -- CMake already reconfigures automatically
# on any CONFIGURE_DEPENDS-listed file changing, the same mechanism
# file(GLOB ... CONFIGURE_DEPENDS) itself relies on, so a plain
# `cmake --build <builddir>` after editing either picks it up with no
# separate step to remember.
#
# enable_language(CXX) here, ahead of find_package(Qt...): Qt6Config.cmake
# transitively find_package(Threads)s, and FindThreads asserts a language is
# already enabled -- normally true by the time this ran (every branch below
# calls project(pnes CXX) itself), but this block now runs before
# all of them. Redundant, not conflicting, with each branch's own project()
# call moments later (CMake tolerates re-enabling an already-enabled
# language); for nes/nes_pal this does bind CXX to the llvm-mos cross
# compiler already selected above via CMAKE_TOOLCHAIN_FILE, same as that
# later project() call would anyway -- find_package(Qt...) itself doesn't
# care which compiler got enabled, only that FindThreads' own assertion is
# satisfied (CMAKE_SYSTEM_NAME=Generic sets no CMAKE_FIND_ROOT_PATH, so
# nothing here restricts the search to a cross sysroot either way).
#
# Saved BEFORE enable_language(CXX): for nes/nes_pal, CMake's own compiler
# enablement exports CXX (and, on some setups, CC) as real process
# environment variables pointing at the just-identified compiler --
# confirmed directly against this llvm-mos toolchain, not documented
# behavior this comment previously assumed didn't apply. That's fine for
# THIS process, but execute_process() below spawns nested `cmake`
# subprocesses that inherit this same process's environment (regardless of
# PATH, and regardless of nothing being passed as COMMAND arguments) -- so
# without restoring these first, tools/uitk's own host-only configure
# silently inherits CXX=<the NES cross-compiler> too and fails its own
# compiler sanity check trying to link a plain host test program against
# NES crt0.
set(_HOST_ENV_CXX "$ENV{CXX}")
set(_HOST_ENV_CC "$ENV{CC}")
enable_language(CXX)
# What enable_language(CXX) just bound CXX/CC to (see this block's own
# comment above) -- saved so the host-compiler swap below, around the
# nested uitk subprocesses only, can be undone afterward. Every later
# project()/enable_language(CXX) call in this file (inside each
# TARGET_PLATFORM branch) needs CXX/CC still pointed at this, not left on
# the host values the uitk swap leaves behind.
set(_MOS_ENV_CXX "$ENV{CXX}")
set(_MOS_ENV_CC "$ENV{CC}")

# What every nested host-tool `cmake` below (tools/uitk, tools/blobman) must be
# told explicitly, because a subprocess inherits NONE of it: a bare
# `cmake -S <tool> -B <dir>` picks CMake's own built-in default generator, which
# on any Unix host is "Unix Makefiles" -- regardless of this build being
# configured with Ninja. On a machine (or Docker image) that has ninja but no
# make, that nested configure dies with
#
#     CMake Error: CMake was unable to find a build program corresponding to
#     "Unix Makefiles".  CMAKE_MAKE_PROGRAM is not set.
#     CMake Error: CMAKE_CXX_COMPILER not set, after EnableLanguage
#
# (the second line is just the first one cascading -- no build tool means no
# compiler check). Handing the nested configure this build's OWN generator, and
# the program it drives, removes the guess entirely: whatever built this project
# is by definition present. CMAKE_MAKE_PROGRAM is only forwarded when CMake
# actually resolved one, so an unset cache entry is left for the nested
# configure to resolve for itself rather than being pinned to an empty string.
set(NESTED_HOST_CMAKE_ARGS -G "${CMAKE_GENERATOR}")
if(CMAKE_MAKE_PROGRAM)
    list(APPEND NESTED_HOST_CMAKE_ARGS -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM})
endif()

# Pinning the generator above has one consequence worth handling here rather
# than in an error message: CMake refuses to reconfigure an existing build
# directory under a different generator than the one that created it
# ("does not match the generator used previously"). Any nested build dir left
# behind by an earlier configure of this project predates that pin and is
# therefore on the default generator, so it would now fail permanently until
# someone deleted it by hand. Drop the stale cache instead -- these are
# disposable host-tool build trees, nothing in them is worth preserving.
macro(clear_nested_build_dir_on_generator_change dir)
    if(EXISTS "${dir}/CMakeCache.txt")
        file(STRINGS "${dir}/CMakeCache.txt" _NESTED_CACHED_GENERATOR
             REGEX "^CMAKE_GENERATOR:INTERNAL=")
        if(NOT _NESTED_CACHED_GENERATOR STREQUAL
           "CMAKE_GENERATOR:INTERNAL=${CMAKE_GENERATOR}")
            file(REMOVE_RECURSE "${dir}")
        endif()
        unset(_NESTED_CACHED_GENERATOR)
    endif()
endmacro()
# On nes/nes_pal (and any other bare-metal preset whose toolchain file sets
# CMAKE_SYSTEM_NAME to a non-host value, e.g. ps2/gc/wii/3ds), that value is
# already latched in by enable_language(CXX) above by the time find_package
# below runs in this same process. Qt5/6's own CMake config declares its
# imported libraries SHARED, so add_library()'s generic "this platform
# doesn't support dynamic linking" check fires for every one of them (~30
# warnings) on any machine that actually has Qt installed -- spurious here,
# since find_package below is a plain HOST tool lookup (for building
# tools/uitk), not part of the cross-compile. These are CMake dev/author
# warnings ("Warning (author)", "Use -Wno-author to suppress"); suppressing
# them needs the real -Wno-dev behavior, which only takes effect from the
# command line/presets (CMAKE_SUPPRESS_DEVELOPER_WARNINGS as a script-side
# set() does NOT work -- confirmed empirically, despite what its docs
# imply), so that's set via "warnings": {"dev": false} on CMakePresets.json's
# _base preset instead of here.
find_package(Qt6 QUIET COMPONENTS Widgets)
if(NOT Qt6Widgets_FOUND)
    find_package(Qt5 QUIET COMPONENTS Widgets)
endif()

if(Qt6Widgets_FOUND OR Qt5Widgets_FOUND)
    file(GLOB_RECURSE UITK_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tools/uitk/*.cpp)
    file(GLOB UITK_SCENE_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/demo/ui/*.uis)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 ${UITK_SCENE_FILES}
                 ${UITK_SOURCES}
                 ${CMAKE_SOURCE_DIR}/tools/uitk/CMakeLists.txt)

    # Restore CXX/CC to whatever they were before enable_language(CXX) above
    # (the host's default if the invoking shell never set them itself) --
    # see that block's own comment. Every execute_process() below is a
    # nested `cmake` subprocess that must configure/build tools/uitk against
    # the HOST compiler, never whatever enable_language(CXX) just bound CXX
    # to for TARGET_PLATFORM=nes/nes_pal.
    if(_HOST_ENV_CXX)
        set(ENV{CXX} "${_HOST_ENV_CXX}")
    else()
        unset(ENV{CXX})
    endif()
    if(_HOST_ENV_CC)
        set(ENV{CC} "${_HOST_ENV_CC}")
    else()
        unset(ENV{CC})
    endif()

    clear_nested_build_dir_on_generator_change(${CMAKE_BINARY_DIR}/uitk-build)
    execute_process(
            COMMAND ${CMAKE_COMMAND} -S ${CMAKE_SOURCE_DIR}/tools/uitk -B ${CMAKE_BINARY_DIR}/uitk-build
                    ${NESTED_HOST_CMAKE_ARGS}
            RESULT_VARIABLE UITK_CONFIGURE_RESULT)
    if(NOT UITK_CONFIGURE_RESULT EQUAL 0)
        message(FATAL_ERROR "uitk: failed to configure tools/uitk (see output above).")
    endif()
    execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}/uitk-build
            RESULT_VARIABLE UITK_BUILD_RESULT)
    if(NOT UITK_BUILD_RESULT EQUAL 0)
        message(FATAL_ERROR "uitk: failed to build (see output above).")
    endif()
    # One export per demo/ui/*.uis found above -- output dir is always that
    # file's own basename (demo/ui/foo.uis -> demo/gen/foo/<target>/foo.hpp|.cpp),
    # so a new scene file needs no edit here to start exporting.
    foreach(UITK_SCENE_FILE ${UITK_SCENE_FILES})
        get_filename_component(UITK_SCENE_NAME ${UITK_SCENE_FILE} NAME_WE)
        execute_process(
                COMMAND ${CMAKE_BINARY_DIR}/uitk-build/uitk --export-all
                        --input ${UITK_SCENE_FILE}
                        --output ${CMAKE_SOURCE_DIR}/demo/gen/${UITK_SCENE_NAME}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                RESULT_VARIABLE UITK_EXPORT_RESULT)
        if(NOT UITK_EXPORT_RESULT EQUAL 0)
            message(FATAL_ERROR "uitk: export failed for ${UITK_SCENE_FILE} (see output above).")
        endif()
    endforeach()

    # Put CXX/CC back to what enable_language(CXX) above actually bound them
    # to -- undoes the host-compiler swap just for the nested uitk
    # subprocesses (see both this swap's own comment and the one on
    # _MOS_ENV_CXX/_MOS_ENV_CC above). Every project()/enable_language(CXX)
    # call from here on (inside each TARGET_PLATFORM branch below) must see
    # CXX/CC pointed at the SAME compiler enable_language(CXX) already
    # identified and cached, not the host swap.
    if(_MOS_ENV_CXX)
        set(ENV{CXX} "${_MOS_ENV_CXX}")
    else()
        unset(ENV{CXX})
    endif()
    if(_MOS_ENV_CC)
        set(ENV{CC} "${_MOS_ENV_CC}")
    else()
        unset(ENV{CC})
    endif()
elseif(EXISTS ${CMAKE_SOURCE_DIR}/demo/gen/title AND EXISTS ${CMAKE_SOURCE_DIR}/demo/gen/gameOptions)
    # STATUS, not WARNING: expected on every Docker/CI build (none of those
    # images install Qt), so a WARNING here would just be permanent build
    # noise. Still surfaced so a local configure without Qt says why it's
    # using the checked-in demo/gen/ output instead of regenerating it.
    message(STATUS
        "Qt Widgets not found; using whatever's already under demo/gen/ "
        "instead of regenerating it from demo/ui/*.uis (e.g. a "
        "downloaded demo-gen CI artifact, or an earlier Qt-equipped local "
        "export). Install Qt6 (or Qt5) to keep it in sync here after "
        "editing demo/ui/title.uis or demo/ui/gameOptions.uis locally.")
else()
    message(FATAL_ERROR
        "Qt Widgets not found, and demo/gen/title/ or demo/gen/gameOptions/ "
        "doesn't exist -- nothing to fall back to. Install Qt6 (or Qt5) so "
        "this configure step can generate them from demo/ui/*.uis, or "
        "populate demo/gen/ yourself first (e.g. download codegen-check's "
        "demo-gen CI artifact).")
endif()

# ----------------------------------------------------------------------
# blobman -- repacks demo/chr/chr.bin (the cartridge's single CHR ROM image)
# and demo/chr/chr.h (the symbolic tile ids beside it) from the layout in
# demo/chr/config.toml, by building and running tools/blobman.
#
# Runs here, at CONFIGURE time, for the same reason the uitk-export step above
# does: the pair has to be current BEFORE anything compiles against it.
# demo/chr/config.toml is listed in CMAKE_CONFIGURE_DEPENDS below, together
# with blobman's own sources and every .chr under demo/chr/assets/, so editing
# the manifest or any piece of art is treated exactly like editing code -- a
# plain `cmake --build <builddir>` reconfigures, repacks, and then rebuilds
# whatever that touched. chr.h is an ordinary header, so the compiler's own
# depfiles pick up its dependents; chr.bin is reached by #embed from
# demo/src/graphics/chr_rom.cpp, which each TARGET_PLATFORM branch below
# already pins with OBJECT_DEPENDS.
#
# Unlike demo/gen/, both outputs are COMMITTED -- so there is always something
# to fall back to, and a host that cannot build blobman at all (no network for
# toml++, no compiler) is a STATUS message and the committed pair, not a
# configure failure. CI's blobman-check job is what proves the committed pair
# still matches the manifest and the art.
#
# Built via its own nested `cmake` invocation rather than add_subdirectory,
# for all the reasons spelled out on the uitk block above -- chiefly that a
# nested subprocess never inherits CMAKE_TOOLCHAIN_FILE, so blobman always
# builds as the host tool it is even when this build targets nes/nes_pal. The
# CXX/CC swap around it is that same block's swap, for that same reason: see
# the comments on _HOST_ENV_CXX / _MOS_ENV_CXX above.
file(GLOB_RECURSE BLOBMAN_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tools/blobman/*.cpp)
file(GLOB_RECURSE BLOBMAN_ART CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/demo/chr/assets/*.chr)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${CMAKE_SOURCE_DIR}/demo/chr/config.toml
             ${BLOBMAN_ART}
             ${BLOBMAN_SOURCES}
             ${CMAKE_SOURCE_DIR}/tools/blobman/CMakeLists.txt)

# Is there a HOST compiler to build blobman with at all? Asked here, before the
# nested configure, because the answer is legitimately "no" on some of the
# cross-build images: docker/Dockerfile.ps2 is Alpine with the prebuilt MIPS
# EE/IOP toolchain and NOTHING else -- no build-base, so no c++/g++/clang++ on
# PATH, only mips64r5900el-ps2-elf-g++ and friends. Letting the nested cmake
# discover that for itself works (the fallback below catches it either way), but
# it costs a wasted configure and spills
#
#     CMake Error at CMakeLists.txt:18 (project):
#       No CMAKE_CXX_COMPILER could be found.
#
# into the log of a build that is actually fine -- an error that reads like a
# broken PS2 toolchain while being nothing of the sort. Probing first turns that
# into the one STATUS line further down that says what really happened.
#
# $ENV{CXX} wins when the invoking shell set it, matching what the nested
# configure would honor anyway; otherwise this is the same c++/g++/clang++
# search CMake itself would run. NO_CMAKE_FIND_ROOT_PATH because a cross
# toolchain file may have set CMAKE_FIND_ROOT_PATH_MODE_PROGRAM to ONLY, which
# would confine this search to the target sysroot -- the exact opposite of what
# a host-tool lookup wants.
if(_HOST_ENV_CXX)
    set(BLOBMAN_HOST_CXX "${_HOST_ENV_CXX}")
else()
    find_program(BLOBMAN_HOST_CXX NAMES c++ g++ clang++ NO_CMAKE_FIND_ROOT_PATH)
endif()

if(_HOST_ENV_CXX)
    set(ENV{CXX} "${_HOST_ENV_CXX}")
else()
    unset(ENV{CXX})
endif()
if(_HOST_ENV_CC)
    set(ENV{CC} "${_HOST_ENV_CC}")
else()
    unset(ENV{CC})
endif()

# Release: this is a throwaway host tool run once per configure, and an
# optimized toml++ parse is the difference between that being unnoticeable and
# not. Quiet on success, loud on failure -- OUTPUT_QUIET is deliberately NOT
# paired with ERROR_QUIET, so a compile error still reaches the log.
if(BLOBMAN_HOST_CXX)
    clear_nested_build_dir_on_generator_change(${CMAKE_BINARY_DIR}/blobman-build)
    # CMAKE_CXX_COMPILER is passed explicitly rather than left to the nested
    # configure's own search: it is exactly what the probe above resolved, so
    # the tool that gets built is the one this block decided on, and a second
    # search under a cross-build environment cannot land somewhere else.
    execute_process(
            COMMAND ${CMAKE_COMMAND} -S ${CMAKE_SOURCE_DIR}/tools/blobman
                                     -B ${CMAKE_BINARY_DIR}/blobman-build
                                     -DCMAKE_BUILD_TYPE=Release
                                     -DCMAKE_CXX_COMPILER=${BLOBMAN_HOST_CXX}
                                     ${NESTED_HOST_CMAKE_ARGS}
            RESULT_VARIABLE BLOBMAN_CONFIGURE_RESULT
            OUTPUT_QUIET)
else()
    set(BLOBMAN_CONFIGURE_RESULT 1)
endif()

if(BLOBMAN_CONFIGURE_RESULT EQUAL 0)
    execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}/blobman-build
            RESULT_VARIABLE BLOBMAN_BUILD_RESULT
            OUTPUT_QUIET)
else()
    set(BLOBMAN_BUILD_RESULT 1)
endif()

# Why the pair could not be repacked, phrased for both message branches below.
# One quoted string per case, deliberately: set() with several string arguments
# makes a LIST, which prints with ';' separators where the spaces should be
# once interpolated into a message.
if(BLOBMAN_HOST_CXX)
    set(BLOBMAN_FALLBACK_REASON "tools/blobman failed to build with ${BLOBMAN_HOST_CXX} (see the output above -- most often no system toml++ and no network to fetch it)")
else()
    set(BLOBMAN_FALLBACK_REASON "this image/host has no C++ compiler of its own, only a cross toolchain (expected on the PS2 build image, for one)")
endif()

if(BLOBMAN_BUILD_RESULT EQUAL 0)
    # -v, always: the per-entry fill report ("entry: ui filled 832 of 1024
    # bytes") is the only visibility into a hunk quietly running out of room,
    # and the configure log is where that belongs -- same reasoning as
    # blobman-check's own invocation in .github/workflows/ci.yml.
    #
    # Relative paths with demo/chr as the working directory, and that is not
    # cosmetic: the manifest's own includes resolve against it, and blobman
    # echoes the -i path verbatim into chr.h's provenance comment, so an
    # absolute path here would rewrite the committed header on every machine
    # and never match what CI diffs against.
    execute_process(
            COMMAND ${CMAKE_BINARY_DIR}/blobman-build/blobman
                    -i config.toml -o chr.bin -l cpp -v
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/demo/chr
            RESULT_VARIABLE BLOBMAN_PACK_RESULT)
    if(NOT BLOBMAN_PACK_RESULT EQUAL 0)
        message(FATAL_ERROR
            "blobman: packing demo/chr/config.toml failed (see output above). "
            "The manifest or the art under demo/chr/assets/ is the problem -- "
            "this is not a toolchain fallback case, so it is fatal.")
    endif()
elseif(EXISTS ${CMAKE_SOURCE_DIR}/demo/chr/chr.bin AND EXISTS ${CMAKE_SOURCE_DIR}/demo/chr/chr.h)
    # STATUS, not WARNING: expected on the Docker/CI images that have no
    # network for toml++'s FetchContent fallback, where a warning would just
    # be permanent build noise. Still surfaced so a local configure says why
    # it is compiling against the committed pair rather than a fresh pack.
    message(STATUS
        "blobman: ${BLOBMAN_FALLBACK_REASON}; using the committed "
        "demo/chr/chr.bin + chr.h instead of repacking them from "
        "demo/chr/config.toml. That is fine for a cross build -- the pair is "
        "committed -- but edits to the manifest or to demo/chr/assets/ will "
        "NOT be picked up here. Repack on a host with a C++20 compiler, or let "
        "CI's blobman-check job catch the drift.")
else()
    message(FATAL_ERROR
        "blobman: ${BLOBMAN_FALLBACK_REASON}, and demo/chr/chr.bin or chr.h is "
        "missing -- nothing to fall back to. Either restore the committed pair "
        "or give this host a C++20 compiler and a toml++ (system package or "
        "network); nothing below can compile against a CHR ROM that is not "
        "there.")
endif()

if(_MOS_ENV_CXX)
    set(ENV{CXX} "${_MOS_ENV_CXX}")
else()
    unset(ENV{CXX})
endif()
if(_MOS_ENV_CC)
    set(ENV{CC} "${_MOS_ENV_CC}")
else()
    unset(ENV{CC})
endif()

# ----------------------------------------------------------------------
# NES target
# ----------------------------------------------------------------------
if(TARGET_PLATFORM STREQUAL "nes" OR TARGET_PLATFORM STREQUAL "nes_pal")
    if(TARGET_PLATFORM STREQUAL "nes_pal")
        set(REGION 1)
    endif()

    # CMAKE_CXX_FLAGS (-T/-L/TOPLEVEL_LD's own FATAL_ERROR guard) is set much
    # earlier now, right after CMAKE_TOOLCHAIN_FILE near the top of this file
    # -- see that block's own comment for why (the unconditional
    # enable_language(CXX) the uitk-export step needs, further up still,
    # turned out to need it too, not just project() below).

    # ------------------------------------------------------------------
    # Where the FamiStudio audio backend is placed. THIS BACKEND IS A PROJECT
    # CHOICE, NOT A LIB ONE -- audio.hpp declares plain prototypes with no
    # definition, and src/nes/audio/famistudio.cpp is the one .cpp that opts a
    # project into FamiStudio specifically by defining them. It tags every one
    # of its functions ::AUDIO_BANK, which expands to a section attribute
    # naming PNES_AUDIO_SECTION. That section MUST be the same PRG-ROM
    # bank the audio engine itself is assembled into -- see
    # demo/famistudio_config.s's FAMISTUDIO_CA65_CODE_SEGMENT, which is the
    # matching knob on the ca65 side. Placed together, famistudio.cpp reaches
    # the engine with plain calls and contains no bank switching at all; a
    # caller long-calls audio::Update() and both arrive mapped.
    #
    # NO DEFAULT, deliberately: a guessed section would place this module in
    # whichever bank the library assumed and produce a wrong-bank call with no
    # diagnostic. Unset is an #error in famistudio.cpp itself, the same "no
    # silent fallback" stance TOPLEVEL_LD above takes.
    # ------------------------------------------------------------------
    if(NOT PNES_AUDIO_SECTION)
        message(FATAL_ERROR
                "PNES_AUDIO_SECTION is not set. It names the ELF section "
                "src/nes/audio/famistudio.cpp is placed into, and must be the bank "
                "the audio engine is assembled into. See local.cmake.example.")
    endif()
    set_source_files_properties(${CMAKE_SOURCE_DIR}/src/nes/audio/famistudio.cpp
            PROPERTIES COMPILE_DEFINITIONS
            "PNES_AUDIO_SECTION=\"${PNES_AUDIO_SECTION}\"")

    # ------------------------------------------------------------------
    # Where the UI toolkit (src/all/extras/ui/*.cpp) is placed. Same
    # treatment as audio's PNES_AUDIO_SECTION above: COMPULSORY,
    # no default value. A default would silently place this module
    # wherever the library guessed, which is exactly the
    # wrong-bank-with-no-diagnostic failure this project's placement
    # mechanism otherwise avoids everywhere else -- a project targeting
    # NES has to make an explicit call for the UI module too, not just
    # audio. Unlike audio, there is no external engine to co-locate with;
    # this is required purely so the placement decision is never a silent
    # guess.
    #
    # Covers every ui .cpp file as one group -- the whole toolkit moves or
    # stays together, same as audio's single-module contract.
    # ------------------------------------------------------------------
    if(NOT PNES_UI_SECTION)
        message(FATAL_ERROR
                "PNES_UI_SECTION is not set. It names the ELF section "
                "src/all/extras/ui/*.cpp is placed into. See local.cmake.example.")
    endif()
    set(PNES_UI_SOURCES
            ${CMAKE_SOURCE_DIR}/src/all/extras/ui/tileslider.cpp
            ${CMAKE_SOURCE_DIR}/src/all/extras/ui/text.cpp)
    set_source_files_properties(${PNES_UI_SOURCES}
            PROPERTIES COMPILE_DEFINITIONS
            "PNES_UI_SECTION=\"${PNES_UI_SECTION}\"")

    # ------------------------------------------------------------------
    # TITLE / SYSMEM: placement for uitk-generated UI code (and any other
    # CREATE_SEGMENT_KEYWORD-built macro demo/src/banks.hpp hands out).
    # SYSMEM mirrors mmc3.hpp's own ::SYSMEM (".sysmem", onboard system RAM,
    # NOLOAD -- see mmc3-helper.ld) -- generated code can't just
    # #include <pnes/mappers/mmc3.hpp> to get that macro (it isn't
    # NES-mapper-specific and shouldn't couple to one), so it's fed in here
    # instead, same as TITLE. DEMO-owned rather than library-owned like
    # AUDIO/UI_SECTION above, so these aren't set_source_files_properties()'d
    # onto a fixed file list -- a generated scene's header can be #included
    # from any demo .cpp, so the definitions below go on the `demo` target as
    # a whole (further down, once that target exists) instead. Compulsory,
    # same "no silent guess" stance as AUDIO/UI_SECTION; see
    # local.cmake.example for why these are bare/ALL_CAPS rather than
    # PNES_*_SECTION-namespaced.
    # ------------------------------------------------------------------
    if(NOT TITLE)
        message(FATAL_ERROR
                "TITLE is not set. It names the ELF section uitk-generated "
                "title-scene code/data is placed into. See local.cmake.example.")
    endif()
    if(NOT SYSMEM)
        message(FATAL_ERROR
                "SYSMEM is not set. It names the ELF section uitk-generated "
                "code using the \"SYSMEM\" Linker Prefix is placed into. "
                "See local.cmake.example.")
    endif()

    # ------------------------------------------------------------------
    # Placement for the other modules that produce code. OPTIONAL, unlike
    # audio's: audio MUST share a bank with the engine it plain-calls, so an
    # unset section there is an #error. These have no such contract, so unset
    # means "don't place it" -- today's behaviour, inlined into callers.
    #
    # Setting one is a real tradeoff and the module's own comment says so: the
    # section only governs a function's out-of-line copy, and LTO will inline
    # the body into a caller elsewhere regardless, so the keyword also carries
    # noinline. That is why video:: is 2,097 bytes compiled alone and 0 bytes
    # in the linked ROM today. Placement means giving up inlining for that
    # module; the section variable being set IS the opt-in.
    #
    # Only modules worth moving get an entry. apu.cpp is register objects and
    # technology.cpp is empty -- both compile to 0 bytes of text, so a section
    # for them would name a place nothing goes. interrupts.cpp is excluded: its
    # one function is a `jmp ($fffc)` reset trampoline that has to stay in the
    # fixed bank. input.cpp is excluded too -- one ~40-byte function called
    # every frame, so it is pinned inline (AI) rather than placed.
    # ------------------------------------------------------------------
    foreach(_mod video)
        string(TOUPPER ${_mod} _MOD)
        if(PNES_${_MOD}_SECTION AND LLVM_MOS_PLATFORM STREQUAL "nes-nrom")
            message(WARNING
                    "PNES_${_MOD}_SECTION is set, but this is an NROM "
                    "build: one fixed 32 KiB bank, nowhere to move code to. "
                    "MODULE_PLACEMENT is inert here, so the section is ignored "
                    "rather than silently costing you the module's inlining.")
        endif()
        if(PNES_${_MOD}_SECTION)
            set_source_files_properties(${CMAKE_SOURCE_DIR}/src/nes/${_mod}.cpp
                    PROPERTIES COMPILE_DEFINITIONS
                    "PNES_${_MOD}_SECTION=\"${PNES_${_MOD}_SECTION}\"")
            message(STATUS "pnes: ${_mod} module placed in "
                           "${PNES_${_MOD}_SECTION} (inlining disabled for it)")
        endif()
    endforeach()

    # LLVM_MOS_PLATFORM itself is set much earlier now (right after
    # CMAKE_TOOLCHAIN_FILE near the top of this file) -- see that block's own
    # comment for why (CMAKE_CXX_COMPILER_FORCED locks in whatever it picks
    # at the FIRST enable_language(CXX), and the uitk-export step's own call
    # happens before this point).
    find_package(llvm-mos-sdk REQUIRED)
    project(pnes CXX)
    set(PLATFORM_DEFINE TARGET_NES "REGION=${REGION}")

    # NROM has no bankswitching, so RESET has nowhere to "escape to" and
    # stays plain int main(). Every other mapper needs its RESET/main pinned
    # to the fixed bank (interrupts.hpp), since main is the code's one
    # guaranteed-always-resident home base while everything else may be
    # banked in and out from under it. LLVM_MOS_PLATFORM is this project's
    # one source of truth for which mapper is being built, set just above.
    if(NOT LLVM_MOS_PLATFORM STREQUAL "nes-nrom")
        list(APPEND PLATFORM_DEFINE NES_MAPPER_BANKSWITCHED)
    endif()

    # Mapper/submapper number for header.hpp's HEADER() macro (see
    # nes2.cpp). This is the only mapper subtarget this branch currently
    # builds (TOPLEVEL_LD is hardcoded above), so it's unconditional for now
    # -- becomes an if/elseif alongside TOPLEVEL_LD once a second mapper
    # subtarget exists, one MAPPER/SUBMAPPER pair per board.
    list(APPEND PLATFORM_DEFINE "MAPPER=4" "SUBMAPPER=0")

    # Alternative nametable wiring (NES2.0 flags 6, bit 3 -- the "four-screen"
    # bit): the board supplies its own nametable memory/routing instead of the
    # console's default 2-way CIRAM mirror (see TVROM's extra VRAM chip, MMC5's
    # ExRAM/fill-mode quadrant select, Sunsoft-4's CHR-as-nametable trick).
    # header.hpp/nes2.hpp only ever tests this for non-zero-ness -- the value
    # itself is not a hardware quantity read out of the header, just a knob a
    # given board's glue code may interpret its own way. Set
    # ALTERNATIVE_NAMETABLE in local.cmake to any non-zero value to set the
    # bit; unset (the default) leaves it clear.
    if(DEFINED ALTERNATIVE_NAMETABLE)
        list(APPEND PLATFORM_DEFINE "ALTERNATIVE_NAMETABLE=${ALTERNATIVE_NAMETABLE}")
    endif()

    # -D-prefixed form for the pass-1 custom command below, which invokes the
    # compiler directly and so doesn't pick up target_compile_definitions.
    list(TRANSFORM PLATFORM_DEFINE PREPEND "-D" OUTPUT_VARIABLE PLATFORM_DEFINE_FLAGS)

    FetchContent_MakeAvailable(intsh tuple)
    _make_consh_available()
    _fixup_fetched_cxx_std_targets()

    # FamiStudio sound engine sources are fetched directly from upstream
    # to avoid vendoring a copy in the repository.
    set(FAMISTUDIO_DIR ${CMAKE_BINARY_DIR}/famistudio)
    set(FAMISTUDIO_BASE_URL "https://raw.githubusercontent.com/BleuBleu/FamiStudio/master/SoundEngine")

    if(NOT EXISTS ${FAMISTUDIO_DIR}/famistudio_ca65.s)
        file(MAKE_DIRECTORY ${FAMISTUDIO_DIR}/NoteTables)

        file(DOWNLOAD ${FAMISTUDIO_BASE_URL}/famistudio_ca65.s ${FAMISTUDIO_DIR}/famistudio_ca65.s)
        file(DOWNLOAD ${FAMISTUDIO_BASE_URL}/famistudio_ca65.h ${FAMISTUDIO_DIR}/famistudio_ca65.h)

        file(DOWNLOAD ${FAMISTUDIO_BASE_URL}/NoteTables/famistudio_note_table_lsb.bin
                ${FAMISTUDIO_DIR}/NoteTables/famistudio_note_table_lsb.bin)
        file(DOWNLOAD ${FAMISTUDIO_BASE_URL}/NoteTables/famistudio_note_table_msb.bin
                ${FAMISTUDIO_DIR}/NoteTables/famistudio_note_table_msb.bin)
        file(DOWNLOAD ${FAMISTUDIO_BASE_URL}/NoteTables/famistudio_note_table_pal_lsb.bin
                ${FAMISTUDIO_DIR}/NoteTables/famistudio_note_table_pal_lsb.bin)
        file(DOWNLOAD ${FAMISTUDIO_BASE_URL}/NoteTables/famistudio_note_table_pal_msb.bin
                ${FAMISTUDIO_DIR}/NoteTables/famistudio_note_table_pal_msb.bin)
    endif()

    # Locate the ca65 assembler and llvm-readelf binary used below. ca65 is
    # part of cc65 (a separate, system-installed toolchain) so it legitimately
    # falls back to PATH. llvm-readelf ships with the SDK: NO_DEFAULT_PATH
    # pins it to the SDK bin (derived from the NES compiler's own directory)
    # so a PATH-resolved llvm-mos can never sneak in.
    find_program(CA65 ca65 HINTS "${CC65_DIR}/bin" "C:/cc65/bin" REQUIRED)
    get_filename_component(LLVM_MOS_BIN ${CMAKE_C_COMPILER} DIRECTORY)
    find_program(LLVM_READELF llvm-readelf
        HINTS "${LLVM_MOS_BIN}" NO_DEFAULT_PATH REQUIRED)
    # Same SDK-pinned lookup as llvm-readelf above; used by the post-link
    # space report (tools/report_space.cmake), which reads region geometry
    # back out of the linker's own __space_* symbols.
    find_program(LLVM_NM llvm-nm
        HINTS "${LLVM_MOS_BIN}" NO_DEFAULT_PATH REQUIRED)

    # ------------------------------------------------------------------
    # lua_log_builder: reads log()'s zero-footprint metadata back out of the
    # linked demo.elf (see include/pnes/logger.hpp) and writes
    # logdata.lua next to demo.nes for tools/pnes.lua to load in Mesen.
    #
    # HOST-NATIVE, NOT CROSS-COMPILED: this whole configuration's own
    # CMAKE_C_COMPILER/CMAKE_CXX_COMPILER are the 6502 cross-compiler (see
    # this file's own NES-target comments above), but this tool has to run
    # ON the machine building the ROM. It is therefore its own separate
    # CMake project (tools/luaLogBuilder/CMakeLists.txt), configured and
    # built here via a NESTED cmake invocation using ${CMAKE_COMMAND} with
    # no toolchain/compiler cache variables forwarded, so it picks up the
    # host's own default compiler exactly like a standalone `cmake -S/-B`
    # invocation would -- never the mos cross-compiler this project's own
    # targets use.
    #
    # BUILT BEFORE ANY PROJECT NES SOURCE COMPILES: add_dependencies() below
    # pins this ahead of pnes, the first target that compiles any of
    # this project's or a consuming project's own NES C++ -- not because the
    # engine's own source needs the tool, but so a developer never watches a
    # full NES build finish only to have logdata.lua generation fail or be
    # silently skipped at the very end.
    set(LUA_LOG_BUILDER_SRC ${CMAKE_SOURCE_DIR}/tools/luaLogBuilder)
    set(LUA_LOG_BUILDER_BUILD ${LUA_LOG_BUILDER_SRC}/build)
    set(LUA_LOG_BUILDER_BIN ${LUA_LOG_BUILDER_BUILD}/lua_log_builder)

    add_custom_command(
            OUTPUT ${LUA_LOG_BUILDER_BIN}
            COMMAND ${CMAKE_COMMAND} -S ${LUA_LOG_BUILDER_SRC} -B ${LUA_LOG_BUILDER_BUILD}
                    -DCMAKE_BUILD_TYPE=Release
            COMMAND ${CMAKE_COMMAND} --build ${LUA_LOG_BUILDER_BUILD}
            DEPENDS ${LUA_LOG_BUILDER_SRC}/main.cpp ${LUA_LOG_BUILDER_SRC}/CMakeLists.txt
            COMMENT "Building lua_log_builder (host-native logdata.lua generator)"
            VERBATIM
    )
    add_custom_target(lua_log_builder_tool DEPENDS ${LUA_LOG_BUILDER_BIN})

    # demo/famistudio_config.s is the REAL, project-owned FamiStudio config
    # file (see its own header comment, and BANKED_CALL_THEORY.txt's
    # "RESOLVED: famistudio_config.s moves to the demo root as a real,
    # directly-edited file" -- this used to be a CMake-generated string
    # instead, which is what that entry flags as the thing to fix). CMake's
    # own job here is reduced to READING values back out of that real file
    # (regex-extracted, never written) so famistudio.cpp's #if FAMISTUDIO_CFG_*
    # checks stay in sync with whatever this file actually says -- editing
    # the .s file is what changes the engine's real configuration; these
    # extracted CMake variables can never drift from it because they're read
    # from it, not a separate source.
    #
    # THE CODE SEGMENT IS THE ONE EXCEPTION, and runs the other way: it is
    # not a free choice of that file's, it MUST equal the section the audio
    # module is compiled into, so it is derived from
    # PNES_AUDIO_SECTION and fed in (see the encoder further down).
    file(READ ${CMAKE_SOURCE_DIR}/demo/famistudio_config.s FAMISTUDIO_CONFIG_TEXT)

    macro(famistudio_extract var pattern)
        string(REGEX MATCH "${pattern}" _famistudio_match "${FAMISTUDIO_CONFIG_TEXT}")
        if(NOT _famistudio_match)
            message(FATAL_ERROR "demo/famistudio_config.s: couldn't find ${var} (expected to match: ${pattern})")
        endif()
        set(${var} ${CMAKE_MATCH_1})
    endmacro()

    famistudio_extract(FAMISTUDIO_CFG_DPCM_SUPPORT "FAMISTUDIO_CFG_DPCM_SUPPORT[ \t]*=[ \t]*([0-9]+)")
    famistudio_extract(FAMISTUDIO_CFG_SFX_SUPPORT  "FAMISTUDIO_CFG_SFX_SUPPORT[ \t]*=[ \t]*([0-9]+)")
    famistudio_extract(FAMISTUDIO_CFG_SFX_STREAMS  "FAMISTUDIO_CFG_SFX_STREAMS[ \t]*=[ \t]*([0-9]+)")
    famistudio_extract(FAMISTUDIO_USE_VOLUME_TRACK "FAMISTUDIO_USE_VOLUME_TRACK[ \t]*=[ \t]*([0-9]+)")

    # NTSC/PAL support are no longer literal digits in famistudio_config.s --
    # that file now computes them itself from the same REGION passed into
    # ca65 below (-D REGION=${REGION}), so they're computed identically here
    # rather than regex-extracted (there's no literal "= 0"/"= 1" left in the
    # file to match).
    set(FAMISTUDIO_CFG_NTSC_SUPPORT 1)
    set(FAMISTUDIO_CFG_PAL_SUPPORT 0)
    if(REGION)
        set(FAMISTUDIO_CFG_NTSC_SUPPORT 0)
        set(FAMISTUDIO_CFG_PAL_SUPPORT 1)
    endif()

    if(FAMISTUDIO_CFG_NTSC_SUPPORT AND FAMISTUDIO_CFG_PAL_SUPPORT)
        message(FATAL_ERROR
                "demo/famistudio_config.s: FAMISTUDIO_CFG_NTSC_SUPPORT and "
                "FAMISTUDIO_CFG_PAL_SUPPORT are both enabled; DUAL mode "
                "requires a runtime region detection routine that this "
                "project does not yet provide.")
    endif()

    file(COPY ${FAMISTUDIO_DIR}/famistudio_ca65.h
            DESTINATION ${CMAKE_BINARY_DIR}/famistudio_include)

    if(EXISTS ${FAMISTUDIO_DIR}/famistudio_llvmmos.h)
        file(COPY ${FAMISTUDIO_DIR}/famistudio_llvmmos.h
                DESTINATION ${CMAKE_BINARY_DIR}/famistudio_include)
    endif()

    # ------------------------------------------------------------------
    # PNES_AUDIO_SECTION -> ca65 segment name.
    #
    # famistudio.cpp's ::AUDIO_BANK and FamiStudio's FAMISTUDIO_CA65_CODE_SEGMENT
    # must name the SAME bank -- that shared placement is the entire reason
    # famistudio.cpp can plain-call famistudio_update() and carry no bank
    # switching. Two hand-written literals could drift apart silently and
    # produce a cross-bank call with no diagnostic, so there is only one:
    # local.cmake's readable ".prg_rom_audio", which link.ld and banks.hpp
    # already use, with the ca65 spelling GENERATED from it here.
    #
    # ld.lld's XO65 reader treats '_' in a ca65 segment name as an escape
    # character, so ".prg_rom_audio" is spelled "_pprg__rom__audio" -- every
    # literal '_' doubled, '.' as "_p" -- plus a "_fx" suffix marking the
    # ELF section executable. Escape literals FIRST or the "_p" introduced by
    # the second replace would itself get doubled by the first.
    # demo/famistudio_config.s documents the full escape table.
    # ------------------------------------------------------------------
    string(REPLACE "_" "__" FAMISTUDIO_SEGMENT "${PNES_AUDIO_SECTION}")
    string(REPLACE "." "_p" FAMISTUDIO_SEGMENT "${FAMISTUDIO_SEGMENT}")
    set(FAMISTUDIO_SEGMENT "${FAMISTUDIO_SEGMENT}_fx")

    # file(CONFIGURE) rather than file(WRITE): it leaves the file untouched
    # when the content is unchanged, so a re-configure doesn't bump the
    # timestamp and force a needless reassembly through the DEPENDS below.
    set(PNES_AUDIO_SEGMENT_INC
            ${CMAKE_BINARY_DIR}/famistudio_include/pnes_audio_segment.inc)
    file(CONFIGURE OUTPUT ${PNES_AUDIO_SEGMENT_INC} CONTENT
"; GENERATED by CMakeLists.txt from PNES_AUDIO_SECTION -- do not edit.
; ${PNES_AUDIO_SECTION} in ca65/XO65 segment spelling.
.define PNES_AUDIO_SEGMENT ${FAMISTUDIO_SEGMENT}
")

    # Assemble the real, project-owned config once. There is no longer a
    # separate "pass 1 without padding, pass 2 with padding" split -- see
    # the measurement step below for why: it was already a permanent no-op,
    # so building the exact same content twice bought nothing. The one
    # object below is used both for the diagnostic measurement link and the
    # real demo link.
    set(FAMISTUDIO_OBJ ${CMAKE_BINARY_DIR}/famistudio.o)
    add_custom_command(
            OUTPUT ${FAMISTUDIO_OBJ}
            COMMAND ${CA65}
            -D REGION=${REGION}
            # Switches famistudio_config.s from its standalone literal
            # fallback to the generated pnes_audio_segment.inc. ca65's -D
            # takes NUMERIC symbols only ("Invalid definition" otherwise),
            # so the segment name itself cannot travel this way -- this flag
            # only selects which branch of the .ifdef there is taken.
            -D PNES_AUDIO_SEGMENT_INC=1
            -I ${CMAKE_SOURCE_DIR}/demo
            -I ${CMAKE_BINARY_DIR}/famistudio_include
            -I ${FAMISTUDIO_DIR}
            -I ${FAMISTUDIO_DIR}/NoteTables
            -o ${FAMISTUDIO_OBJ}
            ${CMAKE_SOURCE_DIR}/demo/famistudio_config.s
            DEPENDS ${CMAKE_SOURCE_DIR}/demo/famistudio_config.s
            ${PNES_AUDIO_SEGMENT_INC}
            COMMENT "Assembling FamiStudio"
    )

    # demo/nes.fms (the real FamiStudio project, edited in the FamiStudio GUI)
    # is the one source of truth for this demo's music -- demo/audio/tracks/
    # nes_audio.s is a regenerated build artifact, not hand-edited, so it's
    # re-exported here from the .fms on every build where the project changed
    # rather than trusted as a stale committed copy. FamiStudio ships its own
    # CLI under this same binary name (case as installed -- "famistudio" on
    # Linux packages, "FamiStudio"/"FamiStudio.exe" on macOS/Windows).
    find_program(FAMISTUDIO NAMES famistudio FamiStudio REQUIRED)

    set(NES_FMS ${CMAKE_SOURCE_DIR}/demo/nes.fms)
    set(NES_AUDIO_ASM ${CMAKE_SOURCE_DIR}/demo/audio/tracks/nes_audio.s)
    add_custom_command(
            OUTPUT ${NES_AUDIO_ASM}
            COMMAND ${FAMISTUDIO} ${NES_FMS} famistudio-asm-export ${NES_AUDIO_ASM}
                    -famistudio-asm-format:ca65
            DEPENDS ${NES_FMS}
            COMMENT "Exporting FamiStudio music: nes.fms -> nes_audio.s (ca65)"
    )
    # nes_sfx.s is NOT regenerated the same way yet: demo/nes.fms currently has
    # no dedicated sound-effect bank matching it (famistudio-asm-sfx-export
    # against the project as it stands today produces each full song
    # re-encoded as one giant synthesized "effect" instead, nothing like the
    # small hand-curated SFX this file actually holds) -- wire it in once the
    # project's SFX bank is sorted out, the same way nes_audio.s is above.

    # demo/famistudio_config.s is handled explicitly above (assembled once
    # into FAMISTUDIO_OBJ) -- excluded here so it isn't ALSO picked up and
    # assembled a second time by this generic "every hand-written .s file"
    # loop, which would produce a second object defining the same symbols.
    file(GLOB_RECURSE DEMO_ASM_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/demo/*.s)
    list(REMOVE_ITEM DEMO_ASM_SOURCES ${CMAKE_SOURCE_DIR}/demo/famistudio_config.s)
    # nes_audio.s (the generated song export) is assembled ONLY through its
    # wrapper, demo/audio/tracks/nes_audio_bank.s, which opens the target
    # PRG-ROM bank's segment and .includes it -- see that file's own comment.
    # Assembling it directly here as well would define every music symbol
    # twice, exactly the clash famistudio_config.s is excluded for above.
    list(REMOVE_ITEM DEMO_ASM_SOURCES ${NES_AUDIO_ASM})
    # nes_sfx.s: same treatment, same reason -- assembled only through
    # demo/audio/sfx/nes_sfx_bank.s, which puts it in the audio DATA bank
    # alongside the music. Engine-read data cannot stay in the resident image:
    # a paired farcall banks that region out for the duration of the call.
    list(REMOVE_ITEM DEMO_ASM_SOURCES ${CMAKE_SOURCE_DIR}/demo/audio/sfx/nes_sfx.s)
    set(DEMO_ASM_OBJECTS "")
    foreach(ASM_SRC ${DEMO_ASM_SOURCES})
        get_filename_component(ASM_NAME ${ASM_SRC} NAME_WE)
        set(ASM_OBJ ${CMAKE_BINARY_DIR}/${ASM_NAME}.o)
        add_custom_command(
                OUTPUT ${ASM_OBJ}
                COMMAND ${CA65}
                -I ${FAMISTUDIO_DIR}
                -I ${FAMISTUDIO_DIR}/NoteTables
                # nes_audio_bank.s .includes the generated nes_audio.s from
                # this directory; ca65 resolves .include against its -I paths.
                -I ${CMAKE_SOURCE_DIR}/demo/audio/tracks
                -I ${CMAKE_SOURCE_DIR}/demo/audio/sfx
                -o ${ASM_OBJ}
                ${ASM_SRC}
                # NES_AUDIO_ASM: the wrapper's .include makes every music
                # object depend on the generated export, which no scan of
                # ${ASM_SRC} alone would reveal.
                DEPENDS ${ASM_SRC} ${NES_AUDIO_ASM}
                COMMENT "Assembling ${ASM_NAME}"
        )
        list(APPEND DEMO_ASM_OBJECTS ${ASM_OBJ})
    endforeach()

    file(GLOB_RECURSE NES_SOURCES CONFIGURE_DEPENDS src/nes/*.cpp src/all/*.cpp)
    # This build targets MMC3 only (TOPLEVEL_LD/MAPPER above) -- vrc1.cpp
    # still exists for a VRC1 project to use, but it also defines a global
    # extern "C" _reset (and other symbols mmc3.cpp defines too), which the
    # glob above would otherwise hand to this same static library: since
    # nothing in an MMC3 build references anything vrc1.cpp provides,
    # leaving it in costs nothing at RUNTIME, but a duplicate-_reset link
    # error is a real risk the moment anything ELSE in the link happens to
    # pull that archive member in. Excluded here, the same pattern already
    # used above for famistudio_config.s's own glob -- becomes conditional
    # (alongside MAPPER/TOPLEVEL_LD) once a real "pick your mapper" build
    # option exists instead of this hardcoded single-target branch.
    list(REMOVE_ITEM NES_SOURCES ${CMAKE_SOURCE_DIR}/src/nes/mappers/vrc1.cpp)
    add_library(pnes STATIC ${NES_SOURCES})

    # lua_log_builder built and ready before any project NES source compiles
    # -- see this file's own comment on lua_log_builder_tool, above.
    add_dependencies(pnes lua_log_builder_tool)

    # Unconditional, in every build type -- not just nes-debug. log()'s whole
    # LMA-resolution scheme (include/pnes/logger.hpp, tools/pnes.lua)
    # depends on DWARF's .debug_line table to turn a log() call site's
    # __FILE__/__LINE__ into a real address; without -g that table doesn't
    # exist and every log point silently resolves to nothing. This costs
    # nothing in the shipped ROM either way: debug sections carry no
    # SHF_ALLOC flag, so they were never going to reach a MEMORY region or
    # get copied into demo.nes by objcopy regardless of build type -- the
    # only thing -g grows is demo.nes.elf, which is not what ships.
    target_compile_options(pnes PUBLIC -g)

    target_include_directories(pnes PUBLIC
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/include/umbrella
            ${CMAKE_BINARY_DIR}/famistudio_include
    )
    target_compile_definitions(pnes PUBLIC
            ${PLATFORM_DEFINE}
            FAMISTUDIO_CFG_NTSC_SUPPORT=${FAMISTUDIO_CFG_NTSC_SUPPORT}
            FAMISTUDIO_CFG_PAL_SUPPORT=${FAMISTUDIO_CFG_PAL_SUPPORT}
            FAMISTUDIO_CFG_DPCM_SUPPORT=${FAMISTUDIO_CFG_DPCM_SUPPORT}
            FAMISTUDIO_CFG_SFX_SUPPORT=${FAMISTUDIO_CFG_SFX_SUPPORT}
            FAMISTUDIO_CFG_SFX_STREAMS=${FAMISTUDIO_CFG_SFX_STREAMS}
            FAMISTUDIO_USE_VOLUME_TRACK=${FAMISTUDIO_USE_VOLUME_TRACK}
            # No FAMISTUDIO_SEGMENT here: famistudio.cpp used to need the engine's
            # segment name for a BANKED_EXTERN tag, and no longer does
            # anything with it -- it reaches the engine with a plain call now
            # that the two share a bank by construction (see the encoder
            # above). PNES_AUDIO_SECTION is the only placement value
            # the C++ side still takes, and it goes to famistudio.cpp alone.
    )
    target_link_libraries(pnes PUBLIC br0::intsh br0::tuple br0::consh)

    # TITLE/TITLE_DATA/SYSMEM, once as a list so both compile paths that see
    # demo/*.cpp stay in sync: the real `demo` target below (normal
    # target_compile_definitions), AND the hand-rolled PASS1_ELF compiler
    # invocation just below, which bypasses every CMake target property
    # (its own comment explains why) and so does NOT inherit anything set on
    # `demo` -- these have to be threaded through explicitly as raw -D flags
    # or uitk-generated demo/ui/**/*.cpp fails this diagnostic build with the
    # very #error guards TITLE/SYSMEM exist to produce. GEN_TARGET_DIR (below,
    # -DGEN_TARGET_DIR=nes on the PASS1_ELF command line itself, since it's
    # the one define here that's NOT also needed by demo/src/banks.hpp) is
    # the same story: title.cpp's own #error guard on it exists for exactly
    # this reason -- catch a compile path that forgot it, loudly, instead of
    # a confusing "file not found" from the #include STRCAT(...) below it.
    # OPTIONS/OPTIONS_DATA: same mechanism as TITLE/TITLE_DATA above
    # (uitk-generated demo/gen/options/*/options.hpp needs them as whole
    # compile definitions, not a header include), but with no local.cmake
    # variable behind them -- the options scene has no per-developer bank
    # choice, it always lives in demo/link.ld's own dedicated prg_rom_options
    # bank (one of the named placeholder banks carved out when PRG-ROM was
    # doubled to 256 KiB), so the section name is a literal here instead.
    set(DEMO_PLACEMENT_DEFINES
            "TITLE=CREATE_SEGMENT_KEYWORD(\"${TITLE}\")"
            "TITLE_DATA=CREATE_SEGMENT_KEYWORD(\"${TITLE}.rodata\")"
            "SYSMEM=CREATE_SEGMENT_KEYWORD(\"${SYSMEM}\")"
            "OPTIONS=CREATE_SEGMENT_KEYWORD(\".prg_rom_options\")"
            "OPTIONS_DATA=CREATE_SEGMENT_KEYWORD(\".prg_rom_options.rodata\")")
    list(TRANSFORM DEMO_PLACEMENT_DEFINES PREPEND "-D" OUTPUT_VARIABLE DEMO_PLACEMENT_FLAGS)

    # demo/gen/title/ holds one uitk-generated title.cpp per console, all
    # sharing the same gen::title:: symbol names -- glob only the one this
    # target actually builds, or every other platform's title.cpp comes
    # along too and the linker sees duplicate symbols for all of them.
    file(GLOB_RECURSE DEMO_C_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/demo/*.cpp)
    list(FILTER DEMO_C_SOURCES EXCLUDE REGEX "/demo/gen/(title|gameOptions)/")
    file(GLOB DEMO_TITLE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/demo/gen/title/nes/*.cpp)
    list(APPEND DEMO_C_SOURCES ${DEMO_TITLE_SOURCES})
    file(GLOB DEMO_GAMEOPTIONS_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/demo/gen/gameOptions/nes/*.cpp)
    list(APPEND DEMO_C_SOURCES ${DEMO_GAMEOPTIONS_SOURCES})

    set(PASS1_ELF ${CMAKE_BINARY_DIR}/demo_pass1.elf)
    add_custom_command(
            OUTPUT ${PASS1_ELF}
            COMMAND ${CMAKE_CXX_COMPILER}
            # This hand-rolled invocation bypasses CMAKE_CXX_STANDARD and the
            # demo target's options, so the standard and the #embed extension
            # silencer must be set explicitly to match the demo target. -x c++
            # forces the C++ frontend for the source inputs.
            -std=gnu++23 -Wno-c23-extensions -x c++
            -include ${CMAKE_SOURCE_DIR}/demo/src/graphics/charmaps.hpp
            -Os ${PLATFORM_DEFINE_FLAGS} ${DEMO_PLACEMENT_FLAGS} -DGEN_TARGET_DIR=nes -D${ORIENTATION}
            # mos-nes.cfg defaults to -mlto-zp=224 (the whole region), letting
            # mos-zero-page-alloc opportunistically promote ordinary globals into
            # zero page with no source-level opt-in. Capped to 0 so zero page is
            # only ever used by what explicitly asks for it: `direct`-tagged C++
            # globals (.zp.bss, technology.hpp) and FamiStudio's own ZEROPAGE
            # segment (famistudio_config.s), neither of which this flag affects.
            -mlto-zp=0
            # This custom command calls the compiler directly, bypassing
            # CMAKE_EXE_LINKER_FLAGS (which supplies -T/-L for every ordinary
            # CMake target's link step) -- so -T/-L have to be repeated here too.
            -T ${TOPLEVEL_LD}
            -L${CMAKE_SOURCE_DIR}/src/nes/mappers
            -I${CMAKE_SOURCE_DIR}/include
            -I${CMAKE_SOURCE_DIR}/include/umbrella
            -I${CMAKE_BINARY_DIR}/famistudio_include
            -I${intsh_SOURCE_DIR}
            -I${tuple_SOURCE_DIR}
            -I${consh_SOURCE_DIR}
            ${DEMO_C_SOURCES}
            # Stop forcing the C++ frontend before the archive/object inputs
            # so the driver treats them as link inputs, not source files.
            -x none
            $<TARGET_FILE:pnes>
            ${FAMISTUDIO_OBJ}
            ${DEMO_ASM_OBJECTS}
            -o ${CMAKE_BINARY_DIR}/demo_pass1
            DEPENDS pnes
            ${FAMISTUDIO_OBJ}
            ${DEMO_ASM_OBJECTS}
            ${DEMO_C_SOURCES}
            COMMENT "Linking diagnostic ELF for ZP/BSS measurement"
            # DEMO_PLACEMENT_FLAGS' entries carry literal ()/" characters
            # (CREATE_SEGMENT_KEYWORD("...")) -- without VERBATIM, CMake
            # passes COMMAND arguments to the underlying generator (here,
            # Ninja's /bin/sh -c) unescaped, so the shell chokes on the bare
            # parens. VERBATIM asks CMake to do the platform-correct
            # escaping instead; every earlier argument here (plain paths/
            # flags, no shell metacharacters) is unaffected by turning it on.
            VERBATIM
    )

    # ------------------------------------------------------------------
    # Diagnostic only: report how much ZP/BSS the C++ side actually uses.
    # Informational -- measure_sections.cmake's own header comment explains
    # why this can never feed back into a real padding step (llvm-mos's own
    # linker script already places ca65's ZEROPAGE/BSS after the C-side
    # sections via wildcard order; adding real padding here would double-
    # count the reservation). Nothing depends on this file's contents.
    # ------------------------------------------------------------------

    set(CA65_PADDING ${CMAKE_BINARY_DIR}/ca65_padding.s)
    add_custom_command(
            OUTPUT ${CA65_PADDING}
            COMMAND ${CMAKE_COMMAND}
            -DREADELF=${LLVM_READELF}
            -DELF_FILE=${PASS1_ELF}
            -DOUTPUT_FILE=${CA65_PADDING}
            -P ${CMAKE_SOURCE_DIR}/tools/measure_sections.cmake
            DEPENDS ${PASS1_ELF}
            ${CMAKE_SOURCE_DIR}/tools/measure_sections.cmake
            COMMENT "Extracting ZP/BSS sizes (diagnostic only)"
    )

    # ------------------------------------------------------------------
    # Final link
    # ------------------------------------------------------------------

    # chr_rom.cpp #embeds the packed blob (demo/chr/chr.bin) and emits it as the
    # cartridge's CHR ROM image. The blob is a blobman output, not a source file,
    # so declare it explicitly: repacking it must retrigger that compile.
    set_source_files_properties(${CMAKE_SOURCE_DIR}/demo/src/graphics/chr_rom.cpp
            PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/demo/chr/chr.bin")

    add_executable(demo ${DEMO_C_SOURCES})
    set_target_properties(demo PROPERTIES SUFFIX ".nes")

    target_link_libraries(demo PRIVATE pnes)

    # Bareword (unquoted) macro naming this target's own demo/gen/*/<dir>
    # subdirectory -- consumed via ::STRCAT (pnes/technology.hpp) for
    # a computed #include, e.g. `#include STRCAT(../../gen/title/GEN_TARGET_DIR/title.hpp)`,
    # not a search-path trick. Value always matches DEMO_TITLE_DIR (hardcoded
    # "nes" here, same as the glob above, since this branch never sets that
    # variable).
    target_compile_definitions(demo PRIVATE GEN_TARGET_DIR=nes)

    # TITLE/TITLE_DATA/SYSMEM as real, whole-macro compile definitions (not
    # just the section-name strings AUDIO/UI_SECTION feed into a locally
    # #define'd macro) -- so they reach every demo .cpp/generated header
    # without depending on banks.hpp having been #included first in the
    # right order. Same list PASS1_ELF's raw compiler invocation above was
    # given as -D flags, kept as one DEMO_PLACEMENT_DEFINES source of truth.
    target_compile_definitions(demo PRIVATE ${DEMO_PLACEMENT_DEFINES})

    # CHR art is pulled in with #embed (C23), which Clang flags as an extension
    # in C++ mode. It is the intended mechanism here, so silence the diagnostic.
    target_compile_options(demo PRIVATE -Wno-c23-extensions)

    add_dependencies(demo demo_famistudio_asm)

    # CA65_PADDING is listed here purely so the diagnostic ZP/BSS measurement
    # (informational only -- see its own comment above) actually runs as
    # part of a normal build; nothing downstream reads its output.
    add_custom_target(demo_famistudio_asm
            DEPENDS ${FAMISTUDIO_OBJ} ${DEMO_ASM_OBJECTS} ${CA65_PADDING}
    )

    # ------------------------------------------------------------------
    # Post-link diagnostic: how full each memory region ended up. Purely
    # informational (see tools/report_space.cmake's own header) -- a region
    # that actually overflows is a hard linker error well before this runs,
    # so this exists to show how much room is LEFT, especially in banks that
    # hold nothing yet and therefore emit no section at all.
    # ------------------------------------------------------------------
    add_custom_command(TARGET demo POST_BUILD
            COMMAND ${CMAKE_COMMAND}
            -DREADELF=${LLVM_READELF}
            -DNM=${LLVM_NM}
            -DELF_FILE=$<TARGET_FILE:demo>.elf
            -P ${CMAKE_SOURCE_DIR}/tools/report_space.cmake
            VERBATIM
    )

    # ------------------------------------------------------------------
    # logdata.lua generation. See include/pnes/logger.hpp and
    # tools/luaLogBuilder/main.cpp's own header comments for the whole
    # design -- this just runs the already-built host tool (see
    # lua_log_builder_tool, above) against the ELF this link just produced,
    # writing logdata.lua next to demo.nes itself (where tools/pnes.lua
    # expects to find it -- it resolves the path from the running ROM's own
    # location, not a fixed one). add_dependencies() ensures the tool exists
    # before this runs even on a from-clean build.
    # ------------------------------------------------------------------
    add_dependencies(demo lua_log_builder_tool)
    add_custom_command(TARGET demo POST_BUILD
            COMMAND ${LUA_LOG_BUILDER_BIN}
            --elf $<TARGET_FILE:demo>.elf
            --output $<TARGET_FILE_DIR:demo>/logdata.lua
            VERBATIM
    )

    # ------------------------------------------------------------------
    # .mlb fixup, so Mesen can name symbols in the profiler / debugger /
    # trace logger instead of showing bare PRG addresses.
    #
    # The llvm-mos SDK emits demo.mlb itself, with `G:` code addresses as raw
    # ELF VMAs -- $e000 for fixed-bank code, and the bank-encoded pseudo-VMAs
    # ($18000, $5826c, $8a400) for banked domains. Mesen wants PRG FILE
    # OFFSETS, which is what its profiler reports as `PRG: $xxxxx`, so the
    # SDK's file names nothing. This rewrites them in place.
    #
    # The script re-derives each offset from demo/link.ld's documented
    # (bank + 1) << 16 encoding and then BYTE-VERIFIES every one against the
    # linked ROM, failing the build on a mismatch. That matters because
    # link.ld's own comment warns the ORIGIN encoding and the FULL() order are
    # "kept in agreement by hand" -- so this step doubles as a check that they
    # still are. Runs after the link, so it sees the .nes this build produced.
    # ------------------------------------------------------------------
    # OPTIONAL, deliberately: nothing else in this build needs Python (the
    # Tiled exporters under demo/tiled are run by hand), so a missing
    # interpreter must not break a ROM build. Without it you just get the
    # SDK's unusable .mlb and have to resolve captures by hand.
    find_program(PNES_PYTHON3 NAMES python3 python)
    if(PNES_PYTHON3)
        add_custom_command(TARGET demo POST_BUILD
                COMMAND ${PNES_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/fix_mlb.py
                $<TARGET_FILE_DIR:demo>/$<TARGET_FILE_BASE_NAME:demo>
                VERBATIM
        )
    else()
        message(STATUS "pnes: python3 not found -- skipping the .mlb fixup, so "
                       "Mesen will show bare PRG addresses instead of symbol "
                       "names (tools/fix_mlb.py can be run by hand later)")
    endif()

    # FamiStudio and the hand-written assembly objects are required by
    # every build configuration.
    target_link_options(demo PRIVATE
            -mlto-zp=0   # match pass-1 cap; disables mos-zero-page-alloc entirely, see pass-1's comment
            ${FAMISTUDIO_OBJ}
            ${DEMO_ASM_OBJECTS}
    )

    # ----------------------------------------------------------------------
    # GameCube + Wii target (libogc / GX), via the devkitPPC toolchain.
    #
    # One backend (src/ogc) serves both consoles -- they share the Gekko/Broadway
    # CPU and the Flipper/Hollywood GX pipeline; only input and a couple of build
    # flags differ. The Wii .dol additionally runs unmodified on Wii U via vWii,
    # so this single branch covers all three with the bar set at a Homebrew app.
    #
    # The toolchain file (GameCube.cmake / Wii.cmake) is supplied via the
    # gc-release / wii-release presets; it sets CMAKE_SYSTEM_NAME and, through
    # Platform/libogc.cmake, the arch flags, the -logc -lm link line (plus
    # -lwiiuse -lbte on Wii) and the ogc_create_dol() helper used below.
    # ----------------------------------------------------------------------
elseif(TARGET_PLATFORM STREQUAL "gc" OR TARGET_PLATFORM STREQUAL "gc_pal" OR TARGET_PLATFORM STREQUAL "wii")
    project(pnes CXX)

    FetchContent_MakeAvailable(intsh tuple)
    _make_consh_available()
    _fixup_fetched_cxx_std_targets()

    # gc_pal builds exactly the "gc" branch below (same PLATFORM_DEFINE/
    # DEMO_TITLE_DIR -- there is no separate TARGET_GC_PAL define, same as
    # nes_pal above has no TARGET_NES_PAL) -- it only flips REGION to 1, which
    # src/ogc/video.cpp's irq::init() reads to decide whether to keep the
    # console's native 50Hz PAL video mode instead of forcing NTSC 60Hz (see
    # that function's own comment).
    if(TARGET_PLATFORM STREQUAL "gc_pal")
        set(REGION 1)
    endif()

    if(TARGET_PLATFORM STREQUAL "gc" OR TARGET_PLATFORM STREQUAL "gc_pal")
        set(PLATFORM_DEFINE TARGET_GC)
        set(DEMO_TITLE_DIR gcn)
    else()
        set(PLATFORM_DEFINE TARGET_WII)
        set(DEMO_TITLE_DIR wii)
    endif()

    # Alternative nametable wiring (see the TARGET_NES branch's own comment,
    # above): a fact about which board a build is modeling, not something
    # exclusive to the NES target itself, so this desktop/console preview
    # needs the same define to route mmc3.cpp's strong ppu::ReadNametable/
    # WriteNametable overrides correctly off-NES too.
    if(DEFINED ALTERNATIVE_NAMETABLE)
        list(APPEND PLATFORM_DEFINE "ALTERNATIVE_NAMETABLE=${ALTERNATIVE_NAMETABLE}")
    endif()

    # The OGC backend presents the shared software PPU (src/emu) through GX: the
    # emulator and all the nametable/OAM/palette write functions are
    # platform-agnostic and compiled here alongside the GX-specific
    # video/present, the PAD/WPAD input, and the (stub) audio.
    file(GLOB OGC_SOURCES CONFIGURE_DEPENDS src/ogc/*.cpp src/emu/*.cpp src/emu/mappers/*.cpp)
    file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/all/*.cpp)
    add_library(pnes STATIC ${OGC_SOURCES} ${ALL_SOURCES})
    target_include_directories(pnes PUBLIC
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/include/umbrella
    )
    # TARGET_OGC is the shared "GameCube-or-Wii" group the engine headers gate on
    # (emulated PPU like SDL, but a fixed viewport and no SDL headers); TARGET_GC
    # / TARGET_WII select the console-specific bits (e.g. WPAD on Wii only).
    target_compile_definitions(pnes PUBLIC ${PLATFORM_DEFINE} TARGET_OGC)

    # The on-target statistical PC sampler + on-screen overlay (src/ogc/
    # profiler.cpp and the overlay path in video.cpp) is a development tool, not
    # a shipping feature: it spins up a high-priority sampler thread and draws a
    # hex/bar overlay every frame. Compile it only into Debug builds via the
    # OGC_PROFILER gate, so Release .dols carry neither the thread nor the draw.
    # PRIVATE: only the library's own TUs (profiler.cpp/video.cpp) consult it.
    target_compile_definitions(pnes PRIVATE $<$<CONFIG:Debug>:OGC_PROFILER>)
    # asnd: libogc's ASND audio library (src/ogc/audio.cpp), not pulled in by
    # OGC_STANDARD_LIBRARIES (-logc -lm) on its own.
    target_link_libraries(pnes PUBLIC br0::intsh br0::tuple br0::consh asnd)

    # The music is EMBEDDED in the .dol, not loaded from disk: a Homebrew
    # Channel/loader .dol has no guaranteed content path the way a packaged
    # Wii U title does, so this mirrors the Wii U backend's approach exactly
    # (src/wiiu's own comment, above) -- encode the desktop WAV to a small
    # Vorbis OGG and #embed it (src/ogc/audio.cpp decodes it with stb_vorbis
    # at runtime, streaming into ASND voice buffers since libogc has no SDL
    # audio device to hand a callback to). Unlike the Wii U path this does NOT
    # force 48kHz: ASND_SetVoice() takes the source rate directly and mixes
    # at whatever rate it's given, so the OGG stays at the WAV's native 44.1kHz.
    find_program(FFMPEG ffmpeg REQUIRED)
    FetchContent_Declare(
            stb
            GIT_REPOSITORY https://github.com/nothings/stb.git
            GIT_TAG        master
            GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(stb)
    target_include_directories(pnes PRIVATE ${stb_SOURCE_DIR})

    set(OGC_ASSET_DIR ${CMAKE_BINARY_DIR}/ogc-assets)
    set(OGC_OGG ${OGC_ASSET_DIR}/pc_audio.ogg)
    set(OGC_WAV ${CMAKE_SOURCE_DIR}/demo/audio/tracks/pc_audio.wav)
    file(MAKE_DIRECTORY ${OGC_ASSET_DIR})
    add_custom_command(
            OUTPUT ${OGC_OGG}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${OGC_ASSET_DIR}
            COMMAND ${FFMPEG} -y -loglevel error -i ${OGC_WAV}
                    -ar 44100 -ac 2 -c:a libvorbis -q:a 4 ${OGC_OGG}
            DEPENDS ${OGC_WAV}
            COMMENT "Encoding GC/Wii audio: pc_audio.wav -> pc_audio.ogg (Vorbis, embedded)"
            VERBATIM)
    add_custom_target(ogc-assets DEPENDS ${OGC_OGG})
    add_dependencies(pnes ogc-assets)
    # audio.cpp resolves `#embed <pc_audio.ogg>` via --embed-dir (the canonical
    # GCC mechanism; -I is not reliably searched for #embed). Scope both the
    # search dir and the rebuild-on-change dependency to that one source.
    set_source_files_properties(${CMAKE_SOURCE_DIR}/src/ogc/audio.cpp
            PROPERTIES
                OBJECT_DEPENDS  ${OGC_OGG}
                COMPILE_OPTIONS "--embed-dir=${OGC_ASSET_DIR}")

    file(GLOB_RECURSE DEMO_SOURCES CONFIGURE_DEPENDS demo/*.cpp)
    list(FILTER DEMO_SOURCES EXCLUDE REGEX "/demo/gen/(title|gameOptions)/")
    file(GLOB DEMO_TITLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/demo/gen/title/${DEMO_TITLE_DIR}/*.cpp")
    list(APPEND DEMO_SOURCES ${DEMO_TITLE_SOURCES})
    file(GLOB DEMO_GAMEOPTIONS_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/demo/gen/gameOptions/${DEMO_TITLE_DIR}/*.cpp")
    list(APPEND DEMO_SOURCES ${DEMO_GAMEOPTIONS_SOURCES})

    # chr_rom.cpp #embeds the packed blob (demo/chr/chr.bin) and emits it as the
    # cartridge's CHR ROM image. The blob is a blobman output, not a source file,
    # so declare it explicitly: repacking it must retrigger that compile.
    set_source_files_properties(${CMAKE_SOURCE_DIR}/demo/src/graphics/chr_rom.cpp
            PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/demo/chr/chr.bin")

    add_executable(demo ${DEMO_SOURCES})
    set_target_properties(demo PROPERTIES SUFFIX ".elf")
    target_link_libraries(demo PRIVATE pnes)

    # See the TARGET_NES branch's own comment on this same call for
    # GEN_TARGET_DIR. Quoted: DEMO_TITLE_DIR is "wii u" (a space) on the Wii
    # branch below, and an unquoted argument here would split on that space
    # into two separate (wrong) compile definitions.
    target_compile_definitions(demo PRIVATE "GEN_TARGET_DIR=${DEMO_TITLE_DIR}")

    # CHR art is pulled in with #embed (C23). devkitPPC's GCC supports it without
    # a diagnostic here; keep the Clang-only silencer guard for parity with the
    # other branches (no-op under GCC).
    target_compile_options(demo PRIVATE $<$<CXX_COMPILER_ID:Clang>:-Wno-c23-extensions>)

    # Enable LTO for release-type configs. The software PPU is the hot path and
    # spans the pnes static lib (emu::GenerateFrame) and the demo (the
    # per-frame IRQ handlers it calls back into); cross-module inlining matters
    # here. CMake routes archiving through gcc-ar/gcc-ranlib once
    # INTERPROCEDURAL_OPTIMIZATION is set, so the .dol-from-.elf step is
    # unaffected. Mirrors the desktop branch below.
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
    if(_ipo_ok)
        foreach(_cfg Release MinSizeRel RelWithDebInfo)
            string(TOUPPER ${_cfg} _CFG)
            set_property(TARGET demo pnes
                    PROPERTY INTERPROCEDURAL_OPTIMIZATION_${_CFG} ON)
        endforeach()
    else()
        message(STATUS "IPO/LTO not supported by this toolchain: ${_ipo_err}")
    endif()

    # Convert the linked ELF into a bootable .dol (Homebrew Channel / loader
    # format). Defined by Platform/libogc.cmake once the OGC toolchain is active.
    ogc_create_dol(demo)

    # ----------------------------------------------------------------------
    # Nintendo 3DS target (libctru + citro2d), via the devkitARM toolchain.
    #
    # The 3DS presents the shared software PPU (src/emu) like the SDL backend:
    # emu::GenerateFrame rasterises one ARGB frame on the ARM11, which is
    # uploaded as a single RGBA8 texture and scaled to the top screen by
    # citro2d. The PICA200 has no paletted-texture format, so the GX/CI4 tilemap
    # path the GameCube/Wii branch uses cannot port -- a CPU raster into one
    # texture is the natural fit.
    #
    # The toolchain file (3DS.cmake) is supplied via the ctr-* presets; it sets
    # CMAKE_SYSTEM_NAME=Nintendo3DS and, through Platform/devkitArm.cmake, the
    # arch flags and the ctr_generate_smdh() / ctr_create_3dsx() helpers used
    # below. citro2d/citro3d/ctru are linked explicitly (the toolchain does not
    # add them implicitly the way libogc.cmake adds -logc).
    # ----------------------------------------------------------------------
elseif(TARGET_PLATFORM STREQUAL "ctr")
    project(pnes CXX)

    FetchContent_MakeAvailable(intsh tuple)
    _make_consh_available()
    _fixup_fetched_cxx_std_targets()

    set(DEMO_TITLE_DIR 3ds)

    # The 3DS backend presents the shared software PPU (src/emu) through citro2d:
    # the emulator and all the nametable/OAM/palette write functions are
    # platform-agnostic and compiled here alongside the citro2d video/present,
    # the hid input, and the (stub) audio.
    file(GLOB CTR_SOURCES CONFIGURE_DEPENDS src/3ds/*.cpp src/emu/*.cpp src/emu/mappers/*.cpp)
    file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/all/*.cpp)
    add_library(pnes STATIC ${CTR_SOURCES} ${ALL_SOURCES})
    target_include_directories(pnes PUBLIC
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/include/umbrella
    )
    # TARGET_CTR is the group the engine headers gate on (emulated PPU like SDL,
    # but a fixed 32x30 viewport and no SDL headers -- same family as TARGET_OGC).
    target_compile_definitions(pnes PUBLIC TARGET_CTR)
    # Alternative nametable wiring (see the TARGET_NES branch's own comment,
    # near the top of this file): a fact about which board a build is
    # modeling, not something exclusive to the NES target itself.
    if(DEFINED ALTERNATIVE_NAMETABLE)
        target_compile_definitions(pnes PUBLIC "ALTERNATIVE_NAMETABLE=${ALTERNATIVE_NAMETABLE}")
    endif()
    target_link_libraries(pnes PUBLIC br0::intsh br0::tuple br0::consh)

    file(GLOB_RECURSE DEMO_SOURCES CONFIGURE_DEPENDS demo/*.cpp)
    list(FILTER DEMO_SOURCES EXCLUDE REGEX "/demo/gen/(title|gameOptions)/")
    file(GLOB DEMO_TITLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/demo/gen/title/${DEMO_TITLE_DIR}/*.cpp")
    list(APPEND DEMO_SOURCES ${DEMO_TITLE_SOURCES})
    file(GLOB DEMO_GAMEOPTIONS_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/demo/gen/gameOptions/${DEMO_TITLE_DIR}/*.cpp")
    list(APPEND DEMO_SOURCES ${DEMO_GAMEOPTIONS_SOURCES})

    # chr_rom.cpp #embeds the packed blob (demo/chr/chr.bin) and emits it as the
    # cartridge's CHR ROM image. The blob is a blobman output, not a source file,
    # so declare it explicitly: repacking it must retrigger that compile.
    set_source_files_properties(${CMAKE_SOURCE_DIR}/demo/src/graphics/chr_rom.cpp
            PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/demo/chr/chr.bin")

    # THE `demo` TARGET IS THE PACKAGED DELIVERABLE, NOT THE BARE ELF.
    # devkitPro's packaging helpers (ctr_create_3dsx, nx_create_nro,
    # wut_create_wuhb, nds_create_rom) each add their OWN separate ALL target on
    # top of the executable, so building just `demo` used to stop at demo.elf and
    # leave demo_3dsx/demo_nro/demo_wuhb/demo_nds (and the .smdh/.nacp they pull
    # in) as extra targets to build by hand. A target can't depend on something
    # that depends on itself, so the linked ELF is `demo_elf` here (its output is
    # still demo.elf, via OUTPUT_NAME) and the helper is handed the name `demo`
    # with TARGET pointing at the ELF: `--target demo` now yields the final
    # artifact. Every console branch that packages through such a helper does
    # this; the ones that package with a POST_BUILD step (GC/Wii, PSP, GBA,
    # NES, desktop) already had one target and are unchanged.
    add_executable(demo_elf ${DEMO_SOURCES})
    set_target_properties(demo_elf PROPERTIES OUTPUT_NAME demo SUFFIX ".elf")
    target_link_libraries(demo_elf PRIVATE pnes citro2d citro3d ctru m)

    # See the TARGET_NES branch's own comment on this same call for GEN_TARGET_DIR.
    target_compile_definitions(demo_elf PRIVATE "GEN_TARGET_DIR=${DEMO_TITLE_DIR}")

    # CHR art is pulled in with #embed (C23). devkitARM's GCC supports it without
    # a diagnostic here; keep the Clang-only silencer guard for parity with the
    # other branches (no-op under GCC).
    target_compile_options(demo_elf PRIVATE $<$<CXX_COMPILER_ID:Clang>:-Wno-c23-extensions>)

    # Enable LTO for release-type configs, mirroring the OGC + desktop branches:
    # the software PPU (emu::GenerateFrame) spans the static lib and the demo's
    # per-frame IRQ handlers, so cross-module inlining matters on the hot path.
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
    if(_ipo_ok)
        foreach(_cfg Release MinSizeRel RelWithDebInfo)
            string(TOUPPER ${_cfg} _CFG)
            set_property(TARGET demo_elf pnes
                    PROPERTY INTERPROCEDURAL_OPTIMIZATION_${_CFG} ON)
        endforeach()
    else()
        message(STATUS "IPO/LTO not supported by this toolchain: ${_ipo_err}")
    endif()

    # Wrap the linked ELF as a .3dsx (homebrew loader format) with an SMDH icon/
    # metadata banner. Both helpers come from devkitARM's Tools3DS.cmake once the
    # 3DS toolchain is active. The packaging target is named `demo` (see the
    # note on add_executable above); demo.smdh is only a custom-command output
    # it depends on, not a target of its own. TARGET names the ELF and, since
    # OUTPUT isn't given, the helper writes ${CMAKE_BINARY_DIR}/demo.3dsx.
    ctr_generate_smdh(demo.smdh
            NAME "pnes demo"
            DESCRIPTION "NES engine demo on 3DS (citro2d)"
            AUTHOR "brette"
    )
    ctr_create_3dsx(demo
            TARGET demo_elf
            SMDH ${CMAKE_BINARY_DIR}/demo.smdh
    )

    # ----------------------------------------------------------------------
    # Nintendo Switch target (libnx), via the devkitA64 toolchain.
    #
    # The Switch presents the shared software PPU (src/emu) like the SDL/3DS
    # backends: emu::GenerateFrame rasterises a 256x240 ARGB frame and the CPU
    # integer-scales it into the libnx framebuffer. The Tegra is wildly
    # overpowered for NES-resolution pixels, so a straight scale-blit avoids
    # pulling in deko3d/EGL for this first pass.
    #
    # The toolchain file (Switch.cmake) is supplied via the switch-* presets; it
    # sets CMAKE_SYSTEM_NAME=NintendoSwitch and, through Platform/NXTools.cmake,
    # the arch flags and the nx_generate_nacp() / nx_create_nro() helpers used
    # below. libnx is linked explicitly (the toolchain does not auto-link it).
    # ----------------------------------------------------------------------
    elseif(TARGET_PLATFORM STREQUAL "switch")
        project(pnes CXX)

        FetchContent_MakeAvailable(intsh tuple)
        _make_consh_available()
        _fixup_fetched_cxx_std_targets()

        set(DEMO_TITLE_DIR switch)

        # The Switch backend presents the shared software PPU (src/emu): the
        # emulator and all the nametable/OAM/palette write functions are
        # platform-agnostic and compiled here alongside the libnx framebuffer
        # video/present, the hid input, and the audout audio.
        file(GLOB NX_SOURCES CONFIGURE_DEPENDS src/switch/*.cpp src/emu/*.cpp src/emu/mappers/*.cpp)
        file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/all/*.cpp)
        add_library(pnes STATIC ${NX_SOURCES} ${ALL_SOURCES})
        target_include_directories(pnes PUBLIC
                ${CMAKE_SOURCE_DIR}/include
                ${CMAKE_SOURCE_DIR}/include/umbrella
        )
        # TARGET_NX is the group the engine headers gate on (emulated PPU, fixed
        # 32x30 viewport, no SDL headers -- same family as TARGET_OGC/TARGET_CTR).
        target_compile_definitions(pnes PUBLIC TARGET_NX)
        # Alternative nametable wiring (see the TARGET_NES branch's own comment,
        # near the top of this file): a fact about which board a build is
        # modeling, not something exclusive to the NES target itself.
        if(DEFINED ALTERNATIVE_NAMETABLE)
            target_compile_definitions(pnes PUBLIC "ALTERNATIVE_NAMETABLE=${ALTERNATIVE_NAMETABLE}")
        endif()
        target_link_libraries(pnes PUBLIC br0::intsh br0::tuple br0::consh)

        file(GLOB_RECURSE DEMO_SOURCES CONFIGURE_DEPENDS demo/*.cpp)
        list(FILTER DEMO_SOURCES EXCLUDE REGEX "/demo/gen/(title|gameOptions)/")
        file(GLOB DEMO_TITLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/demo/gen/title/${DEMO_TITLE_DIR}/*.cpp")
        list(APPEND DEMO_SOURCES ${DEMO_TITLE_SOURCES})
        file(GLOB DEMO_GAMEOPTIONS_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/demo/gen/gameOptions/${DEMO_TITLE_DIR}/*.cpp")
        list(APPEND DEMO_SOURCES ${DEMO_GAMEOPTIONS_SOURCES})

        # chr_rom.cpp #embeds the packed blob (demo/chr/chr.bin) and emits it as the
        # cartridge's CHR ROM image. The blob is a blobman output, not a source file,
        # so declare it explicitly: repacking it must retrigger that compile.
        set_source_files_properties(${CMAKE_SOURCE_DIR}/demo/src/graphics/chr_rom.cpp
                PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/demo/chr/chr.bin")

        # `demo` is the packaged .nro, `demo_elf` the linked ELF -- see the note
        # on the 3DS branch's add_executable for why.
        add_executable(demo_elf ${DEMO_SOURCES})
        set_target_properties(demo_elf PROPERTIES OUTPUT_NAME demo SUFFIX ".elf")
        target_link_libraries(demo_elf PRIVATE pnes nx)

        # See the TARGET_NES branch's own comment on this same call for GEN_TARGET_DIR.
        target_compile_definitions(demo_elf PRIVATE "GEN_TARGET_DIR=${DEMO_TITLE_DIR}")

        # CHR art is pulled in with #embed (C23); keep the Clang-only silencer for
        # parity with the other branches (no-op under devkitA64's GCC).
        target_compile_options(demo_elf PRIVATE $<$<CXX_COMPILER_ID:Clang>:-Wno-c23-extensions>)

        # Enable LTO for release-type configs, mirroring the other branches.
        include(CheckIPOSupported)
        check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
        if(_ipo_ok)
            foreach(_cfg Release MinSizeRel RelWithDebInfo)
                string(TOUPPER ${_cfg} _CFG)
                set_property(TARGET demo_elf pnes
                        PROPERTY INTERPROCEDURAL_OPTIMIZATION_${_CFG} ON)
            endforeach()
        else()
            message(STATUS "IPO/LTO not supported by this toolchain: ${_ipo_err}")
        endif()

        # libnx audout is fixed at 48kHz/2ch/s16, so encode the desktop WAV to
        # exactly that raw PCM at build time and bake it into the .nro RomFS
        # (src/switch/audio.cpp streams it). ffmpeg is provided by
        # docker/Dockerfile.switch. This leaves demo/audio/tracks (the lossless
        # WAV) untouched for desktop -- the conversion output lives only in the
        # build tree.
        find_program(FFMPEG ffmpeg REQUIRED)
        set(NX_ROMFS ${CMAKE_BINARY_DIR}/romfs)
        set(NX_RAW   ${NX_ROMFS}/tracks/pc_audio.raw)
        set(NX_WAV   ${CMAKE_SOURCE_DIR}/demo/audio/tracks/pc_audio.wav)
        # nx_create_nro() validates the RomFS dir exists at configure time, but
        # the audio asset inside it is only produced at build time (below). Create
        # the directory now so configure passes; the build step then fills it.
        file(MAKE_DIRECTORY ${NX_ROMFS}/tracks)
        add_custom_command(
                OUTPUT ${NX_RAW}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${NX_ROMFS}/tracks
                COMMAND ${FFMPEG} -y -loglevel error -i ${NX_WAV}
                        -ar 48000 -ac 2 -f s16le ${NX_RAW}
                DEPENDS ${NX_WAV}
                COMMENT "Encoding Switch audio: pc_audio.wav -> RomFS pc_audio.raw (48k s16le)"
                VERBATIM)
        add_custom_target(switch-assets DEPENDS ${NX_RAW})
        add_dependencies(demo_elf switch-assets)

        # Wrap the linked ELF as a homebrew .nro with a NACP metadata banner and
        # the RomFS asset tree. Both helpers come from devkitA64's NXTools.cmake
        # once the Switch toolchain is active. The packaging target is `demo`;
        # demo.nacp is only a custom-command output it depends on, not a target,
        # and (no OUTPUT given) the .nro lands at ${CMAKE_BINARY_DIR}/demo.nro.
        nx_generate_nacp(demo.nacp
                NAME "pnes demo"
                AUTHOR "brette"
                VERSION "1.0.0"
        )
        nx_create_nro(demo
                TARGET demo_elf
                NACP ${CMAKE_BINARY_DIR}/demo.nacp
                ROMFS ${NX_ROMFS}
        )

    # ----------------------------------------------------------------------
    # Native Wii U target (devkitPPC + wut + the SDL2 Wii U portlib).
    #
    # This is the NATIVE Cafe OS target (.rpx / .wuhb), distinct from the Wii
    # .dol the gc/wii branch above produces (which runs on Wii U only through
    # vWii). The Espresso is wildly overpowered for an NES frame, so -- like the
    # Switch/3DS branches -- this presents the shared software PPU (src/emu):
    # emu::GenerateFrame rasterises one widescreen ARGB frame straight into an
    # SDL2 streaming texture that the GPU scales to fill the window.
    #
    # The whole backend (src/wiiu) is ordinary SDL2: the devkitPro wiiu-sdl2
    # portlib wraps GX2 + ProcUI + the TV scan buffer + VPAD/KPAD controllers
    # internally. This is SDL2 -- separate from the SDL3 desktop backend -- so no
    # code is shared with src/SDL3.
    #
    # The toolchain file (WiiU.cmake) is supplied via the wiiu-* presets; it sets
    # CMAKE_SYSTEM_NAME=CafeOS and, through Platform/CafeOS.cmake, the Espresso
    # arch flags and the wut_create_rpx() / wut_create_wuhb() helpers used below.
    # ----------------------------------------------------------------------
    elseif(TARGET_PLATFORM STREQUAL "wiiu")
        project(pnes CXX)

        FetchContent_MakeAvailable(intsh tuple)
        _make_consh_available()
        _fixup_fetched_cxx_std_targets()

        set(DEMO_TITLE_DIR "wii u")

        # The SDL2 Wii U portlib (devkitPro package wiiu-sdl2). Its CMake config
        # provides the SDL2::SDL2-static imported target, whose interface pulls in
        # the GX2/coreinit/VPAD system libs it wraps.
        find_package(SDL2 REQUIRED)

        # The Wii U backend presents the shared software PPU (src/emu): the
        # emulator and all the nametable/OAM/palette write functions are
        # platform-agnostic and compiled here alongside the SDL2 video/present,
        # the SDL2 GameController input, and the SDL2 audio.
        file(GLOB WIIU_SOURCES CONFIGURE_DEPENDS src/wiiu/*.cpp src/emu/*.cpp src/emu/mappers/*.cpp)
        file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/all/*.cpp)
        add_library(pnes STATIC ${WIIU_SOURCES} ${ALL_SOURCES})
        target_include_directories(pnes PUBLIC
                ${CMAKE_SOURCE_DIR}/include
                ${CMAKE_SOURCE_DIR}/include/umbrella
        )
        # TARGET_WIIU is the native-Wii-U define; the engine headers pair it with
        # the Switch (TARGET_NX) family for the widescreen 52x30 (416x240)
        # viewport, and exclude the SDL3 headers (this backend uses SDL2).
        target_compile_definitions(pnes PUBLIC TARGET_WIIU)
        # Alternative nametable wiring (see the TARGET_NES branch's own comment,
        # near the top of this file): a fact about which board a build is
        # modeling, not something exclusive to the NES target itself.
        if(DEFINED ALTERNATIVE_NAMETABLE)
            target_compile_definitions(pnes PUBLIC "ALTERNATIVE_NAMETABLE=${ALTERNATIVE_NAMETABLE}")
        endif()
        target_link_libraries(pnes PUBLIC SDL2::SDL2-static br0::intsh br0::tuple br0::consh)

        # The music is EMBEDDED in the .rpx, not loaded from disk: a native Wii U
        # title only has a /vol/content mount when launched with its packaged
        # content, and a bare .rpx (e.g. dropped into Cemu) has none. So encode
        # the desktop WAV to a small Vorbis OGG and #embed it (src/wiiu/audio.cpp
        # decodes it with stb_vorbis at startup) -- it then plays however the
        # title is launched, and the artifact stays small.
        find_program(FFMPEG ffmpeg REQUIRED)
        FetchContent_Declare(
                stb
                GIT_REPOSITORY https://github.com/nothings/stb.git
                GIT_TAG        master
                GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(stb)
        target_include_directories(pnes PRIVATE ${stb_SOURCE_DIR})

        set(WIIU_ASSET_DIR ${CMAKE_BINARY_DIR}/wiiu-assets)
        set(WIIU_OGG ${WIIU_ASSET_DIR}/pc_audio.ogg)
        set(WIIU_WAV ${CMAKE_SOURCE_DIR}/demo/audio/tracks/pc_audio.wav)
        # #embed reads pc_audio.ogg at compile time of audio.cpp; create the dir
        # now so it is on the include path at configure, and (a) make the lib
        # depend on the encode target so it runs first, (b) mark audio.cpp as
        # depending on the OGG so editing the source WAV retriggers a recompile.
        file(MAKE_DIRECTORY ${WIIU_ASSET_DIR})
        add_custom_command(
                OUTPUT ${WIIU_OGG}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${WIIU_ASSET_DIR}
                COMMAND ${FFMPEG} -y -loglevel error -i ${WIIU_WAV}
                        -ar 48000 -ac 2 -c:a libvorbis -q:a 4 ${WIIU_OGG}
                DEPENDS ${WIIU_WAV}
                COMMENT "Encoding Wii U audio: pc_audio.wav -> pc_audio.ogg (Vorbis, embedded)"
                VERBATIM)
        add_custom_target(wiiu-assets DEPENDS ${WIIU_OGG})
        add_dependencies(pnes wiiu-assets)
        # audio.cpp resolves `#embed <pc_audio.ogg>` via --embed-dir (the canonical
        # GCC mechanism; -I is not reliably searched for #embed). Scope both the
        # search dir and the rebuild-on-change dependency to that one source.
        set_source_files_properties(${CMAKE_SOURCE_DIR}/src/wiiu/audio.cpp
                PROPERTIES
                    OBJECT_DEPENDS  ${WIIU_OGG}
                    COMPILE_OPTIONS "--embed-dir=${WIIU_ASSET_DIR}")

        file(GLOB_RECURSE DEMO_SOURCES CONFIGURE_DEPENDS demo/*.cpp)
        list(FILTER DEMO_SOURCES EXCLUDE REGEX "/demo/gen/(title|gameOptions)/")
        file(GLOB DEMO_TITLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/demo/gen/title/${DEMO_TITLE_DIR}/*.cpp")
        list(APPEND DEMO_SOURCES ${DEMO_TITLE_SOURCES})
        file(GLOB DEMO_GAMEOPTIONS_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/demo/gen/gameOptions/${DEMO_TITLE_DIR}/*.cpp")
        list(APPEND DEMO_SOURCES ${DEMO_GAMEOPTIONS_SOURCES})

        # chr_rom.cpp #embeds the packed blob (demo/chr/chr.bin) and emits it as the
        # cartridge's CHR ROM image. The blob is a blobman output, not a source file,
        # so declare it explicitly: repacking it must retrigger that compile.
        set_source_files_properties(${CMAKE_SOURCE_DIR}/demo/src/graphics/chr_rom.cpp
                PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/demo/chr/chr.bin")

        # `demo` is the packaged .wuhb, `demo_elf` the linked ELF (the .rpx is a
        # POST_BUILD step of it) -- see the note on the 3DS branch's
        # add_executable for why.
        add_executable(demo_elf ${DEMO_SOURCES})
        set_target_properties(demo_elf PROPERTIES OUTPUT_NAME demo SUFFIX ".elf")
        # The demo supplies its own main() via the RESET macro, so the backend
        # builds with SDL_MAIN_HANDLED and SDL2main is deliberately NOT linked.
        target_link_libraries(demo_elf PRIVATE pnes)

        # See the TARGET_NES branch's own comment on this same call for
        # GEN_TARGET_DIR. Quoted: DEMO_TITLE_DIR is "wii u" (a space) on this
        # branch, and an unquoted argument here would split on that space
        # into two separate (wrong) compile definitions.
        target_compile_definitions(demo_elf PRIVATE "GEN_TARGET_DIR=${DEMO_TITLE_DIR}")

        # CHR art is pulled in with #embed (C23); keep the Clang-only silencer for
        # parity with the other branches (no-op under devkitPPC's GCC).
        target_compile_options(demo_elf PRIVATE $<$<CXX_COMPILER_ID:Clang>:-Wno-c23-extensions>)

        # Enable LTO for release-type configs, mirroring the other branches.
        include(CheckIPOSupported)
        check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
        if(_ipo_ok)
            foreach(_cfg Release MinSizeRel RelWithDebInfo)
                string(TOUPPER ${_cfg} _CFG)
                set_property(TARGET demo_elf pnes
                        PROPERTY INTERPROCEDURAL_OPTIMIZATION_${_CFG} ON)
            endforeach()
        else()
            message(STATUS "IPO/LTO not supported by this toolchain: ${_ipo_err}")
        endif()

        # Convert the linked ELF into an .rpx (the Cafe OS executable format) and
        # package it as a .wuhb bundle (the Aroma/Homebrew-Launcher format). The
        # music is embedded in the .rpx (above), so the bundle needs no content
        # tree -- both artifacts are fully self-contained and play audio however
        # they are launched. Both helpers come from wut's CafeOS.cmake.
        wut_create_rpx(demo_elf)
        wut_create_wuhb(demo
                TARGET    demo_elf
                NAME      "pnes demo"
                SHORTNAME "pnes"
                AUTHOR    "brette"
        )

        # ------------------------------------------------------------------
        # Optional: installable WUP title (the NON-homebrew path)
        #
        # The .rpx/.wuhb above are the homebrew artifacts (Aroma / HBL). For
        # people who want to *install* their game as a real Wii U title --
        # runnable with the system closed, no homebrew environment imposed --
        # this assembles the unpacked code/content/meta tree a Wii U title
        # needs and (when a common key is supplied) runs NUSPacker to encrypt
        # it into an installable WUP (.app/.h3/.tmd/.tik/.cert).
        #
        # This is a separate `demo-wup` target, NOT part of the default build:
        # the homebrew path stays key-free and self-contained, and the WUP
        # path only kicks in when someone asks for it (and only *packs* when a
        # key is present -- otherwise it still emits the tree for manual use).
        # The Wii U common key is a copyrighted secret: it is NEVER committed,
        # only passed in via WIIU_COMMON_KEY at build time (a CI secret / local
        # cache var, as a 32-hex string or a path to a file containing one).
        # ------------------------------------------------------------------
        set(WIIU_TITLE_ID     "000500001337C0DE" CACHE STRING "Wii U title id (16 hex)")
        set(WIIU_GROUP_ID     "0000C0DE"         CACHE STRING "Wii U group id (8 hex)")
        set(WIIU_PRODUCT_CODE "WUP-N-PNES"       CACHE STRING "Wii U product code")
        set(WIIU_COMPANY_CODE "0001"             CACHE STRING "Wii U company code")
        set(WIIU_LONGNAME     "${GAME_NAME}"     CACHE STRING "Wii U menu long name")
        set(WIIU_SHORTNAME    "${GAME_NAME}"     CACHE STRING "Wii U menu short name")
        set(WIIU_PUBLISHER    "brette"           CACHE STRING "Wii U menu publisher")
        set(WIIU_RPX_NAME     "demo.rpx"         CACHE STRING "rpx filename inside the WUP")
        set(WIIU_NUSPACKER_JAR "$ENV{WIIU_NUSPACKER_JAR}" CACHE STRING "Path to NUSPacker.jar")
        set(WIIU_COMMON_KEY   "$ENV{WIIU_COMMON_KEY}"     CACHE STRING "Wii U common key (32 hex) or file path")

        set(WUP_DIR     ${CMAKE_BINARY_DIR}/wup)
        set(WUP_CODE    ${WUP_DIR}/code)
        set(WUP_CONTENT ${WUP_DIR}/content)
        set(WUP_META    ${WUP_DIR}/meta)
        set(WUP_OUT     ${CMAKE_BINARY_DIR}/wup-installable)

        # The title metadata: configure_file substitutes the @WIIU_*@ tokens in
        # the proven dimok HBL-channel templates (src/wiiu/title/*.in) at
        # configure time, so editing a cache var re-emits the XMLs on reconfigure.
        configure_file(${CMAKE_SOURCE_DIR}/src/wiiu/title/app.xml.in  ${WUP_CODE}/app.xml  @ONLY)
        configure_file(${CMAKE_SOURCE_DIR}/src/wiiu/title/cos.xml.in  ${WUP_CODE}/cos.xml  @ONLY)
        configure_file(${CMAKE_SOURCE_DIR}/src/wiiu/title/meta.xml.in ${WUP_META}/meta.xml @ONLY)

        # The menu artwork. The Wii U menu wants UNCOMPRESSED, top-left-origin
        # TGAs at fixed sizes; derive them all from the existing demo/demo.png
        # icon with ImageMagick so a title ships with art out of the box. (The
        # boot sound, bootSound.btsnd, is optional -- the title boots silent
        # without it -- so it is intentionally omitted.)
        find_program(IMAGEMAGICK_CONVERT NAMES magick convert)
        set(WUP_ICON  ${WUP_META}/iconTex.tga)
        set(WUP_BOOTTV  ${WUP_META}/bootTvTex.tga)
        set(WUP_BOOTDRC ${WUP_META}/bootDrcTex.tga)
        set(WUP_BOOTLOGO ${WUP_META}/bootLogoTex.tga)
        set(WIIU_ICON_SRC ${CMAKE_SOURCE_DIR}/demo/demo.png)
        if(IMAGEMAGICK_CONVERT)
            # iconTex 128x128, bootTvTex 1280x720, bootDrcTex 854x480,
            # bootLogoTex ~170x42 -- all uncompressed, top-left origin.
            set(_tga_opts -define tga:image-origin=TopLeft -compress none -type TrueColor)
            add_custom_command(OUTPUT ${WUP_ICON}
                    COMMAND ${IMAGEMAGICK_CONVERT} ${WIIU_ICON_SRC} -resize 128x128! ${_tga_opts} ${WUP_ICON}
                    DEPENDS ${WIIU_ICON_SRC} VERBATIM)
            add_custom_command(OUTPUT ${WUP_BOOTTV}
                    COMMAND ${IMAGEMAGICK_CONVERT} ${WIIU_ICON_SRC} -resize 1280x720 -background black -gravity center -extent 1280x720 ${_tga_opts} ${WUP_BOOTTV}
                    DEPENDS ${WIIU_ICON_SRC} VERBATIM)
            add_custom_command(OUTPUT ${WUP_BOOTDRC}
                    COMMAND ${IMAGEMAGICK_CONVERT} ${WIIU_ICON_SRC} -resize 854x480 -background black -gravity center -extent 854x480 ${_tga_opts} ${WUP_BOOTDRC}
                    DEPENDS ${WIIU_ICON_SRC} VERBATIM)
            add_custom_command(OUTPUT ${WUP_BOOTLOGO}
                    COMMAND ${IMAGEMAGICK_CONVERT} ${WIIU_ICON_SRC} -resize 170x42 -background black -gravity center -extent 170x42 ${_tga_opts} ${WUP_BOOTLOGO}
                    DEPENDS ${WIIU_ICON_SRC} VERBATIM)
            set(WUP_TGAS ${WUP_ICON} ${WUP_BOOTTV} ${WUP_BOOTDRC} ${WUP_BOOTLOGO})
        else()
            message(STATUS "wiiu: ImageMagick not found; demo-wup will skip menu-art (TGA) generation")
            set(WUP_TGAS "")
        endif()

        # The packer step runs through a small -P script (cmake/wiiu_pack.cmake)
        # so the "pack vs. skip when no key" decision happens at BUILD time, not
        # configure time -- you can set WIIU_COMMON_KEY after configuring and
        # just rebuild demo-wup. The script also reads the key from a file if a
        # path was given, so a CI secret can be mounted as a file.
        add_custom_target(demo-wup
                # The rpx is built by the demo target; drop it into code/ as the
                # name cos.xml's argstr references.
                COMMAND ${CMAKE_COMMAND} -E copy
                        ${CMAKE_CURRENT_BINARY_DIR}/${WIIU_RPX_NAME} ${WUP_CODE}/${WIIU_RPX_NAME}
                # NUSPacker requires a non-empty content/; the title has no data
                # files (audio is embedded in the rpx), so seed a placeholder.
                COMMAND ${CMAKE_COMMAND} -E make_directory ${WUP_CONTENT}
                COMMAND ${CMAKE_COMMAND} -E touch ${WUP_CONTENT}/.keep
                COMMAND ${CMAKE_COMMAND}
                        -DWUP_DIR=${WUP_DIR}
                        -DWUP_OUT=${WUP_OUT}
                        -DNUSPACKER_JAR=${WIIU_NUSPACKER_JAR}
                        -DCOMMON_KEY=${WIIU_COMMON_KEY}
                        -DTITLE_ID=${WIIU_TITLE_ID}
                        -P ${CMAKE_SOURCE_DIR}/cmake/wiiu_pack.cmake
                DEPENDS demo ${WUP_TGAS}
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                COMMENT "Assembling Wii U installable title (code/content/meta) and packing WUP"
                VERBATIM)

    # ----------------------------------------------------------------------
    # Sony PSP target (pspsdk/pspdev), via the pspdev.cmake toolchain.
    #
    # Like the Switch/Wii U backends, the PSP presents the shared software PPU
    # (src/emu): emu::GenerateFrame rasterises one widescreen ARGB frame and the
    # CPU nearest-neighbour scale-blits it straight into VRAM through its
    # uncached alias -- no GU (the PSP's GE/3D pipeline) at all, the same
    # "skip the heavier GPU API for a first pass" call the Switch backend made
    # against deko3d. See src/psp/video.cpp for the framebuffer/scaling details.
    #
    # Unlike every devkitPro target above, this is NOT a devkitPro toolchain:
    # pspdev is its own separate SDK (psp-gcc/psp-g++, PSPDEV env var), and
    # pspdev.cmake sets CMAKE_SYSTEM_NAME to plain "Generic" rather than
    # anything PSP-specific -- so, uniquely among the console branches here,
    # TARGET_PSP is NOT derived from CMAKE_SYSTEM_NAME and must be set
    # explicitly below, same as the desktop branches do for their platforms.
    # The toolchain file (via presets, $env{PSPDEV}/psp/share/pspdev.cmake)
    # pulls in CreatePBP.cmake, whose create_pbp_file() helper packs the linked
    # ELF into the single-file EBOOT.PBP homebrew format used below.
    # ----------------------------------------------------------------------
    elseif(TARGET_PLATFORM STREQUAL "psp")
        project(pnes CXX)

        FetchContent_MakeAvailable(intsh tuple)
        _make_consh_available()
        _fixup_fetched_cxx_std_targets()

        set(DEMO_TITLE_DIR psp)

        # The PSP backend presents the shared software PPU (src/emu): the
        # emulator and all the nametable/OAM/palette write functions are
        # platform-agnostic and compiled here alongside the raw-VRAM video/
        # present, the pspctrl input, and the pspaudio streaming audio.
        file(GLOB PSP_SOURCES CONFIGURE_DEPENDS src/psp/*.cpp src/emu/*.cpp src/emu/mappers/*.cpp)
        file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/all/*.cpp)
        add_library(pnes STATIC ${PSP_SOURCES} ${ALL_SOURCES})
        target_include_directories(pnes PUBLIC
                ${CMAKE_SOURCE_DIR}/include
                ${CMAKE_SOURCE_DIR}/include/umbrella
        )
        # TARGET_PSP is the group the engine headers gate on (emulated PPU,
        # widescreen ~16:9 viewport shared with TARGET_NX/TARGET_WIIU, no SDL
        # headers). See this branch's own header comment for why this can't be
        # derived from CMAKE_SYSTEM_NAME the way the devkitPro branches' defines
        # effectively are.
        target_compile_definitions(pnes PUBLIC TARGET_PSP)
        # Alternative nametable wiring (see the TARGET_NES branch's own comment,
        # near the top of this file): a fact about which board a build is
        # modeling, not something exclusive to the NES target itself.
        if(DEFINED ALTERNATIVE_NAMETABLE)
            target_compile_definitions(pnes PUBLIC "ALTERNATIVE_NAMETABLE=${ALTERNATIVE_NAMETABLE}")
        endif()
        target_link_libraries(pnes PUBLIC br0::intsh br0::tuple br0::consh)

        # The PSP Fat/Slim's 32-64MB of RAM can't hold the 47MB desktop
        # pc_audio.wav decoded to raw PCM, so this follows the GameCube/Wii U
        # shape: transcode to a small Vorbis OGG at build time and #embed it
        # (src/psp/audio.cpp decodes it with stb_vorbis, streaming from a
        # dedicated thread). Encoded straight to this backend's playback rate
        # (44.1kHz, the PSP DAC's fixed rate) so no runtime resampling is needed.
        find_program(FFMPEG ffmpeg REQUIRED)
        FetchContent_Declare(
                stb
                GIT_REPOSITORY https://github.com/nothings/stb.git
                GIT_TAG        master
                GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(stb)
        target_include_directories(pnes PRIVATE ${stb_SOURCE_DIR})

        set(PSP_ASSET_DIR ${CMAKE_BINARY_DIR}/psp-assets)
        set(PSP_OGG ${PSP_ASSET_DIR}/pc_audio.ogg)
        set(PSP_WAV ${CMAKE_SOURCE_DIR}/demo/audio/tracks/pc_audio.wav)
        file(MAKE_DIRECTORY ${PSP_ASSET_DIR})
        add_custom_command(
                OUTPUT ${PSP_OGG}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${PSP_ASSET_DIR}
                COMMAND ${FFMPEG} -y -loglevel error -i ${PSP_WAV}
                        -ar 44100 -ac 2 -c:a libvorbis -q:a 4 ${PSP_OGG}
                DEPENDS ${PSP_WAV}
                COMMENT "Encoding PSP audio: pc_audio.wav -> pc_audio.ogg (Vorbis, embedded)"
                VERBATIM)
        add_custom_target(psp-assets DEPENDS ${PSP_OGG})
        add_dependencies(pnes psp-assets)
        # audio.cpp resolves `#embed <pc_audio.ogg>` via --embed-dir (the
        # canonical GCC mechanism; -I is not reliably searched for #embed).
        # Scope both the search dir and the rebuild-on-change dependency to
        # that one source.
        set_source_files_properties(${CMAKE_SOURCE_DIR}/src/psp/audio.cpp
                PROPERTIES
                    OBJECT_DEPENDS  ${PSP_OGG}
                    COMPILE_OPTIONS "--embed-dir=${PSP_ASSET_DIR}")

        file(GLOB_RECURSE DEMO_SOURCES CONFIGURE_DEPENDS demo/*.cpp)
        list(FILTER DEMO_SOURCES EXCLUDE REGEX "/demo/gen/(title|gameOptions)/")
        file(GLOB DEMO_TITLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/demo/gen/title/${DEMO_TITLE_DIR}/*.cpp")
        list(APPEND DEMO_SOURCES ${DEMO_TITLE_SOURCES})
        file(GLOB DEMO_GAMEOPTIONS_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/demo/gen/gameOptions/${DEMO_TITLE_DIR}/*.cpp")
        list(APPEND DEMO_SOURCES ${DEMO_GAMEOPTIONS_SOURCES})

        # chr_rom.cpp #embeds the packed blob (demo/chr/chr.bin) and emits it as the
        # cartridge's CHR ROM image. The blob is a blobman output, not a source file,
        # so declare it explicitly: repacking it must retrigger that compile.
        set_source_files_properties(${CMAKE_SOURCE_DIR}/demo/src/graphics/chr_rom.cpp
                PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/demo/chr/chr.bin")

        add_executable(demo ${DEMO_SOURCES})
        # pspdev's own crt0/linking wants the raw ELF extension for
        # create_pbp_file()/psp-fixup-imports below, not a bare "demo".
        set_target_properties(demo PROPERTIES SUFFIX ".elf")
        target_link_libraries(demo PRIVATE pnes pspdisplay pspctrl pspaudio pspge psppower)

        # See the TARGET_NES branch's own comment on this same call for GEN_TARGET_DIR.
        target_compile_definitions(demo PRIVATE "GEN_TARGET_DIR=${DEMO_TITLE_DIR}")

        # CHR art is pulled in with #embed (C23); keep the Clang-only silencer
        # for parity with the other branches (no-op under psp-g++/GCC).
        target_compile_options(demo PRIVATE $<$<CXX_COMPILER_ID:Clang>:-Wno-c23-extensions>)

        # Enable LTO for release-type configs, mirroring the other branches.
        include(CheckIPOSupported)
        check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
        if(_ipo_ok)
            foreach(_cfg Release MinSizeRel RelWithDebInfo)
                string(TOUPPER ${_cfg} _CFG)
                set_property(TARGET demo pnes
                        PROPERTY INTERPROCEDURAL_OPTIMIZATION_${_CFG} ON)
            endforeach()
        else()
            message(STATUS "IPO/LTO not supported by this toolchain: ${_ipo_err}")
        endif()

        # Pack the linked ELF into a single-file EBOOT.PBP homebrew image
        # (create_pbp_file comes from pspdev.cmake's bundled CreatePBP.cmake).
        # No ICON_PATH/BACKGROUND_PATH/PREVIEW_PATH art is supplied yet -- the
        # PSP menu falls back to its default icon without them, same as an
        # unset SMDH banner on the 3DS branch would.
        create_pbp_file(
                TARGET      demo
                TITLE       "pnes demo"
                VERSION     "1.00"
        )

    # ----------------------------------------------------------------------
    # Sony PlayStation 2 target (ps2dev/ps2sdk + gsKit), via cmake/PS2.cmake
    # (ps2dev ships no CMake toolchain of its own -- see that file).
    #
    # Unlike every emulated-PPU backend above, the PS2 does NOT run the shared
    # software rasteriser (emu::GenerateFrame) and is not widescreen: the GS
    # (Graphics Synthesizer) uploads the NES's native 256x240 frame as one
    # texture and draws it as a single hardware-scaled, pillarboxed quad onto
    # the fixed 640x448 NTSC screen, so video.hpp's viewport_tx()/viewport_ty()
    # for TARGET_PS2 are 32x30 -- identical to the real NES -- rather than a
    # "render more world" widescreen expansion. That's also why this branch
    # reuses the PSP's demo/gen/title and demo/gen/gameOptions art instead of
    # needing its own (see the DEMO_TITLE_DIR comment below for why PSP's,
    # not the NES's own, despite the PS2's viewport matching the NES exactly).
    #
    # ps2sdk's own tamtypes.h already typedefs u8/u16/u32/u64/s8/s16/s32/s64 as
    # ordinary global typedefs, so the real intsh dependency (whose
    # `using namespace br0::intsh` would make those names ambiguous, breaking
    # even ps2sdk's own headers -- see src/ps2/intsh_shim/intsh.hpp) is never
    # fetched for this target. br0::intsh is aliased to a local shim instead,
    # which supplies only the signed i8/i16/i32/i64 names tamtypes.h lacks
    # (and #includes tamtypes.h itself, ahead of everything else -- see
    # src/ps2/intsh_shim/intsh's own comment), so every shared engine header's
    # unconditional `#include <intsh>; using namespace br0::intsh;` keeps
    # working completely unmodified.
    # ----------------------------------------------------------------------
    elseif(TARGET_PLATFORM STREQUAL "ps2")
        project(pnes CXX)

        add_library(ps2_intsh_shim INTERFACE)
        target_include_directories(ps2_intsh_shim INTERFACE ${CMAKE_SOURCE_DIR}/src/ps2/intsh_shim)
        add_library(br0::intsh ALIAS ps2_intsh_shim)

        FetchContent_MakeAvailable(tuple)
        _make_consh_available()
        _fixup_fetched_cxx_std_targets()

        # Reuse the PSP's generated title/options art, NOT the NES's: the
        # NES's demo/gen/title|gameOptions|options/nes headers additionally
        # #error without SYSMEM/TITLE/TITLE_DATA/OPTIONS/OPTIONS_DATA, which
        # only mean anything for the real cartridge's bank-switched ROM
        # layout (set in a developer's local.cmake) -- nonsensical for a PS2
        # ELF with no such memory map. The PSP's own generated headers have
        # no such requirement (same shared video.hpp/text.hpp calls, just
        # without the NES-only bank placement), and PSP's panel is a
        # superset of the PS2's native 32x30 (480x272 vs this branch's
        # 256x240), so the same absolute tile coordinates fit here too.
        set(DEMO_TITLE_DIR psp)

        if(NOT DEFINED ENV{PS2SDK} OR NOT DEFINED ENV{GSKIT})
            message(FATAL_ERROR
                "PS2 build requires the PS2SDK and GSKIT env vars (set by the "
                "ps2dev/ps2dev Docker image -- see docker/Dockerfile.ps2).")
        endif()
        set(PS2SDK_ROOT "$ENV{PS2SDK}")
        set(GSKIT_ROOT  "$ENV{GSKIT}")

        # The PS2 backend presents the shared software PPU (src/emu): the
        # emulator and all the nametable/OAM/palette write functions are
        # platform-agnostic and compiled here alongside the gsKit video/
        # present, the libpad input, and the audsrv streaming audio.
        file(GLOB PS2_SOURCES CONFIGURE_DEPENDS src/ps2/*.cpp src/emu/*.cpp src/emu/mappers/*.cpp)

        # Music backend. Each of the three defines the whole audio:: surface, so
        # exactly one is compiled and the rest are dropped from the glob.
        #
        #   stream (default) src/ps2/audio_stream.cpp -- SPU2-format ADPCM
        #                    decoded on the EE. Uses the shared pc_audio.wav at
        #                    any length with no asset constraint and no size
        #                    knob to get right, for a fraction of a percent of
        #                    the EE. The portable default.
        #   spu2             src/ps2/audio_spu2.cpp -- resident SPU2 hardware
        #                    voice. Costs the EE literally nothing, but the
        #                    sample must fit SPU2 sound RAM (~756KB measured),
        #                    so it needs an asset trimmed to a short loop.
        #   vorbis           src/ps2/audio.cpp -- stb_vorbis on the EE. Best
        #                    compression, but the decode is what held this
        #                    backend at ~55fps. Kept as a reference path.
        set(PS2_MUSIC "stream" CACHE STRING
                "PS2 music backend: stream (EE-decoded ADPCM), spu2 (SPU2 hardware voice), vorbis")
        set_property(CACHE PS2_MUSIC PROPERTY STRINGS stream spu2 vorbis)

        set(PS2_AUDIO_ALL
                ${CMAKE_SOURCE_DIR}/src/ps2/audio_stream.cpp
                ${CMAKE_SOURCE_DIR}/src/ps2/audio_spu2.cpp
                ${CMAKE_SOURCE_DIR}/src/ps2/audio.cpp)
        if(PS2_MUSIC STREQUAL "stream")
            set(PS2_AUDIO_SOURCE ${CMAKE_SOURCE_DIR}/src/ps2/audio_stream.cpp)
        elseif(PS2_MUSIC STREQUAL "spu2")
            set(PS2_AUDIO_SOURCE ${CMAKE_SOURCE_DIR}/src/ps2/audio_spu2.cpp)
        elseif(PS2_MUSIC STREQUAL "vorbis")
            set(PS2_AUDIO_SOURCE ${CMAKE_SOURCE_DIR}/src/ps2/audio.cpp)
        else()
            message(FATAL_ERROR "PS2_MUSIC must be stream, spu2 or vorbis, got '${PS2_MUSIC}'")
        endif()
        foreach(_unused IN LISTS PS2_AUDIO_ALL)
            if(NOT _unused STREQUAL PS2_AUDIO_SOURCE)
                list(REMOVE_ITEM PS2_SOURCES ${_unused})
            endif()
        endforeach()
        file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/all/*.cpp)
        add_library(pnes STATIC ${PS2_SOURCES} ${ALL_SOURCES})
        target_include_directories(pnes PUBLIC
                ${CMAKE_SOURCE_DIR}/include
                ${CMAKE_SOURCE_DIR}/include/umbrella
                ${PS2SDK_ROOT}/ee/include
                ${PS2SDK_ROOT}/common/include
                ${GSKIT_ROOT}/include
        )
        # TARGET_PS2 is the group the engine headers gate on (NES-native
        # 32x30 viewport, no SDL headers -- see this branch's header comment).
        # _EE is what tamtypes.h/every ps2sdk header gates its EE-side
        # declarations on; src/ps2/internal.hpp includes tamtypes.h before any
        # engine header, so it must be visible from the very first TU, not
        # just within src/ps2/.
        target_compile_definitions(pnes PUBLIC TARGET_PS2 _EE)
        # Alternative nametable wiring (see the TARGET_NES branch's own comment,
        # near the top of this file): a fact about which board a build is
        # modeling, not something exclusive to the NES target itself.
        if(DEFINED ALTERNATIVE_NAMETABLE)
            target_compile_definitions(pnes PUBLIC "ALTERNATIVE_NAMETABLE=${ALTERNATIVE_NAMETABLE}")
        endif()
        target_link_libraries(pnes PUBLIC br0::intsh br0::tuple br0::consh)

        find_program(FFMPEG ffmpeg REQUIRED)
        set(PS2_ASSET_DIR ${CMAKE_BINARY_DIR}/ps2-assets)
        set(PS2_WAV ${CMAKE_SOURCE_DIR}/demo/audio/tracks/pc_audio.wav)
        file(MAKE_DIRECTORY ${PS2_ASSET_DIR})

        if(PS2_MUSIC STREQUAL "stream")
            # Downmix to mono at PS2_STREAM_RATE and encode to SPU2 ADPCM with
            # ps2sdk's bin/adpenc. No -L: the loop flags it would set are
            # instructions to the SPU2's voice hardware, and this backend
            # doesn't play through a voice -- audio_stream.cpp loops in
            # software, which also lets it honour a per-track loop_start.
            #
            # No size ceiling applies here (nothing has to fit SPU2 RAM), so the
            # rate costs only ELF size: rate * seconds * 16/28 bytes, i.e.
            # ~6.8MB for the current 4m29s track at 44.1kHz.
            #
            # 44100 rather than something smaller because the rate is NOT a free
            # choice -- it is handed to audsrv_set_format(), which rejects
            # formats it does not support, and 44100 is the rate the vorbis
            # backend already proved works on this console. 22050 is the other
            # safe value (ps2sdk's own playwav/playwav2 samples use it) and
            # halves the size. An unproven 32000 produced silence.
            set(PS2_STREAM_RATE "44100" CACHE STRING
                    "Sample rate for the EE-decoded ADPCM music stream (44100 and 22050 are proven on hardware)")
            find_program(PS2_ADPENC adpenc HINTS ${PS2SDK_ROOT}/bin REQUIRED)

            set(PS2_ADP     ${PS2_ASSET_DIR}/pc_audio.adp)
            set(PS2_ADP_WAV ${PS2_ASSET_DIR}/pc_audio_mono.wav)
            add_custom_command(
                    OUTPUT ${PS2_ADP}
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${PS2_ASSET_DIR}
                    COMMAND ${FFMPEG} -y -loglevel error -i ${PS2_WAV}
                            -ar ${PS2_STREAM_RATE} -ac 1 ${PS2_ADP_WAV}
                    COMMAND ${PS2_ADPENC} ${PS2_ADP_WAV} ${PS2_ADP}
                    DEPENDS ${PS2_WAV}
                    COMMENT "Encoding PS2 audio: pc_audio.wav -> pc_audio.adp (ADPCM stream @ ${PS2_STREAM_RATE}Hz mono, embedded)"
                    VERBATIM)
            add_custom_target(ps2-assets DEPENDS ${PS2_ADP})
            set(PS2_AUDIO_ASSET ${PS2_ADP})
            # audio_stream.cpp needs the rate the asset was actually encoded at
            # to configure audsrv's output format; a mismatch plays at the
            # wrong speed, so it is passed rather than guessed.
            set_source_files_properties(${PS2_AUDIO_SOURCE}
                    PROPERTIES COMPILE_DEFINITIONS "PS2_STREAM_RATE=${PS2_STREAM_RATE}")
        elseif(PS2_MUSIC STREQUAL "spu2")
            # Downmix to mono at PS2_ADPCM_RATE, then encode to SPU2 ADPCM with
            # ps2sdk's own bin/adpenc. -L sets the loop flag in the output's
            # header and in the first ADPCM block, which is what makes the
            # hardware voice loop with no software involvement at all.
            #
            # The rate is the whole budget question: SPU2 ADPCM is 16 bytes per
            # 28 samples and the sample has to be resident in the SPU2's 2MB of
            # sound RAM, so rate * seconds * 0.571 must stay under it. For the
            # current 4m29s pc_audio.wav that means ~13.6kHz is the ceiling and
            # 11025 (1.69MB, ~81% of SPU2 RAM) is the safe default. Trimming the
            # asset to its musical loop is what buys fidelity back -- see
            # src/ps2/audio_spu2.cpp's file header.
            set(PS2_ADPCM_RATE "11025" CACHE STRING
                    "Source sample rate for the SPU2 ADPCM music sample (mono; must fit SPU2's 2MB)")

            # Diagnostic knob: 0 encodes the whole track, anything else
            # truncates to that many seconds. ps2sdk's own playadpcm sample only
            # ever loads a ~30KB sample, so a full-length one asks
            # audsrv_load_adpcm() to do something ~56x larger than the path is
            # demonstrated with -- if a short sample plays and a long one does
            # not, the ceiling is SPU2 RAM or audsrv's RPC upload, not anything
            # in src/ps2/audio_spu2.cpp.
            # Defaults to 120s, not the whole track, because the whole track does
            # not fit -- see PS2_ADPCM_MAX_BYTES below. 120s at 11025Hz is the
            # largest configuration measured working on hardware.
            set(PS2_ADPCM_SECONDS "120" CACHE STRING
                    "Truncate the SPU2 ADPCM sample to N seconds (0 = whole track)")

            # Measured on hardware, not derived from the SPU2's nominal 2MB:
            # 756032 bytes plays correctly, 1134032 hangs the console during
            # audio::Init. audsrv also reserves part of SPU2 sound RAM for its
            # own PCM ring, which is why the usable ceiling is well under 2MB.
            # Enforced by cmake/ps2_check_adpcm_size.cmake -- see that file for
            # why this is a build error rather than something checked at runtime.
            set(PS2_ADPCM_MAX_BYTES "786432" CACHE STRING
                    "Hard limit for the encoded SPU2 ADPCM sample, in bytes")
            if(PS2_ADPCM_SECONDS STREQUAL "0")
                set(PS2_ADPCM_TRUNCATE "")
            else()
                set(PS2_ADPCM_TRUNCATE -t ${PS2_ADPCM_SECONDS})
            endif()

            find_program(PS2_ADPENC adpenc HINTS ${PS2SDK_ROOT}/bin REQUIRED)

            set(PS2_ADP     ${PS2_ASSET_DIR}/pc_audio.adp)
            set(PS2_ADP_WAV ${PS2_ASSET_DIR}/pc_audio_mono.wav)
            add_custom_command(
                    OUTPUT ${PS2_ADP}
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${PS2_ASSET_DIR}
                    COMMAND ${FFMPEG} -y -loglevel error -i ${PS2_WAV}
                            -ar ${PS2_ADPCM_RATE} -ac 1 ${PS2_ADPCM_TRUNCATE} ${PS2_ADP_WAV}
                    COMMAND ${PS2_ADPENC} -L ${PS2_ADP_WAV} ${PS2_ADP}
                    COMMAND ${CMAKE_COMMAND} -DADP=${PS2_ADP} -DMAX=${PS2_ADPCM_MAX_BYTES}
                            -P ${CMAKE_SOURCE_DIR}/cmake/ps2_check_adpcm_size.cmake
                    DEPENDS ${PS2_WAV} ${CMAKE_SOURCE_DIR}/cmake/ps2_check_adpcm_size.cmake
                    COMMENT "Encoding PS2 audio: pc_audio.wav -> pc_audio.adp (SPU2 ADPCM @ ${PS2_ADPCM_RATE}Hz mono, ${PS2_ADPCM_SECONDS}s=0:all, embedded)"
                    VERBATIM)
            add_custom_target(ps2-assets DEPENDS ${PS2_ADP})
            set(PS2_AUDIO_ASSET ${PS2_ADP})
        else()
            # Same reasoning as the PSP branch: transcode the desktop
            # pc_audio.wav to a small Vorbis OGG at build time and #embed it
            # (src/ps2/audio.cpp decodes it with stb_vorbis, streaming from a
            # dedicated EE thread).
            FetchContent_Declare(
                    stb
                    GIT_REPOSITORY https://github.com/nothings/stb.git
                    GIT_TAG        master
                    GIT_SHALLOW    TRUE
            )
            FetchContent_MakeAvailable(stb)
            target_include_directories(pnes PRIVATE ${stb_SOURCE_DIR})

            set(PS2_OGG ${PS2_ASSET_DIR}/pc_audio.ogg)
            add_custom_command(
                    OUTPUT ${PS2_OGG}
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${PS2_ASSET_DIR}
                    COMMAND ${FFMPEG} -y -loglevel error -i ${PS2_WAV}
                            -ar 44100 -ac 2 -c:a libvorbis -q:a 4 ${PS2_OGG}
                    DEPENDS ${PS2_WAV}
                    COMMENT "Encoding PS2 audio: pc_audio.wav -> pc_audio.ogg (Vorbis, embedded)"
                    VERBATIM)
            add_custom_target(ps2-assets DEPENDS ${PS2_OGG})
            set(PS2_AUDIO_ASSET ${PS2_OGG})
        endif()
        add_dependencies(pnes ps2-assets)

        # Either audio backend also #embeds ps2sdk's prebuilt audsrv.irx (see
        # their file headers for why audsrv -- unlike the BIOS-resident
        # SIO2MAN/PADMAN/LIBSD -- needs to be loaded this way); --embed-dir
        # takes a single directory, so both embeds need their own dir on the
        # search list.
        set_source_files_properties(${PS2_AUDIO_SOURCE}
                PROPERTIES
                    OBJECT_DEPENDS  ${PS2_AUDIO_ASSET}
                    COMPILE_OPTIONS "--embed-dir=${PS2_ASSET_DIR};--embed-dir=${PS2SDK_ROOT}/iop/irx")

        file(GLOB_RECURSE DEMO_SOURCES CONFIGURE_DEPENDS demo/*.cpp)
        list(FILTER DEMO_SOURCES EXCLUDE REGEX "/demo/gen/(title|gameOptions)/")
        file(GLOB DEMO_TITLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/demo/gen/title/${DEMO_TITLE_DIR}/*.cpp")
        list(APPEND DEMO_SOURCES ${DEMO_TITLE_SOURCES})
        file(GLOB DEMO_GAMEOPTIONS_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/demo/gen/gameOptions/${DEMO_TITLE_DIR}/*.cpp")
        list(APPEND DEMO_SOURCES ${DEMO_GAMEOPTIONS_SOURCES})

        # chr_rom.cpp #embeds the packed blob (demo/chr/chr.bin) and emits it as the
        # cartridge's CHR ROM image. The blob is a blobman output, not a source file,
        # so declare it explicitly: repacking it must retrigger that compile.
        set_source_files_properties(${CMAKE_SOURCE_DIR}/demo/src/graphics/chr_rom.cpp
                PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/demo/chr/chr.bin")

        add_executable(demo ${DEMO_SOURCES})
        # ps2sdk's own linking wants the raw ELF extension, same as the PSP
        # branch's EBOOT packing step wants a bare .elf to pack from.
        set_target_properties(demo PROPERTIES SUFFIX ".elf")
        target_link_directories(demo PRIVATE ${PS2SDK_ROOT}/ee/lib ${GSKIT_ROOT}/lib)
        # gskit/gskit_toolkit/dmakit (gsKit + its DMA helper) and pad/audsrv
        # are the only ps2sdk/gsKit libs this backend needs beyond what the
        # compiler's own specs already link (libkernel/libcglue/libpthread*/
        # libcdvd -- see cmake/PS2.cmake's header comment). patches (libpatches,
        # sbv_patches.h) is needed on top of those: see audio.cpp's own comment
        # on sbv_patch_enable_lmb() for why loading audsrv.irx from an EE RAM
        # buffer (SifExecModuleBuffer, not a real rom0:/host: path) requires it.
        target_link_libraries(demo PRIVATE pnes
                gskit gskit_toolkit dmakit
                pad audsrv patches
        )

        # See the TARGET_NES branch's own comment on this same call for GEN_TARGET_DIR.
        target_compile_definitions(demo PRIVATE "GEN_TARGET_DIR=${DEMO_TITLE_DIR}")

        # CHR art is pulled in with #embed (C23); keep the Clang-only silencer
        # for parity with the other branches (no-op under ee-g++/GCC).
        target_compile_options(demo PRIVATE $<$<CXX_COMPILER_ID:Clang>:-Wno-c23-extensions>)

        # Enable LTO for release-type configs, mirroring the other branches.
        include(CheckIPOSupported)
        check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
        if(_ipo_ok)
            foreach(_cfg Release MinSizeRel RelWithDebInfo)
                string(TOUPPER ${_cfg} _CFG)
                set_property(TARGET demo pnes
                        PROPERTY INTERPROCEDURAL_OPTIMIZATION_${_CFG} ON)
            endforeach()
        else()
            message(STATUS "IPO/LTO not supported by this toolchain: ${_ipo_err}")
        endif()

    # ----------------------------------------------------------------------
    # Nintendo DS / DSi target (devkitARM + libnds), via the NDS.cmake toolchain.
    #
    # Unlike every other emulated-PPU backend (SDL/OGC/CTR/NX/WIIU), the DS does
    # NOT run the software rasteriser (emu::GenerateFrame). The DS is a 2D tile
    # console whose hardware is a near-cousin of the NES PPU, so this backend maps
    # the NES PPU onto the DS's NATIVE 2D hardware (Model B):
    #   * CHR pattern tables  -> DS character (tile) VRAM (2bpp -> 4bpp expand)
    #   * nametable+attribute -> DS BG tilemap entries (tile + palette + flip)
    #   * NES master palette  -> DS BGR555 palette RAM
    #   * OAM shadow          -> DS hardware OBJ (oamSet)
    #   * raster band timeline (emu::GenerateBands) -> a per-scanline HBlank scroll
    #     table, so mid-frame scroll changes and the sprite-0 split still work.
    # It therefore still pulls in src/emu for the platform-agnostic PPU bookkeeping
    # (nametable/OAM/palette writes, the band/IRQ timeline, the OAM shadow), but
    # the per-pixel rasteriser there goes unused.
    #
    # The 256x192 panel is 6 tiles shorter than the NES's 240px; the backend shows
    # a window onto the unaltered 256x240 world (the engine never compromises the
    # NES game for the DS). Audio: the 4 MB DS has no room for baked PCM, so audio
    # is stubbed (src/nds/audio.cpp) exactly like the GameCube/Wii backends -- the
    # user supplies a synthesiser. The DSi build (TARGET_DSI) has 16 MB and may
    # later embed crunched audio; for now it shares the DS stub path. Only the tiny
    # CHR art (a few KiB) is embedded.
    #
    # The toolchain file (NDS.cmake) is supplied via the nds-*/dsi-* presets; it
    # sets CMAKE_SYSTEM_NAME=NintendoDS and, through Platform/NintendoDS.cmake, the
    # ARM9 arch flags and the nds_create_rom() helper used below. libnds is linked
    # explicitly as -lnds9 (calico's ARM7 + linker script come in via the toolchain).
    # ----------------------------------------------------------------------
    # Sony PlayStation 3 target (ps3dev/PSL1GHT), via cmake/PS3.cmake (ps3dev
    # ships no CMake toolchain of its own -- see that file).
    #
    # The compiler for this branch is Clang, not the ppu-g++ that ps3dev
    # builds: ps3toolchain's PPU GCC is 13.2.0, which has no #embed, and
    # src/ps3/audio.cpp needs it. cmake/PS3.cmake sets that up and
    # keeps ppu-g++ as the link driver; docker/Dockerfile.ps3 is what puts both
    # in one image.
    #
    # PSL1GHT's ppu-types.h already typedefs u8/u16/u32/u64/s8/s16/s32/s64, so
    # the real intsh (whose `using namespace br0::intsh` would make those names
    # ambiguous unqualified -- breaking PSL1GHT's own headers, not just this
    # project's) is never fetched here either; src/ps3/intsh_shim/intsh stands
    # in, exactly as the ps2 branch does for ps2sdk's tamtypes.h. See that
    # shim's own comment.
    elseif(TARGET_PLATFORM STREQUAL "ps3")
        project(pnes CXX)

        add_library(ps3_intsh_shim INTERFACE)
        target_include_directories(ps3_intsh_shim INTERFACE ${CMAKE_SOURCE_DIR}/src/ps3/intsh_shim)
        add_library(br0::intsh ALIAS ps3_intsh_shim)

        FetchContent_MakeAvailable(tuple)
        _make_consh_available()
        _fixup_fetched_cxx_std_targets()

        # Reuse the PSP's generated title/options art, NOT the NES's -- for the
        # identical reason the ps2 branch does (see its own comment): the NES's
        # demo/gen/*/nes headers #error without SYSMEM/TITLE/TITLE_DATA/
        # OPTIONS/OPTIONS_DATA, which only describe the real cartridge's
        # bank-switched ROM layout and mean nothing for a PS3 SELF. The PSP's
        # generated headers make the same shared video.hpp/text.hpp calls with
        # no bank placement, and its panel is a superset of this branch's
        # NES-native 256x240, so the same absolute tile coordinates fit.
        set(DEMO_TITLE_DIR psp)

        if(NOT DEFINED ENV{PSL1GHT})
            message(FATAL_ERROR
                "PS3 build requires the PSL1GHT env var (set alongside PS3DEV by "
                "the pnes-ps3 Docker image -- see docker/Dockerfile.ps3).")
        endif()
        set(PS3DEV_ROOT  "$ENV{PS3DEV}")
        set(PSL1GHT_ROOT "$ENV{PSL1GHT}")

        # The PS3 backend presents the shared software PPU (src/emu): the
        # emulator and all the nametable/OAM/palette write functions are
        # platform-agnostic and compiled here alongside the RSX-mapped
        # framebuffer present, the libio pad input, and the libaudio streaming
        # audio.
        file(GLOB PS3_SOURCES CONFIGURE_DEPENDS src/ps3/*.cpp src/emu/*.cpp src/emu/mappers/*.cpp)
        file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/all/*.cpp)
        add_library(pnes STATIC ${PS3_SOURCES} ${ALL_SOURCES})
        target_include_directories(pnes PUBLIC
                ${CMAKE_SOURCE_DIR}/include
                ${CMAKE_SOURCE_DIR}/include/umbrella
                ${PSL1GHT_ROOT}/ppu/include
        )
        # TARGET_PS3 is the group the engine headers gate on (NES-native 32x30
        # viewport, no SDL headers -- see video.hpp's own TARGET_PS3 branch).
        # Unlike the PS2's _EE, PSL1GHT needs no equivalent "this is the PPU"
        # define: its headers are unconditional.
        target_compile_definitions(pnes PUBLIC TARGET_PS3)
        # Alternative nametable wiring (see the TARGET_NES branch's own comment,
        # near the top of this file): a fact about which board a build is
        # modeling, not something exclusive to the NES target itself.
        if(DEFINED ALTERNATIVE_NAMETABLE)
            target_compile_definitions(pnes PUBLIC "ALTERNATIVE_NAMETABLE=${ALTERNATIVE_NAMETABLE}")
        endif()
        # Boot tracing: ps3_trace() (src/ps3/internal.hpp) writes one line per
        # init step straight to lv2's TTY channel 0, in RELEASE as well as
        # debug -- deliberately not NDEBUG-gated, unlike log(), because the
        # thing it exists to diagnose is a release build that never reaches a
        # first frame. Every bail-out in irq::init() and audio::Init() names
        # itself, so a console that boots to black reports which step failed
        # instead of leaving it to be guessed at.
        #
        # ON while this backend is being brought up on real hardware/RPCS3;
        # configure with -DPS3_BOOT_TRACE=0 to compile it out to nothing (the
        # helper becomes an empty inline taking an unused argument). Flip the
        # default once a first boot is confirmed good.
        set(PS3_BOOT_TRACE 1 CACHE STRING "Trace PS3 init steps to the lv2 TTY (bring-up aid)")
        target_compile_definitions(pnes PUBLIC "PS3_BOOT_TRACE=${PS3_BOOT_TRACE}")

        target_link_libraries(pnes PUBLIC br0::intsh br0::tuple br0::consh)

        # src/ps3/audio.cpp #embeds the music asset, so the backend library
        # needs this silencer too -- not just the demo target the way the other
        # console branches do it. On those, Clang is never the compiler and the
        # flag is dead weight kept for parity; here Clang IS the compiler, so
        # #embed really does warn as a C23 extension in C++ mode.
        target_compile_options(pnes PRIVATE $<$<CXX_COMPILER_ID:Clang>:-Wno-c23-extensions>)

        # Same reasoning as the PSP/PS2 branches: transcode the desktop
        # pc_audio.wav to a small Vorbis OGG at build time and #embed it
        # (src/ps3/audio.cpp decodes it with stb_vorbis, streaming from a
        # dedicated PPU thread).
        #
        # 48000, not the 44100 the PSP and PS2 branches encode to: libaudio's
        # port runs at a fixed 48kHz and takes 32-bit float samples, so encoding
        # the asset at that rate means the backend neither resamples nor
        # converts formats. See src/ps3/audio.cpp's file header.
        find_program(FFMPEG ffmpeg REQUIRED)
        FetchContent_Declare(
                stb
                GIT_REPOSITORY https://github.com/nothings/stb.git
                GIT_TAG        master
                GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(stb)
        target_include_directories(pnes PRIVATE ${stb_SOURCE_DIR})

        set(PS3_ASSET_DIR ${CMAKE_BINARY_DIR}/ps3-assets)
        set(PS3_OGG ${PS3_ASSET_DIR}/pc_audio.ogg)
        set(PS3_WAV ${CMAKE_SOURCE_DIR}/demo/audio/tracks/pc_audio.wav)
        file(MAKE_DIRECTORY ${PS3_ASSET_DIR})
        add_custom_command(
                OUTPUT ${PS3_OGG}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${PS3_ASSET_DIR}
                COMMAND ${FFMPEG} -y -loglevel error -i ${PS3_WAV}
                        -ar 48000 -ac 2 -c:a libvorbis -q:a 4 ${PS3_OGG}
                DEPENDS ${PS3_WAV}
                COMMENT "Encoding PS3 audio: pc_audio.wav -> pc_audio.ogg (Vorbis 48kHz, embedded)"
                VERBATIM)
        add_custom_target(ps3-assets DEPENDS ${PS3_OGG})
        add_dependencies(pnes ps3-assets)

        # Only one --embed-dir is needed here, unlike the ps2 branch's two:
        # there is no IOP-side module to embed alongside the music, because
        # libaudio is PPU-side (see src/ps3/audio.cpp's file header).
        set_source_files_properties(${CMAKE_SOURCE_DIR}/src/ps3/audio.cpp
                PROPERTIES
                    OBJECT_DEPENDS  ${PS3_OGG}
                    COMPILE_OPTIONS "--embed-dir=${PS3_ASSET_DIR}")

        file(GLOB_RECURSE DEMO_SOURCES CONFIGURE_DEPENDS demo/*.cpp)
        list(FILTER DEMO_SOURCES EXCLUDE REGEX "/demo/gen/(title|gameOptions)/")
        file(GLOB DEMO_TITLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/demo/gen/title/${DEMO_TITLE_DIR}/*.cpp")
        list(APPEND DEMO_SOURCES ${DEMO_TITLE_SOURCES})
        file(GLOB DEMO_GAMEOPTIONS_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/demo/gen/gameOptions/${DEMO_TITLE_DIR}/*.cpp")
        list(APPEND DEMO_SOURCES ${DEMO_GAMEOPTIONS_SOURCES})

        # chr_rom.cpp #embeds the packed blob (demo/chr/chr.bin) and emits it as the
        # cartridge's CHR ROM image. The blob is a blobman output, not a source file,
        # so declare it explicitly: repacking it must retrigger that compile.
        set_source_files_properties(${CMAKE_SOURCE_DIR}/demo/src/graphics/chr_rom.cpp
                PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/demo/chr/chr.bin")

        add_executable(demo ${DEMO_SOURCES})
        # The SELF packing step below wants a bare .elf to pack from, same as
        # the PSP branch's EBOOT.PBP step and the ps2 branch's raw ELF.
        set_target_properties(demo PROPERTIES SUFFIX ".elf")
        target_link_directories(demo PRIVATE ${PSL1GHT_ROOT}/ppu/lib)
        # PSL1GHT's conventional link set and order. rsx/gcm_sys are the RSX
        # context + display-buffer/flip side used by src/ps3/video.cpp; io is
        # libio (pad); sysutil/sysmodule are the XMB exit callback; audio is
        # libaudio; lv2 is the syscall layer everything above sits on and has
        # to come last. m is newlib's libm, which stb_vorbis needs (floor/pow).
        target_link_libraries(demo PRIVATE pnes
                rsx gcm_sys
                io
                sysutil sysmodule
                audio
                lv2 m
        )

        # See the TARGET_NES branch's own comment on this same call for GEN_TARGET_DIR.
        target_compile_definitions(demo PRIVATE "GEN_TARGET_DIR=${DEMO_TITLE_DIR}")

        # CHR art is pulled in with #embed (C23). Unlike the other console
        # branches, where this silencer is a no-op kept for parity, Clang IS the
        # compiler here -- so this is the branch where it actually fires.
        target_compile_options(demo PRIVATE $<$<CXX_COMPILER_ID:Clang>:-Wno-c23-extensions>)

        # Pack the linked ELF into the SELF homebrew image the console actually
        # boots. Three steps, all ps3dev host tools:
        #
        #   sprxlinker  rewrites the ELF's PRX import/export stubs IN PLACE.
        #               PSL1GHT links against stub libraries whose relocations
        #               are not finished by the linker, so this is not optional
        #               -- it is what PSL1GHT's own ppu_rules runs immediately
        #               after its link step. In place, and before the two packs
        #               below, is also what leaves demo.elf itself bootable:
        #               RPCS3 loads a bare PPU ELF, which is the one way to
        #               exercise this backend with the SELF layer taken out of
        #               the picture entirely.
        #   fself       wraps the result as EBOOT.BIN, the filename the XMB
        #               expects inside a homebrew folder's USRDIR.
        #   make_self   wraps it a second time as EBOOT.CEX.BIN.
        #
        # fself is the one EBOOT.BIN is built from, and the difference is not
        # cosmetic -- it is in the SCE header's key_revision field:
        #
        #   fself      5343 4500 0000 0002 8000 0001 ...   segments stored raw
        #   make_self  5343 4500 0000 0002 0001 0001 ...   segments compressed
        #
        # 0x8000 is the agreed "this image is unsigned, read the segments
        # straight out" marker, and it is what loaders check first. make_self
        # instead emits a retail-SHAPED image -- real key revision, compressed
        # segments -- which obliges the loader down its decrypt-with-console-
        # keys path for a file that was never signed by anything. Neither image
        # is any more genuinely signed than the other (ps3dev has no private
        # key), so the retail shape buys nothing and costs the raw-load route.
        #
        # PSL1GHT's own ppu_rules emits both for exactly this reason: its %.self
        # rule runs make_self AND then fself into $(basename $@).fake.self. This
        # just picks the fake one as the default rather than the extra.
        # EBOOT.CEX.BIN is kept so nothing that wants the other form has to go
        # looking for a tool.
        #
        # Neither pack strips first, unlike ppu_rules (which does $(STRIP) ->
        # sprxlinker -> make_self). Keeping the symbols is what lets addr2line
        # turn an RPCS3 backtrace back into a line of this repo, and make_self
        # only stores PT_LOAD segments anyway, so the DWARF costs size in the
        # fself image alone.
        #
        # POST_BUILD on the demo target rather than a separate add_custom_target
        # so that `cmake --build` produces the bootable artifact with no extra
        # target to remember -- matching how the Wii U branch packs its RPX.
        find_program(PS3_SPRXLINKER sprxlinker HINTS ${PS3DEV_ROOT}/bin REQUIRED)
        find_program(PS3_FSELF      fself      HINTS ${PS3DEV_ROOT}/bin REQUIRED)
        find_program(PS3_MAKE_SELF  make_self  HINTS ${PS3DEV_ROOT}/bin REQUIRED)
        add_custom_command(TARGET demo POST_BUILD
                COMMAND ${PS3_SPRXLINKER} $<TARGET_FILE:demo>
                COMMAND ${PS3_FSELF} $<TARGET_FILE:demo> ${CMAKE_BINARY_DIR}/EBOOT.BIN
                COMMAND ${PS3_MAKE_SELF} $<TARGET_FILE:demo> ${CMAKE_BINARY_DIR}/EBOOT.CEX.BIN
                COMMENT "Packing PS3 SELF: demo.elf -> EBOOT.BIN (sprxlinker + fself) + EBOOT.CEX.BIN (make_self)"
                VERBATIM)

        # LTO is deliberately NOT enabled here, unlike every other branch.
        #
        # This is the one target where Clang compiles and ppu-g++ links (see
        # cmake/PS3.cmake). -flto makes Clang emit LLVM bitcode rather than
        # PowerPC objects, and ps3dev's GCC/binutils has no LLVMgold plugin to
        # read it, so the link would fail on every object. Worse, it would fail
        # late and confusingly: check_ipo_supported() asks the COMPILER whether
        # it supports IPO -- Clang says yes -- and cmake/PS3.cmake sets
        # CMAKE_TRY_COMPILE_TARGET_TYPE to STATIC_LIBRARY, so that probe never
        # exercises a real link and never sees the problem.
        #
        # Revisit only alongside the compiler split itself: either both halves
        # become Clang (needs lld plus PSL1GHT's linker scripts verified
        # against it) or both become GCC (needs #embed, i.e. ps3dev on GCC 15 --
        # see .ignore/ps3-cmake-build-setup/NOTES.md).

    elseif(TARGET_PLATFORM STREQUAL "nds" OR TARGET_PLATFORM STREQUAL "dsi")
        project(pnes CXX)

        FetchContent_MakeAvailable(intsh tuple)
        _make_consh_available()
        _fixup_fetched_cxx_std_targets()

        # The DS backend maps the shared PPU bookkeeping (src/emu) onto libnds 2D
        # hardware: the emulator and all the nametable/OAM/palette write functions
        # are platform-agnostic and compiled here alongside the libnds video/
        # present, the keypad input, and the (stub) audio.
        file(GLOB NDS_SOURCES CONFIGURE_DEPENDS src/nds/*.cpp src/emu/*.cpp src/emu/mappers/*.cpp)
        file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/all/*.cpp)
        add_library(pnes STATIC ${NDS_SOURCES} ${ALL_SOURCES})
        target_include_directories(pnes PUBLIC
                ${CMAKE_SOURCE_DIR}/include
                ${CMAKE_SOURCE_DIR}/include/umbrella
        )
        # TARGET_NDS is the group the engine headers gate on (native 2D hardware,
        # 32x24 viewport, no SDL headers). The DSi build adds TARGET_DSI on top so
        # the audio path can opt into the larger RAM later.
        target_compile_definitions(pnes PUBLIC TARGET_NDS)
        if(TARGET_PLATFORM STREQUAL "dsi")
            target_compile_definitions(pnes PUBLIC TARGET_DSI)
            set(DEMO_TITLE_DIR dsi)
        else()
            set(DEMO_TITLE_DIR ds)
        endif()
        # Alternative nametable wiring (see the TARGET_NES branch's own comment,
        # near the top of this file): a fact about which board a build is
        # modeling, not something exclusive to the NES target itself.
        if(DEFINED ALTERNATIVE_NAMETABLE)
            target_compile_definitions(pnes PUBLIC "ALTERNATIVE_NAMETABLE=${ALTERNATIVE_NAMETABLE}")
        endif()
        target_link_libraries(pnes PUBLIC br0::intsh br0::tuple br0::consh)

        file(GLOB_RECURSE DEMO_SOURCES CONFIGURE_DEPENDS demo/*.cpp)
        list(FILTER DEMO_SOURCES EXCLUDE REGEX "/demo/gen/(title|gameOptions)/")
        file(GLOB DEMO_TITLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/demo/gen/title/${DEMO_TITLE_DIR}/*.cpp")
        list(APPEND DEMO_SOURCES ${DEMO_TITLE_SOURCES})
        file(GLOB DEMO_GAMEOPTIONS_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/demo/gen/gameOptions/${DEMO_TITLE_DIR}/*.cpp")
        list(APPEND DEMO_SOURCES ${DEMO_GAMEOPTIONS_SOURCES})

        # chr_rom.cpp #embeds the packed blob (demo/chr/chr.bin) and emits it as the
        # cartridge's CHR ROM image. The blob is a blobman output, not a source file,
        # so declare it explicitly: repacking it must retrigger that compile.
        set_source_files_properties(${CMAKE_SOURCE_DIR}/demo/src/graphics/chr_rom.cpp
                PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/demo/chr/chr.bin")

        # `demo` is the packaged .nds ROM, `demo_elf` the linked ARM9 ELF -- see
        # the note on the 3DS branch's add_executable for why.
        add_executable(demo_elf ${DEMO_SOURCES})
        set_target_properties(demo_elf PROPERTIES OUTPUT_NAME demo SUFFIX ".elf")
        target_link_libraries(demo_elf PRIVATE pnes nds9)

        # See the TARGET_NES branch's own comment on this same call for GEN_TARGET_DIR.
        target_compile_definitions(demo_elf PRIVATE "GEN_TARGET_DIR=${DEMO_TITLE_DIR}")

        # CHR art is pulled in with #embed (C23); keep the Clang-only silencer for
        # parity with the other branches (no-op under devkitARM's GCC).
        target_compile_options(demo_elf PRIVATE $<$<CXX_COMPILER_ID:Clang>:-Wno-c23-extensions>)

        # Enable LTO for release-type configs, mirroring the other branches.
        include(CheckIPOSupported)
        check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
        if(_ipo_ok)
            foreach(_cfg Release MinSizeRel RelWithDebInfo)
                string(TOUPPER ${_cfg} _CFG)
                set_property(TARGET demo_elf pnes
                        PROPERTY INTERPROCEDURAL_OPTIMIZATION_${_CFG} ON)
            endforeach()
        else()
            message(STATUS "IPO/LTO not supported by this toolchain: ${_ipo_err}")
        endif()

        # Wrap the linked ELF as a .nds ROM (the default ARM7 core + libnds icon
        # come from calico automatically). nds_create_rom() comes from devkitARM's
        # Platform/NintendoDS.cmake once the NDS toolchain is active. The output is
        # named demo.nds so it matches the other targets' demo.<ext> convention.
        # The ROM target is `demo` itself (ARM9 names the ELF); it used to be a
        # separate `demo_nds`, which left `--target demo` at the bare ELF.
        nds_create_rom(demo
                ARM9 demo_elf
                NAME "pnes demo"
                SUBTITLE1 "NES engine demo on DS"
                SUBTITLE2 "github.com/brette"
                OUTPUT ${CMAKE_BINARY_DIR}/demo.nds
        )

    # ----------------------------------------------------------------------
    # Game Boy Advance target (devkitARM + libgba), via devkitPro's official
    # GBA.cmake toolchain (the gba-* presets point CMAKE_TOOLCHAIN_FILE at
    # $DEVKITPRO/cmake/GBA.cmake, exactly like the DS uses NDS.cmake).
    #
    # Same family as the DS: the GBA does NOT run the software rasteriser
    # (emu::GenerateFrame). It is a 2D tile console whose hardware is a near-cousin
    # of the NES PPU, so this backend maps the NES PPU onto the GBA's NATIVE 2D
    # hardware (Model B):
    #   * CHR pattern tables  -> GBA character (tile) VRAM (2bpp -> 4bpp expand)
    #   * nametable+attribute -> GBA BG tilemap entries (tile + palette + flip)
    #   * NES master palette  -> GBA BGR555 palette RAM
    #   * OAM shadow          -> GBA hardware OBJ (OAM written directly)
    #   * raster band timeline (emu::GenerateBands) -> a per-scanline HBlank scroll
    #     table, so mid-frame scroll changes and the sprite-0 split still work.
    # It therefore still pulls in src/emu for the platform-agnostic PPU bookkeeping
    # (nametable/OAM/palette writes, the band/IRQ timeline, the OAM shadow), but
    # the per-pixel rasteriser there goes unused.
    #
    # The 240x160 panel is the first target NARROWER than the NES (30 vs 32 tiles)
    # as well as shorter (20 vs 30); the backend shows a window onto the unaltered
    # 256x240 world (the engine never compromises the NES game for the GBA). Audio:
    # the GBA's tiny RAM has no room for baked PCM, so audio is stubbed
    # (src/gba/audio.cpp) exactly like the DS/GameCube/Wii backends -- the user
    # supplies a synthesiser. Only the tiny CHR art (a few KiB) is embedded.
    #
    # The toolchain ($DEVKITPRO/cmake/GBA.cmake) is supplied via the gba-* presets;
    # it imports devkitARM.cmake and routes CMAKE_SYSTEM_NAME=NintendoGBA to
    # devkitPro's Platform/NintendoGBA.cmake module, which sets the armv4t flags,
    # links libgba and provides the gba_create_rom() helper used below (gba.specs
    # crt0 + cart link script are applied by gba_create_rom itself).
    # ----------------------------------------------------------------------
    elseif(TARGET_PLATFORM STREQUAL "gba")
        project(pnes CXX)

        FetchContent_MakeAvailable(intsh tuple)
        _make_consh_available()
        _fixup_fetched_cxx_std_targets()

        set(DEMO_TITLE_DIR gba)

        # The GBA backend maps the shared PPU bookkeeping (src/emu) onto libgba 2D
        # hardware: the emulator and all the nametable/OAM/palette write functions
        # are platform-agnostic and compiled here alongside the libgba video/
        # present, the keypad input, and the (stub) audio.
        file(GLOB GBA_SOURCES CONFIGURE_DEPENDS src/gba/*.cpp src/emu/*.cpp src/emu/mappers/*.cpp)
        file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/all/*.cpp)
        add_library(pnes STATIC ${GBA_SOURCES} ${ALL_SOURCES})
        target_include_directories(pnes PUBLIC
                ${CMAKE_SOURCE_DIR}/include
                ${CMAKE_SOURCE_DIR}/include/umbrella
        )
        # TARGET_GBA is the group the engine headers gate on (native 2D hardware,
        # 30x20 viewport -- the first target narrower than the NES, no SDL headers).
        target_compile_definitions(pnes PUBLIC TARGET_GBA)
        # Alternative nametable wiring (see the TARGET_NES branch's own comment,
        # near the top of this file): a fact about which board a build is
        # modeling, not something exclusive to the NES target itself.
        if(DEFINED ALTERNATIVE_NAMETABLE)
            target_compile_definitions(pnes PUBLIC "ALTERNATIVE_NAMETABLE=${ALTERNATIVE_NAMETABLE}")
        endif()
        target_link_libraries(pnes PUBLIC br0::intsh br0::tuple br0::consh)

        file(GLOB_RECURSE DEMO_SOURCES CONFIGURE_DEPENDS demo/*.cpp)
        list(FILTER DEMO_SOURCES EXCLUDE REGEX "/demo/gen/(title|gameOptions)/")
        file(GLOB DEMO_TITLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/demo/gen/title/${DEMO_TITLE_DIR}/*.cpp")
        list(APPEND DEMO_SOURCES ${DEMO_TITLE_SOURCES})
        file(GLOB DEMO_GAMEOPTIONS_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/demo/gen/gameOptions/${DEMO_TITLE_DIR}/*.cpp")
        list(APPEND DEMO_SOURCES ${DEMO_GAMEOPTIONS_SOURCES})

        # chr_rom.cpp #embeds the packed blob (demo/chr/chr.bin) and emits it as the
        # cartridge's CHR ROM image. The blob is a blobman output, not a source file,
        # so declare it explicitly: repacking it must retrigger that compile.
        set_source_files_properties(${CMAKE_SOURCE_DIR}/demo/src/graphics/chr_rom.cpp
                PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/demo/chr/chr.bin")

        add_executable(demo ${DEMO_SOURCES})
        set_target_properties(demo PROPERTIES SUFFIX ".elf")
        # libgba (-lgba) + gba.specs come in via the platform module's standard
        # libraries/linker flags, so only the engine archive is linked explicitly.
        target_link_libraries(demo PRIVATE pnes)

        # See the TARGET_NES branch's own comment on this same call for GEN_TARGET_DIR.
        target_compile_definitions(demo PRIVATE "GEN_TARGET_DIR=${DEMO_TITLE_DIR}")

        # CHR art is pulled in with #embed (C23); keep the Clang-only silencer for
        # parity with the other branches (no-op under devkitARM's GCC).
        target_compile_options(demo PRIVATE $<$<CXX_COMPILER_ID:Clang>:-Wno-c23-extensions>)

        # Enable LTO for release-type configs, mirroring the other branches.
        include(CheckIPOSupported)
        check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
        if(_ipo_ok)
            foreach(_cfg Release MinSizeRel RelWithDebInfo)
                string(TOUPPER ${_cfg} _CFG)
                set_property(TARGET demo pnes
                        PROPERTY INTERPROCEDURAL_OPTIMIZATION_${_CFG} ON)
            endforeach()
        else()
            message(STATUS "IPO/LTO not supported by this toolchain: ${_ipo_err}")
        endif()

        # Wrap the linked ELF as a header-fixed .gba ROM. devkitPro's
        # gba_create_rom() operates on the executable target directly: it adds the
        # cart link options (-specs=gba.specs) to `demo`, then POST_BUILD runs
        # objcopy -O binary + gbafix (Nintendo logo checksum/complement + the
        # title/game/maker codes). Output is named demo.gba to match demo.<ext>.
        gba_create_rom(demo
                OUTPUT ${CMAKE_BINARY_DIR}/demo.gba
                TITLE "PNES"
                GAMECODE "CPNE"
                MAKERCODE "00"
        )

    # ----------------------------------------------------------------------
    # Desktop targets (macOS, Windows, Linux)
    # ----------------------------------------------------------------------
else()
    project(pnes CXX)

    FetchContent_MakeAvailable(intsh tuple)
    _make_consh_available()
    _fixup_fetched_cxx_std_targets()

    set(DEMO_TITLE_DIR pc)

    if(TARGET_PLATFORM STREQUAL "mac")
        set(PLATFORM_DEFINE TARGET_MACOS)
    elseif(TARGET_PLATFORM STREQUAL "win")
        set(PLATFORM_DEFINE TARGET_WINDOWS)
    elseif(TARGET_PLATFORM STREQUAL "linux")
        set(PLATFORM_DEFINE TARGET_LINUX)
    else()
        message(FATAL_ERROR "Set -DTARGET_PLATFORM to one of: nes, win, mac, linux")
    endif()

    # ------------------------------------------------------------------
    # mac-arm64-* builds ON Apple Silicon hardware -- there is no cross
    # toolchain wired up for it from an Intel host (unlike win-arm64/
    # linux-arm64, which both have a real, demonstrated cross path: MSVC's
    # ARM64 import libs via vcvarsall/CMAKE_C(XX)_COMPILER_TARGET for
    # Windows, and the aarch64 cross toolchain + multiarch dev packages
    # baked into docker/Dockerfile.linux for Linux -- see the pkgconfig
    # check a little further down for the Linux case, and win-x86's total
    # absence of any such guard for the Windows case). Configuring
    # mac-arm64 from an IDE profile picker on an Intel Mac would silently
    # produce a binary the developer can't run or test locally, so refuse
    # it here.
    #
    # This is deliberately narrower than it once was: a previous version of
    # this check applied to all three arm64 platforms, gating on host CPU
    # (or /.dockerenv as a Linux-only escape hatch) rather than on whether
    # a cross toolchain is actually present. That was the wrong signal --
    # it broke the win-arm64 CI job's genuine vcvarsall-based cross build,
    # and would equally have broken a Linux dev who installed
    # gcc-aarch64-linux-gnu natively instead of going through Docker. Each
    # platform now gets a check that reflects what's actually true for it.
    #
    # CMAKE_HOST_SYSTEM_PROCESSOR (not CMAKE_SYSTEM_PROCESSOR, which
    # reflects the TARGET when cross-compiling) is the real host CPU as
    # registry-reported -- arm64 on macOS.
    # ------------------------------------------------------------------
    if(TARGET_PLATFORM STREQUAL "mac" AND CMAKE_OSX_ARCHITECTURES MATCHES "arm64"
            AND NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|ARM64)$")
        message(FATAL_ERROR
                "This preset targets arm64 (Apple Silicon), but the host CPU "
                "is '${CMAKE_HOST_SYSTEM_PROCESSOR}'. mac-arm64 builds ON "
                "Apple Silicon hardware, not cross-compile to it from an "
                "Intel Mac -- run this natively on an Apple Silicon machine "
                "instead.")
    endif()

    # Alternative nametable wiring (see the TARGET_NES branch's own comment,
    # above): this is a fact about which board a build is modeling, not
    # something exclusive to the NES target itself, so the desktop preview
    # needs the same define to route mmc3.cpp's strong ppu::ReadNametable/
    # WriteNametable overrides correctly off-NES too -- otherwise
    # ALTERNATIVE_NAMETABLE reads as 0 in every #if here regardless of what
    # local.cmake actually sets.
    if(DEFINED ALTERNATIVE_NAMETABLE)
        list(APPEND PLATFORM_DEFINE "ALTERNATIVE_NAMETABLE=${ALTERNATIVE_NAMETABLE}")
    endif()

    include(FetchContent)

    # ------------------------------------------------------------------
    # On Linux, link against the system's SDL3 and libusb-1.0 instead of
    # vendoring and building our own copies. libusb is LGPL-2.1, which
    # requires dynamic linking against a distro-swappable library anyway;
    # SDL3 (zlib license) has no such requirement, but distros commonly
    # package it now (Arch, Fedora, Debian trixie+, Tumbleweed, Gentoo,
    # NixOS) and building it from source every time is otherwise pure
    # waste. Same policy for both: prefer the system package on Linux,
    # keep vendoring on platforms (Windows/Mac) where one isn't
    # reliably available.
    #
    # NOTE: requires libsdl3-dev + libusb-1.0 and their pkg-config files
    # installed on the host (libsdl3-dev + libusb-1.0-0-dev on Debian/
    # Ubuntu trixie+, SDL3-devel + libusbx-devel on Fedora/RHEL). Older
    # LTS distros (Ubuntu 22.04/24.04, Debian bookworm, RHEL) don't carry
    # SDL3 yet and need a PPA/backport or a source build override.
    # ------------------------------------------------------------------
    if(TARGET_PLATFORM STREQUAL "linux")
        # ------------------------------------------------------------------
        # 32-bit (i686) builds must resolve pkg-config against the 32-bit
        # sdl3/libusb-1.0 .pc files, not the native x64 ones -- pkg-config
        # itself doesn't know or care about target arch, so left alone it
        # would happily hand back the wrong ELF class and fail at link time.
        # Distros disagree on where those 32-bit .pc files live (Debian/
        # Ubuntu's i386 multiarch triplet dir vs Arch/openSUSE's /usr/lib32),
        # so detect whichever is actually present rather than hardcoding one
        # -- that hardcoding used to live in CMakePresets.json's
        # linux-x86-release preset and broke the moment this was tried
        # natively on Arch (see git history). /usr/share/pkgconfig is always
        # appended: arch-independent .pc files (e.g. xproto) live there and
        # are dropped from the search entirely if PKG_CONFIG_LIBDIR omits it.
        # ------------------------------------------------------------------
        if(CMAKE_CXX_COMPILER_TARGET MATCHES "^i[3-6]86-")
            foreach(_x86_pkgconfig_dir
                    /usr/lib32/pkgconfig
                    /usr/lib/i386-linux-gnu/pkgconfig)
                if(EXISTS ${_x86_pkgconfig_dir})
                    set(ENV{PKG_CONFIG_LIBDIR} "${_x86_pkgconfig_dir}:/usr/share/pkgconfig")
                    break()
                endif()
            endforeach()
            if(NOT DEFINED ENV{PKG_CONFIG_LIBDIR})
                message(FATAL_ERROR
                        "32-bit Linux build: couldn't find a 32-bit pkgconfig "
                        "dir (checked /usr/lib32/pkgconfig, "
                        "/usr/lib/i386-linux-gnu/pkgconfig). Install the "
                        "32-bit sdl3/libusb-1.0 dev packages for your distro, "
                        "or set PKG_CONFIG_LIBDIR yourself before configuring.")
            endif()
        endif()

        # ------------------------------------------------------------------
        # Same problem, mirrored for cross-compiling TO aarch64 from a
        # non-arm64 host (the only way linux-arm64-release reaches this
        # branch at all -- the host-arch guard above only lets that happen
        # inside a Docker container, e.g. docker/Dockerfile.linux's
        # crossbuild-essential-arm64 + libsdl3-dev:arm64 install). A NATIVE
        # arm64 host needs no override: its default pkgconfig search path
        # already points at its own native .pc files. Debian/Ubuntu's arm64
        # multiarch triplet dir is the only layout this project's Docker
        # image uses, so unlike the i686 case above there's just the one
        # candidate to check.
        # ------------------------------------------------------------------
        if(CMAKE_CXX_COMPILER_TARGET MATCHES "^aarch64-"
                AND NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
            if(EXISTS /usr/lib/aarch64-linux-gnu/pkgconfig)
                set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig")
            else()
                message(FATAL_ERROR
                        "Cross-compiling to aarch64 Linux: couldn't find "
                        "/usr/lib/aarch64-linux-gnu/pkgconfig. Install the "
                        "aarch64 sdl3/libusb-1.0 dev packages for your distro, "
                        "or set PKG_CONFIG_LIBDIR yourself before configuring.")
            endif()
        endif()

        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL3   REQUIRED IMPORTED_TARGET sdl3)
        pkg_check_modules(LibUSB REQUIRED IMPORTED_TARGET libusb-1.0)

        if(NOT TARGET SDL3::SDL3-static)
            add_library(SDL3::SDL3-static ALIAS PkgConfig::SDL3)
        endif()
        if(NOT TARGET LibUSB::LibUSB)
            add_library(LibUSB::LibUSB ALIAS PkgConfig::LibUSB)
        endif()
    else()
        # ------------------------------------------------------------------
        # Fetch SDL3 source and build it statically alongside the project.
        # ------------------------------------------------------------------
        set(SDL3_GIT_TAG "release-3.2.16" CACHE STRING "SDL3 git tag to fetch")

        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON  CACHE BOOL "" FORCE)
        set(SDL_TEST   OFF CACHE BOOL "" FORCE)
        set(SDL_TESTS  OFF CACHE BOOL "" FORCE)
        set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
        set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
        set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
        set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
        set(SDL_POWER OFF CACHE BOOL "" FORCE)
        set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
        set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
        set(SDL_GPU OFF CACHE BOOL "" FORCE)
        set(SDL_RENDER_GPU OFF CACHE BOOL "" FORCE)

        FetchContent_Declare(
                SDL3
                GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
                GIT_TAG        ${SDL3_GIT_TAG}
                GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(SDL3)
    endif()

    # The SDL3 backend presents the shared software PPU (src/emu): the emulator
    # and all the nametable/OAM/palette write functions are platform-agnostic
    # and compiled here alongside the SDL-specific window/present/input/audio.
    file(GLOB SDL3_SOURCES CONFIGURE_DEPENDS src/SDL3/*.cpp src/emu/*.cpp src/emu/mappers/*.cpp)
    file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/all/*.cpp)
    add_library(pnes STATIC ${SDL3_SOURCES} ${ALL_SOURCES})
    target_include_directories(pnes PUBLIC
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/include/umbrella
    )
    target_compile_definitions(pnes PUBLIC ${PLATFORM_DEFINE})
    target_link_libraries(pnes PUBLIC SDL3::SDL3-static)
    target_link_libraries(pnes PUBLIC br0::intsh br0::tuple br0::consh)

    if(TARGET_PLATFORM STREQUAL "linux")
        target_link_libraries(pnes PUBLIC PkgConfig::LibUSB)
    endif()

    file(GLOB_RECURSE DEMO_SOURCES CONFIGURE_DEPENDS demo/*.cpp)
    list(FILTER DEMO_SOURCES EXCLUDE REGEX "/demo/gen/(title|gameOptions)/")
    file(GLOB DEMO_TITLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/demo/gen/title/${DEMO_TITLE_DIR}/*.cpp")
    list(APPEND DEMO_SOURCES ${DEMO_TITLE_SOURCES})
    file(GLOB DEMO_GAMEOPTIONS_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/demo/gen/gameOptions/${DEMO_TITLE_DIR}/*.cpp")
    list(APPEND DEMO_SOURCES ${DEMO_GAMEOPTIONS_SOURCES})

    # chr_rom.cpp #embeds the packed blob (demo/chr/chr.bin) and emits it as the
    # cartridge's CHR ROM image. The blob is a blobman output, not a source file,
    # so declare it explicitly: repacking it must retrigger that compile.
    set_source_files_properties(${CMAKE_SOURCE_DIR}/demo/src/graphics/chr_rom.cpp
            PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/demo/chr/chr.bin")

    add_executable(demo ${DEMO_SOURCES})
    target_link_libraries(demo PRIVATE pnes)

    # See the TARGET_NES branch's own comment on this same call for GEN_TARGET_DIR.
    target_compile_definitions(demo PRIVATE "GEN_TARGET_DIR=${DEMO_TITLE_DIR}")

    # CHR art is pulled in with #embed (C23), which Clang flags as an extension
    # in C++ mode. It is the intended mechanism here, so silence the diagnostic.
    # Clang-only: GCC doesn't know this flag and would emit an "unrecognized
    # option" note, and its own #embed support doesn't warn here anyway.
    target_compile_options(demo PRIVATE $<$<CXX_COMPILER_ID:Clang>:-Wno-c23-extensions>)

    # Enable LTO for release-type configs on desktop targets. CMake routes
    # archive/link through gcc-ar/gcc-ranlib (GCC) or lld (Clang) automatically
    # once INTERPROCEDURAL_OPTIMIZATION is set on the target.
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
    if(_ipo_ok)
        foreach(_cfg Release MinSizeRel RelWithDebInfo)
            string(TOUPPER ${_cfg} _CFG)
            set_property(TARGET demo pnes
                    PROPERTY INTERPROCEDURAL_OPTIMIZATION_${_CFG} ON)
        endforeach()
    else()
        message(STATUS "IPO/LTO not supported by this toolchain: ${_ipo_err}")
    endif()

    # Desktop runs the demo from its own directory and loads tracks/ + sfx/ by
    # relative path, so they are copied next to the executable.
    add_custom_command(TARGET demo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/demo/audio/tracks
            $<TARGET_FILE_DIR:demo>/tracks
    )

    add_custom_command(TARGET demo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/demo/audio/sfx
            $<TARGET_FILE_DIR:demo>/sfx
    )

    # ------------------------------------------------------------------
    # Strip the final executable in non-Debug builds. MSVC has no strip
    # (symbols live in a separate .pdb), so only run when CMAKE_STRIP is
    # available (GCC/Clang/MinGW toolchains).
    #
    # CMAKE_STRIP is whatever host binutils `strip` CMake found -- for the
    # linux-x86-release cross build that's fine, since x86_64 binutils'
    # strip/objcopy understands the i386 ELF machine type natively (biarch).
    # It is NOT fine for linux-arm64-release: the host's strip can't
    # recognise the aarch64 ELF machine type at all ("Unable to recognise
    # the format of the input file"), so the cross-arch aarch64-linux-gnu-*
    # strip from binutils-aarch64-linux-gnu (installed in
    # docker/Dockerfile.linux alongside the rest of the arm64 cross bits)
    # is used instead when it's present.
    # ------------------------------------------------------------------
    set(_PNES_STRIP "${CMAKE_STRIP}")
    if(CMAKE_CXX_COMPILER_TARGET MATCHES "^aarch64-"
            AND NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        find_program(_PNES_AARCH64_STRIP aarch64-linux-gnu-strip)
        if(_PNES_AARCH64_STRIP)
            set(_PNES_STRIP "${_PNES_AARCH64_STRIP}")
        endif()
    endif()

    if(_PNES_STRIP AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_custom_command(TARGET demo POST_BUILD
                COMMAND ${_PNES_STRIP} $<TARGET_FILE:demo>
                COMMENT "Stripping demo executable"
        )
    endif()

    # ------------------------------------------------------------------
    # AppImage packaging (Linux only). Produces a single-file executable
    # bundling the demo, SDL3's dynamic deps, and the tracks/sfx assets.
    # Runs on any glibc >= 2.31 distro with no install step.
    # Build with: cmake --build <dir> --target demo-appimage
    # ------------------------------------------------------------------
    if(TARGET_PLATFORM STREQUAL "linux")
        find_program(LINUXDEPLOY linuxdeploy)
        if(LINUXDEPLOY)
            set(APPDIR "${CMAKE_BINARY_DIR}/AppDir")

            # The forced-bundle libasound path below is a multiarch triplet
            # directory, which differs between the linux-release (x64),
            # linux-x86-release, and linux-arm64-release presets; derive it
            # from the same compiler target the presets already set, rather
            # than hardcoding x64.
            if(CMAKE_CXX_COMPILER_TARGET MATCHES "^i[3-6]86-")
                set(_LINUX_MULTIARCH_TRIPLET "i386-linux-gnu")
            elseif(CMAKE_CXX_COMPILER_TARGET MATCHES "^aarch64-")
                set(_LINUX_MULTIARCH_TRIPLET "aarch64-linux-gnu")
            else()
                set(_LINUX_MULTIARCH_TRIPLET "x86_64-linux-gnu")
            endif()

            # linuxdeploy bundles its own x86_64 `strip` inside its AppImage
            # (it doesn't consult PATH for one), so when cross-packaging for
            # aarch64 on an x86_64 host it fails with "Unable to recognise
            # the architecture of the input file" on every ELF it deploys --
            # the same host/target mismatch _PNES_STRIP works around above,
            # but linuxdeploy has no equivalent --strip-tool flag to point
            # at aarch64-linux-gnu-strip instead. It does honour a documented
            # NO_STRIP env var to skip that step entirely, which is fine
            # here: the demo executable this AppImage bundles is already
            # stripped above, and libasound.so.2 doesn't need to be.
            set(_PNES_LINUXDEPLOY_ENV "")
            if(CMAKE_CXX_COMPILER_TARGET MATCHES "^aarch64-"
                    AND NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
                set(_PNES_LINUXDEPLOY_ENV "NO_STRIP=1")
            endif()

            configure_file(
                    "${CMAKE_SOURCE_DIR}/docker/demo.desktop.in"
                    "${CMAKE_BINARY_DIR}/demo.desktop"
                    @ONLY
            )

            # Custom AppRun — switches cwd to usr/bin so the demo finds
            # its bundled tracks/ and sfx/ via the same relative paths
            # used for local dev runs.
            file(WRITE "${CMAKE_BINARY_DIR}/AppRun"
"#!/bin/bash
HERE=\"$(dirname \"$(readlink -f \"\${0}\")\")\"
export LD_LIBRARY_PATH=\"\${HERE}/usr/lib:\${LD_LIBRARY_PATH}\"
cd \"\${HERE}/usr/bin\"
exec ./demo \"\$@\"
")

            # The AppImage bundles binary redistributions of libSDL3.so and
            # libusb-1.0.so alongside the demo, but both are now linked
            # against the host's system packages rather than vendored, so
            # we hold no local copy of either's license text to copy in —
            # those obligations fall on whoever built the system packages,
            # not on this build. Only our own LICENSE travels with the
            # bundle.
            set(_APPDIR_LICENSES "${APPDIR}/usr/share/doc/pnes")

            add_custom_target(demo-appimage
                    COMMAND ${CMAKE_COMMAND} -E rm -rf "${APPDIR}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${APPDIR}/usr/bin"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${_APPDIR_LICENSES}"
                    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:demo> "${APPDIR}/usr/bin/demo"
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                            "${CMAKE_SOURCE_DIR}/demo/audio/tracks" "${APPDIR}/usr/bin/tracks"
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                            "${CMAKE_SOURCE_DIR}/demo/audio/sfx"    "${APPDIR}/usr/bin/sfx"
                    COMMAND ${CMAKE_COMMAND} -E copy
                            "${CMAKE_SOURCE_DIR}/LICENSE"     "${_APPDIR_LICENSES}/LICENSE"
                    COMMAND ${CMAKE_COMMAND} -E copy
                            "${CMAKE_BINARY_DIR}/AppRun"      "${APPDIR}/AppRun"
                    COMMAND chmod +x "${APPDIR}/AppRun"
                    COMMAND ${CMAKE_COMMAND} -E env ${_PNES_LINUXDEPLOY_ENV}
                            ${LINUXDEPLOY}
                            --appdir "${APPDIR}"
                            --executable "${APPDIR}/usr/bin/demo"
                            --desktop-file "${CMAKE_BINARY_DIR}/demo.desktop"
                            --icon-file "${CMAKE_SOURCE_DIR}/demo/demo.png"
                            # Force-bundle libasound: AppImage's default excludelist skips it
                            # on the assumption every Linux desktop has ALSA, but minimal
                            # images (e.g., WSL's default Ubuntu) don't.
                            --library /usr/lib/${_LINUX_MULTIARCH_TRIPLET}/libasound.so.2
                            --output appimage
                    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
                    DEPENDS demo
                    COMMENT "Packaging AppImage"
                    VERBATIM
            )
        else()
            message(STATUS
                    "linuxdeploy not found on PATH; 'demo-appimage' target disabled. "
                    "Build inside the provided Docker image, or install linuxdeploy locally.")
        endif()
    endif()
endif()

# One REGION define for every target, regardless of which TARGET_PLATFORM
# branch above actually ran (exactly one does, and it's the only one that
# has created the pnes target by this point): 0 for NTSC (every
# target's default) or 1 for the nes_pal target set above.
target_compile_definitions(pnes PUBLIC REGION=${REGION})

# uitk-generated title.cpp (every target dir under demo/gen/title/) uses
# charmap_generic but never includes demo/src/graphics/charmaps.hpp itself --
# force it in ahead of every demo TU instead. Safe project-wide: it's just an
# inline constexpr object (see its own header comment -- no ODR/LTO cost, and
# nothing of the charmap itself reaches the ROM).
#
# The executable is `demo` on most targets, but `demo_elf` on the ones where
# `demo` is the packaged deliverable instead (3DS, Switch, Wii U, DS/DSi -- see
# the note on the 3DS branch's add_executable).
if(TARGET demo_elf)
    set(DEMO_EXE_TARGET demo_elf)
else()
    set(DEMO_EXE_TARGET demo)
endif()
target_compile_options(${DEMO_EXE_TARGET} PRIVATE -include ${CMAKE_SOURCE_DIR}/demo/src/graphics/charmaps.hpp)