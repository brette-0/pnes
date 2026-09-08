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

# Application / SDL3 window title, compiled in as the PROJECT_NAME define.
# Set PROJECT_NAME in local.cmake to override per-developer; a default keeps
# the window titled when local.cmake is absent or leaves it unset.
#
# NOTE: this must stay ABOVE the project() calls below -- project() sets its
# own PROJECT_NAME variable, which would otherwise clobber this value before
# the define is created.
if(NOT PROJECT_NAME)
    set(PROJECT_NAME "My Platform-NES Game")
endif()

# Capture the title for the AppImage .desktop file's Name= (configure_file
# substitutes @GAME_NAME@ in docker/demo.desktop.in). This must happen here,
# before project() resets the PROJECT_NAME *variable* to "platform-nes":
# appimagetool derives the output filename's prefix from Name=, and an empty
# Name produces a "-x86_64.AppImage" argument that mksquashfs rejects as a flag.
# project() does not touch GAME_NAME, so it survives to the linux block below.
set(GAME_NAME "${PROJECT_NAME}")

# Nametable mirroring orientation: platform-nes.hpp requires exactly one of
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
# usable from C++ in C++23. The gnu++ dialect (CMAKE_CXX_EXTENSIONS ON)
# keeps the GNU compound-literal extension the video/interrupt code uses.
# Both toolchains accept it: llvm-mos's clang++ and host GCC.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# ----------------------------------------------------------------------
# br0::intsh — header-only integer type aliases, used via #include <intsh>.
# An INTERFACE target, so the same declaration serves both the NES and
# desktop builds; FetchContent_MakeAvailable is called inside each branch
# below (after project()), and the target is linked PUBLIC onto the
# platform-nes library so the include dir propagates to the demo.
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
# NES target
# ----------------------------------------------------------------------
if(TARGET_PLATFORM STREQUAL "nes" OR TARGET_PLATFORM STREQUAL "nes_pal")
    if(TARGET_PLATFORM STREQUAL "nes_pal")
        set(REGION 1)
    endif()

    # Mapper 4 (MMC3), not NROM: linked against the generic nes-platform
    # crt0 (LLVM_MOS_PLATFORM=nes, not nes-nrom), since nes-nrom's own
    # crt0.o/link.ld hardcode NROM-specific __prg_rom_size/__chr_rom_size
    # defaults that this MMC3 build must supply itself (see mmc3-helper.ld's
    # COROLLARY-equivalent reasoning, carried over from vrc1-helper.ld).
    #
    # The generic nes platform has no default link.ld (unlike nes-nrom and
    # the other mapper subtargets), so -T must be supplied explicitly. This
    # has to happen *before* project() below: project() runs CMake's own
    # compiler-identification and ABI-detection probes, which both perform
    # real links, and without -T those fail before any of this project's
    # own code is ever touched ("cannot find linker script link.ld", then
    # a harder-to-place "No known features for CXX compiler" once
    # identification itself silently comes back empty).
    #
    # CMAKE_CXX_FLAGS, not CMAKE_EXE_LINKER_FLAGS: CMake's own
    # CMAKE_DETERMINE_COMPILER_ID probe builds its test invocation from
    # CMAKE_<LANG>_FLAGS specifically (see CMakeDetermineCompilerId.cmake) and
    # does not consult CMAKE_EXE_LINKER_FLAGS at all, so -T placed there is
    # invisible to that one probe even though it reaches every ordinary
    # target's link step. Compile-only steps harmlessly warn that -T is
    # unused; silenced below rather than left to spam every translation unit.
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

    # ------------------------------------------------------------------
    # Where the FamiStudio audio backend is placed. THIS BACKEND IS A PROJECT
    # CHOICE, NOT A LIB ONE -- audio.hpp declares plain prototypes with no
    # definition, and src/nes/audio/famistudio.cpp is the one .cpp that opts a
    # project into FamiStudio specifically by defining them. It tags every one
    # of its functions ::AUDIO_BANK, which expands to a section attribute
    # naming PLATFORM_NES_AUDIO_SECTION. That section MUST be the same PRG-ROM
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
    if(NOT PLATFORM_NES_AUDIO_SECTION)
        message(FATAL_ERROR
                "PLATFORM_NES_AUDIO_SECTION is not set. It names the ELF section "
                "src/nes/audio/famistudio.cpp is placed into, and must be the bank "
                "the audio engine is assembled into. See local.cmake.example.")
    endif()
    set_source_files_properties(${CMAKE_SOURCE_DIR}/src/nes/audio/famistudio.cpp
            PROPERTIES COMPILE_DEFINITIONS
            "PLATFORM_NES_AUDIO_SECTION=\"${PLATFORM_NES_AUDIO_SECTION}\"")

    # ------------------------------------------------------------------
    # Where the UI toolkit (src/all/extras/ui/*.cpp) is placed. Same
    # treatment as audio's PLATFORM_NES_AUDIO_SECTION above: COMPULSORY,
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
    if(NOT PLATFORM_NES_UI_SECTION)
        message(FATAL_ERROR
                "PLATFORM_NES_UI_SECTION is not set. It names the ELF section "
                "src/all/extras/ui/*.cpp is placed into. See local.cmake.example.")
    endif()
    set(PLATFORM_NES_UI_SOURCES
            ${CMAKE_SOURCE_DIR}/src/all/extras/ui/canvas.cpp
            ${CMAKE_SOURCE_DIR}/src/all/extras/ui/tickbox.cpp
            ${CMAKE_SOURCE_DIR}/src/all/extras/ui/tileslider.cpp
            ${CMAKE_SOURCE_DIR}/src/all/extras/ui/text.cpp
            ${CMAKE_SOURCE_DIR}/src/all/extras/ui/singlechoice.cpp)
    set_source_files_properties(${PLATFORM_NES_UI_SOURCES}
            PROPERTIES COMPILE_DEFINITIONS
            "PLATFORM_NES_UI_SECTION=\"${PLATFORM_NES_UI_SECTION}\"")

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
        if(PLATFORM_NES_${_MOD}_SECTION AND LLVM_MOS_PLATFORM STREQUAL "nes-nrom")
            message(WARNING
                    "PLATFORM_NES_${_MOD}_SECTION is set, but this is an NROM "
                    "build: one fixed 32 KiB bank, nowhere to move code to. "
                    "MODULE_PLACEMENT is inert here, so the section is ignored "
                    "rather than silently costing you the module's inlining.")
        endif()
        if(PLATFORM_NES_${_MOD}_SECTION)
            set_source_files_properties(${CMAKE_SOURCE_DIR}/src/nes/${_mod}.cpp
                    PROPERTIES COMPILE_DEFINITIONS
                    "PLATFORM_NES_${_MOD}_SECTION=\"${PLATFORM_NES_${_MOD}_SECTION}\"")
            message(STATUS "platform-nes: ${_mod} module placed in "
                           "${PLATFORM_NES_${_MOD}_SECTION} (inlining disabled for it)")
        endif()
    endforeach()

    set(LLVM_MOS_PLATFORM nes)
    find_package(llvm-mos-sdk REQUIRED)
    project(platform-nes CXX)
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
    # linked demo.elf (see include/platform-nes/logger.hpp) and writes
    # logdata.lua next to demo.nes for tools/logger.lua to load in Mesen.
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
    # pins this ahead of platform-nes, the first target that compiles any of
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
    # PLATFORM_NES_AUDIO_SECTION and fed in (see the encoder further down).
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
    # PLATFORM_NES_AUDIO_SECTION -> ca65 segment name.
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
    string(REPLACE "_" "__" FAMISTUDIO_SEGMENT "${PLATFORM_NES_AUDIO_SECTION}")
    string(REPLACE "." "_p" FAMISTUDIO_SEGMENT "${FAMISTUDIO_SEGMENT}")
    set(FAMISTUDIO_SEGMENT "${FAMISTUDIO_SEGMENT}_fx")

    # file(CONFIGURE) rather than file(WRITE): it leaves the file untouched
    # when the content is unchanged, so a re-configure doesn't bump the
    # timestamp and force a needless reassembly through the DEPENDS below.
    set(PNES_AUDIO_SEGMENT_INC
            ${CMAKE_BINARY_DIR}/famistudio_include/pnes_audio_segment.inc)
    file(CONFIGURE OUTPUT ${PNES_AUDIO_SEGMENT_INC} CONTENT
"; GENERATED by CMakeLists.txt from PLATFORM_NES_AUDIO_SECTION -- do not edit.
; ${PLATFORM_NES_AUDIO_SECTION} in ca65/XO65 segment spelling.
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
    add_library(platform-nes STATIC ${NES_SOURCES})

    # lua_log_builder built and ready before any project NES source compiles
    # -- see this file's own comment on lua_log_builder_tool, above.
    add_dependencies(platform-nes lua_log_builder_tool)

    # Unconditional, in every build type -- not just nes-debug. log()'s whole
    # LMA-resolution scheme (include/platform-nes/logger.hpp, tools/logger.lua)
    # depends on DWARF's .debug_line table to turn a log() call site's
    # __FILE__/__LINE__ into a real address; without -g that table doesn't
    # exist and every log point silently resolves to nothing. This costs
    # nothing in the shipped ROM either way: debug sections carry no
    # SHF_ALLOC flag, so they were never going to reach a MEMORY region or
    # get copied into demo.nes by objcopy regardless of build type -- the
    # only thing -g grows is demo.nes.elf, which is not what ships.
    target_compile_options(platform-nes PUBLIC -g)

    target_include_directories(platform-nes PUBLIC
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/include/umbrella
            ${CMAKE_BINARY_DIR}/famistudio_include
    )
    target_compile_definitions(platform-nes PUBLIC
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
            # above). PLATFORM_NES_AUDIO_SECTION is the only placement value
            # the C++ side still takes, and it goes to famistudio.cpp alone.
    )
    target_link_libraries(platform-nes PUBLIC br0::intsh br0::tuple)

    file(GLOB_RECURSE DEMO_C_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/demo/*.cpp)

    set(PASS1_ELF ${CMAKE_BINARY_DIR}/demo_pass1.elf)
    add_custom_command(
            OUTPUT ${PASS1_ELF}
            COMMAND ${CMAKE_CXX_COMPILER}
            # This hand-rolled invocation bypasses CMAKE_CXX_STANDARD and the
            # demo target's options, so the standard and the #embed extension
            # silencer must be set explicitly to match the demo target. -x c++
            # forces the C++ frontend for the source inputs.
            -std=gnu++23 -Wno-c23-extensions -x c++
            -Os ${PLATFORM_DEFINE_FLAGS} -D${ORIENTATION}
            # Reserve 10 ZP bytes for FamiStudio (8 vars + 2 alignment slack).
            # mos-nes.cfg defaults to -mlto-zp=224 (the full region); capping at
            # 214 prevents mos-zero-page-alloc from filling the last 10 bytes and
            # leaving no room for FamiStudio's ZEROPAGE segment.
            -mlto-zp=214
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
            ${DEMO_C_SOURCES}
            # Stop forcing the C++ frontend before the archive/object inputs
            # so the driver treats them as link inputs, not source files.
            -x none
            $<TARGET_FILE:platform-nes>
            ${FAMISTUDIO_OBJ}
            ${DEMO_ASM_OBJECTS}
            -o ${CMAKE_BINARY_DIR}/demo_pass1
            DEPENDS platform-nes
            ${FAMISTUDIO_OBJ}
            ${DEMO_ASM_OBJECTS}
            ${DEMO_C_SOURCES}
            COMMENT "Linking diagnostic ELF for ZP/BSS measurement"
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

    # metatiles.cpp includes chr.hpp, which #embeds the demo/chr art files and
    # (being the includer) emits the folded CHR ROM image. Declare the art as an
    # explicit dependency so editing it retriggers a recompile.
    file(GLOB DEMO_CHR_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/demo/chr/*)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/demo/src/graphics/metatiles.cpp
            PROPERTIES OBJECT_DEPENDS "${DEMO_CHR_FILES}")

    add_executable(demo ${DEMO_C_SOURCES})
    set_target_properties(demo PROPERTIES SUFFIX ".nes")

    target_link_libraries(demo PRIVATE platform-nes)

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
    # logdata.lua generation. See include/platform-nes/logger.hpp and
    # tools/luaLogBuilder/main.cpp's own header comments for the whole
    # design -- this just runs the already-built host tool (see
    # lua_log_builder_tool, above) against the ELF this link just produced,
    # writing logdata.lua next to demo.nes itself (where tools/logger.lua
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

    # FamiStudio and the hand-written assembly objects are required by
    # every build configuration.
    target_link_options(demo PRIVATE
            -mlto-zp=214   # leaves 10 bytes at the top of zero page for FamiStudio's own ZEROPAGE segment
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
elseif(TARGET_PLATFORM STREQUAL "gc" OR TARGET_PLATFORM STREQUAL "wii")
    project(platform-nes CXX)

    FetchContent_MakeAvailable(intsh tuple)

    if(TARGET_PLATFORM STREQUAL "gc")
        set(PLATFORM_DEFINE TARGET_GC)
    else()
        set(PLATFORM_DEFINE TARGET_WII)
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
    add_library(platform-nes STATIC ${OGC_SOURCES} ${ALL_SOURCES})
    target_include_directories(platform-nes PUBLIC
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/include/umbrella
    )
    # TARGET_OGC is the shared "GameCube-or-Wii" group the engine headers gate on
    # (emulated PPU like SDL, but a fixed viewport and no SDL headers); TARGET_GC
    # / TARGET_WII select the console-specific bits (e.g. WPAD on Wii only).
    target_compile_definitions(platform-nes PUBLIC ${PLATFORM_DEFINE} TARGET_OGC)

    # The on-target statistical PC sampler + on-screen overlay (src/ogc/
    # profiler.cpp and the overlay path in video.cpp) is a development tool, not
    # a shipping feature: it spins up a high-priority sampler thread and draws a
    # hex/bar overlay every frame. Compile it only into Debug builds via the
    # OGC_PROFILER gate, so Release .dols carry neither the thread nor the draw.
    # PRIVATE: only the library's own TUs (profiler.cpp/video.cpp) consult it.
    target_compile_definitions(platform-nes PRIVATE $<$<CONFIG:Debug>:OGC_PROFILER>)
    # asnd: libogc's ASND audio library (src/ogc/audio.cpp), not pulled in by
    # OGC_STANDARD_LIBRARIES (-logc -lm) on its own.
    target_link_libraries(platform-nes PUBLIC br0::intsh br0::tuple asnd)

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
    target_include_directories(platform-nes PRIVATE ${stb_SOURCE_DIR})

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
    add_dependencies(platform-nes ogc-assets)
    # audio.cpp resolves `#embed <pc_audio.ogg>` via --embed-dir (the canonical
    # GCC mechanism; -I is not reliably searched for #embed). Scope both the
    # search dir and the rebuild-on-change dependency to that one source.
    set_source_files_properties(${CMAKE_SOURCE_DIR}/src/ogc/audio.cpp
            PROPERTIES
                OBJECT_DEPENDS  ${OGC_OGG}
                COMPILE_OPTIONS "--embed-dir=${OGC_ASSET_DIR}")

    file(GLOB_RECURSE DEMO_SOURCES CONFIGURE_DEPENDS demo/*.cpp)

    # metatiles.cpp includes chr.hpp, which #embeds the demo/chr art files and
    # (being the includer) emits the folded CHR ROM image. Declare the art as an
    # explicit dependency so editing it retriggers a recompile.
    file(GLOB DEMO_CHR_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/demo/chr/*)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/demo/src/graphics/metatiles.cpp
            PROPERTIES OBJECT_DEPENDS "${DEMO_CHR_FILES}")

    add_executable(demo ${DEMO_SOURCES})
    set_target_properties(demo PROPERTIES SUFFIX ".elf")
    target_link_libraries(demo PRIVATE platform-nes)

    # CHR art is pulled in with #embed (C23). devkitPPC's GCC supports it without
    # a diagnostic here; keep the Clang-only silencer guard for parity with the
    # other branches (no-op under GCC).
    target_compile_options(demo PRIVATE $<$<CXX_COMPILER_ID:Clang>:-Wno-c23-extensions>)

    # Enable LTO for release-type configs. The software PPU is the hot path and
    # spans the platform-nes static lib (emu::GenerateFrame) and the demo (the
    # per-frame IRQ handlers it calls back into); cross-module inlining matters
    # here. CMake routes archiving through gcc-ar/gcc-ranlib once
    # INTERPROCEDURAL_OPTIMIZATION is set, so the .dol-from-.elf step is
    # unaffected. Mirrors the desktop branch below.
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
    if(_ipo_ok)
        foreach(_cfg Release MinSizeRel RelWithDebInfo)
            string(TOUPPER ${_cfg} _CFG)
            set_property(TARGET demo platform-nes
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
    project(platform-nes CXX)

    FetchContent_MakeAvailable(intsh tuple)

    # The 3DS backend presents the shared software PPU (src/emu) through citro2d:
    # the emulator and all the nametable/OAM/palette write functions are
    # platform-agnostic and compiled here alongside the citro2d video/present,
    # the hid input, and the (stub) audio.
    file(GLOB CTR_SOURCES CONFIGURE_DEPENDS src/3ds/*.cpp src/emu/*.cpp src/emu/mappers/*.cpp)
    file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/all/*.cpp)
    add_library(platform-nes STATIC ${CTR_SOURCES} ${ALL_SOURCES})
    target_include_directories(platform-nes PUBLIC
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/include/umbrella
    )
    # TARGET_CTR is the group the engine headers gate on (emulated PPU like SDL,
    # but a fixed 32x30 viewport and no SDL headers -- same family as TARGET_OGC).
    target_compile_definitions(platform-nes PUBLIC TARGET_CTR)
    # Alternative nametable wiring (see the TARGET_NES branch's own comment,
    # near the top of this file): a fact about which board a build is
    # modeling, not something exclusive to the NES target itself.
    if(DEFINED ALTERNATIVE_NAMETABLE)
        target_compile_definitions(platform-nes PUBLIC "ALTERNATIVE_NAMETABLE=${ALTERNATIVE_NAMETABLE}")
    endif()
    target_link_libraries(platform-nes PUBLIC br0::intsh br0::tuple)

    file(GLOB_RECURSE DEMO_SOURCES CONFIGURE_DEPENDS demo/*.cpp)

    # metatiles.cpp includes chr.hpp, which #embeds the demo/chr art files and
    # (being the includer) emits the folded CHR ROM image. Declare the art as an
    # explicit dependency so editing it retriggers a recompile.
    file(GLOB DEMO_CHR_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/demo/chr/*)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/demo/src/graphics/metatiles.cpp
            PROPERTIES OBJECT_DEPENDS "${DEMO_CHR_FILES}")

    add_executable(demo ${DEMO_SOURCES})
    set_target_properties(demo PROPERTIES SUFFIX ".elf")
    target_link_libraries(demo PRIVATE platform-nes citro2d citro3d ctru m)

    # CHR art is pulled in with #embed (C23). devkitARM's GCC supports it without
    # a diagnostic here; keep the Clang-only silencer guard for parity with the
    # other branches (no-op under GCC).
    target_compile_options(demo PRIVATE $<$<CXX_COMPILER_ID:Clang>:-Wno-c23-extensions>)

    # Enable LTO for release-type configs, mirroring the OGC + desktop branches:
    # the software PPU (emu::GenerateFrame) spans the static lib and the demo's
    # per-frame IRQ handlers, so cross-module inlining matters on the hot path.
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
    if(_ipo_ok)
        foreach(_cfg Release MinSizeRel RelWithDebInfo)
            string(TOUPPER ${_cfg} _CFG)
            set_property(TARGET demo platform-nes
                    PROPERTY INTERPROCEDURAL_OPTIMIZATION_${_CFG} ON)
        endforeach()
    else()
        message(STATUS "IPO/LTO not supported by this toolchain: ${_ipo_err}")
    endif()

    # Wrap the linked ELF as a .3dsx (homebrew loader format) with an SMDH icon/
    # metadata banner. Both helpers come from devkitARM's Tools3DS.cmake once the
    # 3DS toolchain is active.
    ctr_generate_smdh(demo.smdh
            NAME "platform-nes demo"
            DESCRIPTION "NES engine demo on 3DS (citro2d)"
            AUTHOR "brette"
    )
    ctr_create_3dsx(demo
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
        project(platform-nes CXX)

        FetchContent_MakeAvailable(intsh tuple)

        # The Switch backend presents the shared software PPU (src/emu): the
        # emulator and all the nametable/OAM/palette write functions are
        # platform-agnostic and compiled here alongside the libnx framebuffer
        # video/present, the hid input, and the audout audio.
        file(GLOB NX_SOURCES CONFIGURE_DEPENDS src/switch/*.cpp src/emu/*.cpp src/emu/mappers/*.cpp)
        file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/all/*.cpp)
        add_library(platform-nes STATIC ${NX_SOURCES} ${ALL_SOURCES})
        target_include_directories(platform-nes PUBLIC
                ${CMAKE_SOURCE_DIR}/include
                ${CMAKE_SOURCE_DIR}/include/umbrella
        )
        # TARGET_NX is the group the engine headers gate on (emulated PPU, fixed
        # 32x30 viewport, no SDL headers -- same family as TARGET_OGC/TARGET_CTR).
        target_compile_definitions(platform-nes PUBLIC TARGET_NX)
        # Alternative nametable wiring (see the TARGET_NES branch's own comment,
        # near the top of this file): a fact about which board a build is
        # modeling, not something exclusive to the NES target itself.
        if(DEFINED ALTERNATIVE_NAMETABLE)
            target_compile_definitions(platform-nes PUBLIC "ALTERNATIVE_NAMETABLE=${ALTERNATIVE_NAMETABLE}")
        endif()
        target_link_libraries(platform-nes PUBLIC br0::intsh br0::tuple)

        file(GLOB_RECURSE DEMO_SOURCES CONFIGURE_DEPENDS demo/*.cpp)

        # metatiles.cpp #embeds the demo/chr art; declare it as a dependency so
        # editing the art retriggers a recompile.
        file(GLOB DEMO_CHR_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/demo/chr/*)
        set_source_files_properties(${CMAKE_SOURCE_DIR}/demo/src/graphics/metatiles.cpp
                PROPERTIES OBJECT_DEPENDS "${DEMO_CHR_FILES}")

        add_executable(demo ${DEMO_SOURCES})
        set_target_properties(demo PROPERTIES SUFFIX ".elf")
        target_link_libraries(demo PRIVATE platform-nes nx)

        # CHR art is pulled in with #embed (C23); keep the Clang-only silencer for
        # parity with the other branches (no-op under devkitA64's GCC).
        target_compile_options(demo PRIVATE $<$<CXX_COMPILER_ID:Clang>:-Wno-c23-extensions>)

        # Enable LTO for release-type configs, mirroring the other branches.
        include(CheckIPOSupported)
        check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
        if(_ipo_ok)
            foreach(_cfg Release MinSizeRel RelWithDebInfo)
                string(TOUPPER ${_cfg} _CFG)
                set_property(TARGET demo platform-nes
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
        add_dependencies(demo switch-assets)

        # Wrap the linked ELF as a homebrew .nro with a NACP metadata banner and
        # the RomFS asset tree. Both helpers come from devkitA64's NXTools.cmake
        # once the Switch toolchain is active.
        nx_generate_nacp(demo.nacp
                NAME "platform-nes demo"
                AUTHOR "brette"
                VERSION "1.0.0"
        )
        nx_create_nro(demo
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
        project(platform-nes CXX)

        FetchContent_MakeAvailable(intsh tuple)

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
        add_library(platform-nes STATIC ${WIIU_SOURCES} ${ALL_SOURCES})
        target_include_directories(platform-nes PUBLIC
                ${CMAKE_SOURCE_DIR}/include
                ${CMAKE_SOURCE_DIR}/include/umbrella
        )
        # TARGET_WIIU is the native-Wii-U define; the engine headers pair it with
        # the Switch (TARGET_NX) family for the widescreen 52x30 (416x240)
        # viewport, and exclude the SDL3 headers (this backend uses SDL2).
        target_compile_definitions(platform-nes PUBLIC TARGET_WIIU)
        # Alternative nametable wiring (see the TARGET_NES branch's own comment,
        # near the top of this file): a fact about which board a build is
        # modeling, not something exclusive to the NES target itself.
        if(DEFINED ALTERNATIVE_NAMETABLE)
            target_compile_definitions(platform-nes PUBLIC "ALTERNATIVE_NAMETABLE=${ALTERNATIVE_NAMETABLE}")
        endif()
        target_link_libraries(platform-nes PUBLIC SDL2::SDL2-static br0::intsh br0::tuple)

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
        target_include_directories(platform-nes PRIVATE ${stb_SOURCE_DIR})

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
        add_dependencies(platform-nes wiiu-assets)
        # audio.cpp resolves `#embed <pc_audio.ogg>` via --embed-dir (the canonical
        # GCC mechanism; -I is not reliably searched for #embed). Scope both the
        # search dir and the rebuild-on-change dependency to that one source.
        set_source_files_properties(${CMAKE_SOURCE_DIR}/src/wiiu/audio.cpp
                PROPERTIES
                    OBJECT_DEPENDS  ${WIIU_OGG}
                    COMPILE_OPTIONS "--embed-dir=${WIIU_ASSET_DIR}")

        file(GLOB_RECURSE DEMO_SOURCES CONFIGURE_DEPENDS demo/*.cpp)

        # metatiles.cpp #embeds the demo/chr art; declare it as a dependency so
        # editing the art retriggers a recompile.
        file(GLOB DEMO_CHR_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/demo/chr/*)
        set_source_files_properties(${CMAKE_SOURCE_DIR}/demo/src/graphics/metatiles.cpp
                PROPERTIES OBJECT_DEPENDS "${DEMO_CHR_FILES}")

        add_executable(demo ${DEMO_SOURCES})
        set_target_properties(demo PROPERTIES SUFFIX ".elf")
        # The demo supplies its own main() via the RESET macro, so the backend
        # builds with SDL_MAIN_HANDLED and SDL2main is deliberately NOT linked.
        target_link_libraries(demo PRIVATE platform-nes)

        # CHR art is pulled in with #embed (C23); keep the Clang-only silencer for
        # parity with the other branches (no-op under devkitPPC's GCC).
        target_compile_options(demo PRIVATE $<$<CXX_COMPILER_ID:Clang>:-Wno-c23-extensions>)

        # Enable LTO for release-type configs, mirroring the other branches.
        include(CheckIPOSupported)
        check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
        if(_ipo_ok)
            foreach(_cfg Release MinSizeRel RelWithDebInfo)
                string(TOUPPER ${_cfg} _CFG)
                set_property(TARGET demo platform-nes
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
        wut_create_rpx(demo)
        wut_create_wuhb(demo
                NAME      "platform-nes demo"
                SHORTNAME "platform-nes"
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
        project(platform-nes CXX)

        FetchContent_MakeAvailable(intsh tuple)

        # The PSP backend presents the shared software PPU (src/emu): the
        # emulator and all the nametable/OAM/palette write functions are
        # platform-agnostic and compiled here alongside the raw-VRAM video/
        # present, the pspctrl input, and the pspaudio streaming audio.
        file(GLOB PSP_SOURCES CONFIGURE_DEPENDS src/psp/*.cpp src/emu/*.cpp src/emu/mappers/*.cpp)
        file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/all/*.cpp)
        add_library(platform-nes STATIC ${PSP_SOURCES} ${ALL_SOURCES})
        target_include_directories(platform-nes PUBLIC
                ${CMAKE_SOURCE_DIR}/include
                ${CMAKE_SOURCE_DIR}/include/umbrella
        )
        # TARGET_PSP is the group the engine headers gate on (emulated PPU,
        # widescreen ~16:9 viewport shared with TARGET_NX/TARGET_WIIU, no SDL
        # headers). See this branch's own header comment for why this can't be
        # derived from CMAKE_SYSTEM_NAME the way the devkitPro branches' defines
        # effectively are.
        target_compile_definitions(platform-nes PUBLIC TARGET_PSP)
        # Alternative nametable wiring (see the TARGET_NES branch's own comment,
        # near the top of this file): a fact about which board a build is
        # modeling, not something exclusive to the NES target itself.
        if(DEFINED ALTERNATIVE_NAMETABLE)
            target_compile_definitions(platform-nes PUBLIC "ALTERNATIVE_NAMETABLE=${ALTERNATIVE_NAMETABLE}")
        endif()
        target_link_libraries(platform-nes PUBLIC br0::intsh br0::tuple)

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
        target_include_directories(platform-nes PRIVATE ${stb_SOURCE_DIR})

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
        add_dependencies(platform-nes psp-assets)
        # audio.cpp resolves `#embed <pc_audio.ogg>` via --embed-dir (the
        # canonical GCC mechanism; -I is not reliably searched for #embed).
        # Scope both the search dir and the rebuild-on-change dependency to
        # that one source.
        set_source_files_properties(${CMAKE_SOURCE_DIR}/src/psp/audio.cpp
                PROPERTIES
                    OBJECT_DEPENDS  ${PSP_OGG}
                    COMPILE_OPTIONS "--embed-dir=${PSP_ASSET_DIR}")

        file(GLOB_RECURSE DEMO_SOURCES CONFIGURE_DEPENDS demo/*.cpp)

        # metatiles.cpp #embeds the demo/chr art; declare it as a dependency so
        # editing the art retriggers a recompile.
        file(GLOB DEMO_CHR_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/demo/chr/*)
        set_source_files_properties(${CMAKE_SOURCE_DIR}/demo/src/graphics/metatiles.cpp
                PROPERTIES OBJECT_DEPENDS "${DEMO_CHR_FILES}")

        add_executable(demo ${DEMO_SOURCES})
        # pspdev's own crt0/linking wants the raw ELF extension for
        # create_pbp_file()/psp-fixup-imports below, not a bare "demo".
        set_target_properties(demo PROPERTIES SUFFIX ".elf")
        target_link_libraries(demo PRIVATE platform-nes pspdisplay pspctrl pspaudio pspge psppower)

        # CHR art is pulled in with #embed (C23); keep the Clang-only silencer
        # for parity with the other branches (no-op under psp-g++/GCC).
        target_compile_options(demo PRIVATE $<$<CXX_COMPILER_ID:Clang>:-Wno-c23-extensions>)

        # Enable LTO for release-type configs, mirroring the other branches.
        include(CheckIPOSupported)
        check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
        if(_ipo_ok)
            foreach(_cfg Release MinSizeRel RelWithDebInfo)
                string(TOUPPER ${_cfg} _CFG)
                set_property(TARGET demo platform-nes
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
                TITLE       "platform-nes demo"
                VERSION     "1.00"
        )

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
    elseif(TARGET_PLATFORM STREQUAL "nds" OR TARGET_PLATFORM STREQUAL "dsi")
        project(platform-nes CXX)

        FetchContent_MakeAvailable(intsh tuple)

        # The DS backend maps the shared PPU bookkeeping (src/emu) onto libnds 2D
        # hardware: the emulator and all the nametable/OAM/palette write functions
        # are platform-agnostic and compiled here alongside the libnds video/
        # present, the keypad input, and the (stub) audio.
        file(GLOB NDS_SOURCES CONFIGURE_DEPENDS src/nds/*.cpp src/emu/*.cpp src/emu/mappers/*.cpp)
        file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/all/*.cpp)
        add_library(platform-nes STATIC ${NDS_SOURCES} ${ALL_SOURCES})
        target_include_directories(platform-nes PUBLIC
                ${CMAKE_SOURCE_DIR}/include
                ${CMAKE_SOURCE_DIR}/include/umbrella
        )
        # TARGET_NDS is the group the engine headers gate on (native 2D hardware,
        # 32x24 viewport, no SDL headers). The DSi build adds TARGET_DSI on top so
        # the audio path can opt into the larger RAM later.
        target_compile_definitions(platform-nes PUBLIC TARGET_NDS)
        if(TARGET_PLATFORM STREQUAL "dsi")
            target_compile_definitions(platform-nes PUBLIC TARGET_DSI)
        endif()
        # Alternative nametable wiring (see the TARGET_NES branch's own comment,
        # near the top of this file): a fact about which board a build is
        # modeling, not something exclusive to the NES target itself.
        if(DEFINED ALTERNATIVE_NAMETABLE)
            target_compile_definitions(platform-nes PUBLIC "ALTERNATIVE_NAMETABLE=${ALTERNATIVE_NAMETABLE}")
        endif()
        target_link_libraries(platform-nes PUBLIC br0::intsh br0::tuple)

        file(GLOB_RECURSE DEMO_SOURCES CONFIGURE_DEPENDS demo/*.cpp)

        # metatiles.cpp #embeds the demo/chr art; declare it as a dependency so
        # editing the art retriggers a recompile.
        file(GLOB DEMO_CHR_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/demo/chr/*)
        set_source_files_properties(${CMAKE_SOURCE_DIR}/demo/src/graphics/metatiles.cpp
                PROPERTIES OBJECT_DEPENDS "${DEMO_CHR_FILES}")

        add_executable(demo ${DEMO_SOURCES})
        set_target_properties(demo PROPERTIES SUFFIX ".elf")
        target_link_libraries(demo PRIVATE platform-nes nds9)

        # CHR art is pulled in with #embed (C23); keep the Clang-only silencer for
        # parity with the other branches (no-op under devkitARM's GCC).
        target_compile_options(demo PRIVATE $<$<CXX_COMPILER_ID:Clang>:-Wno-c23-extensions>)

        # Enable LTO for release-type configs, mirroring the other branches.
        include(CheckIPOSupported)
        check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
        if(_ipo_ok)
            foreach(_cfg Release MinSizeRel RelWithDebInfo)
                string(TOUPPER ${_cfg} _CFG)
                set_property(TARGET demo platform-nes
                        PROPERTY INTERPROCEDURAL_OPTIMIZATION_${_CFG} ON)
            endforeach()
        else()
            message(STATUS "IPO/LTO not supported by this toolchain: ${_ipo_err}")
        endif()

        # Wrap the linked ELF as a .nds ROM (the default ARM7 core + libnds icon
        # come from calico automatically). nds_create_rom() comes from devkitARM's
        # Platform/NintendoDS.cmake once the NDS toolchain is active. The output is
        # named demo.nds so it matches the other targets' demo.<ext> convention.
        nds_create_rom(demo_nds
                ARM9 demo
                NAME "platform-nes demo"
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
        project(platform-nes CXX)

        FetchContent_MakeAvailable(intsh tuple)

        # The GBA backend maps the shared PPU bookkeeping (src/emu) onto libgba 2D
        # hardware: the emulator and all the nametable/OAM/palette write functions
        # are platform-agnostic and compiled here alongside the libgba video/
        # present, the keypad input, and the (stub) audio.
        file(GLOB GBA_SOURCES CONFIGURE_DEPENDS src/gba/*.cpp src/emu/*.cpp src/emu/mappers/*.cpp)
        file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/all/*.cpp)
        add_library(platform-nes STATIC ${GBA_SOURCES} ${ALL_SOURCES})
        target_include_directories(platform-nes PUBLIC
                ${CMAKE_SOURCE_DIR}/include
                ${CMAKE_SOURCE_DIR}/include/umbrella
        )
        # TARGET_GBA is the group the engine headers gate on (native 2D hardware,
        # 30x20 viewport -- the first target narrower than the NES, no SDL headers).
        target_compile_definitions(platform-nes PUBLIC TARGET_GBA)
        # Alternative nametable wiring (see the TARGET_NES branch's own comment,
        # near the top of this file): a fact about which board a build is
        # modeling, not something exclusive to the NES target itself.
        if(DEFINED ALTERNATIVE_NAMETABLE)
            target_compile_definitions(platform-nes PUBLIC "ALTERNATIVE_NAMETABLE=${ALTERNATIVE_NAMETABLE}")
        endif()
        target_link_libraries(platform-nes PUBLIC br0::intsh br0::tuple)

        file(GLOB_RECURSE DEMO_SOURCES CONFIGURE_DEPENDS demo/*.cpp)

        # metatiles.cpp #embeds the demo/chr art; declare it as a dependency so
        # editing the art retriggers a recompile.
        file(GLOB DEMO_CHR_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/demo/chr/*)
        set_source_files_properties(${CMAKE_SOURCE_DIR}/demo/src/graphics/metatiles.cpp
                PROPERTIES OBJECT_DEPENDS "${DEMO_CHR_FILES}")

        add_executable(demo ${DEMO_SOURCES})
        set_target_properties(demo PROPERTIES SUFFIX ".elf")
        # libgba (-lgba) + gba.specs come in via the platform module's standard
        # libraries/linker flags, so only the engine archive is linked explicitly.
        target_link_libraries(demo PRIVATE platform-nes)

        # CHR art is pulled in with #embed (C23); keep the Clang-only silencer for
        # parity with the other branches (no-op under devkitARM's GCC).
        target_compile_options(demo PRIVATE $<$<CXX_COMPILER_ID:Clang>:-Wno-c23-extensions>)

        # Enable LTO for release-type configs, mirroring the other branches.
        include(CheckIPOSupported)
        check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
        if(_ipo_ok)
            foreach(_cfg Release MinSizeRel RelWithDebInfo)
                string(TOUPPER ${_cfg} _CFG)
                set_property(TARGET demo platform-nes
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
                TITLE "PLATFORMNES"
                GAMECODE "CPNE"
                MAKERCODE "00"
        )

    # ----------------------------------------------------------------------
    # Desktop targets (macOS, Windows, Linux)
    # ----------------------------------------------------------------------
else()
    project(platform-nes CXX)

    FetchContent_MakeAvailable(intsh tuple)

    if(TARGET_PLATFORM STREQUAL "mac")
        set(PLATFORM_DEFINE TARGET_MACOS)
    elseif(TARGET_PLATFORM STREQUAL "win")
        set(PLATFORM_DEFINE TARGET_WINDOWS)
    elseif(TARGET_PLATFORM STREQUAL "linux")
        set(PLATFORM_DEFINE TARGET_LINUX)
    else()
        message(FATAL_ERROR "Set -DTARGET_PLATFORM to one of: nes, win, mac, linux")
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
    add_library(platform-nes STATIC ${SDL3_SOURCES} ${ALL_SOURCES})
    target_include_directories(platform-nes PUBLIC
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/include/umbrella
    )
    target_compile_definitions(platform-nes PUBLIC ${PLATFORM_DEFINE})
    target_link_libraries(platform-nes PUBLIC SDL3::SDL3-static)
    target_link_libraries(platform-nes PUBLIC br0::intsh br0::tuple)

    if(TARGET_PLATFORM STREQUAL "linux")
        target_link_libraries(platform-nes PUBLIC PkgConfig::LibUSB)
    endif()

    file(GLOB_RECURSE DEMO_SOURCES CONFIGURE_DEPENDS demo/*.cpp)

    # metatiles.cpp includes chr.hpp, which #embeds the demo/chr art files and
    # (being the includer) emits the folded CHR ROM image. Declare the art as an
    # explicit dependency so editing it retriggers a recompile.
    file(GLOB DEMO_CHR_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/demo/chr/*)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/demo/src/graphics/metatiles.cpp
            PROPERTIES OBJECT_DEPENDS "${DEMO_CHR_FILES}")

    add_executable(demo ${DEMO_SOURCES})
    target_link_libraries(demo PRIVATE platform-nes)

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
            set_property(TARGET demo platform-nes
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
    # ------------------------------------------------------------------
    if(CMAKE_STRIP AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_custom_command(TARGET demo POST_BUILD
                COMMAND ${CMAKE_STRIP} $<TARGET_FILE:demo>
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
            set(_APPDIR_LICENSES "${APPDIR}/usr/share/doc/platform-nes")

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
                    COMMAND ${LINUXDEPLOY}
                            --appdir "${APPDIR}"
                            --executable "${APPDIR}/usr/bin/demo"
                            --desktop-file "${CMAKE_BINARY_DIR}/demo.desktop"
                            --icon-file "${CMAKE_SOURCE_DIR}/demo/demo.png"
                            # Force-bundle libasound: AppImage's default excludelist skips it
                            # on the assumption every Linux desktop has ALSA, but minimal
                            # images (e.g., WSL's default Ubuntu) don't.
                            --library /usr/lib/x86_64-linux-gnu/libasound.so.2
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
# has created the platform-nes target by this point): 0 for NTSC (every
# target's default) or 1 for the nes_pal target set above.
target_compile_definitions(platform-nes PUBLIC REGION=${REGION})