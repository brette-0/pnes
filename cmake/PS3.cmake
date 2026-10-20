# CMAKE_TOOLCHAIN_FILE for TARGET_PLATFORM=ps3.
#
# Same situation as cmake/PS2.cmake: unlike devkitPro (3DS/GameCube/Wii/Switch/
# Wii U/DS/DSi/GBA) and pspdev (PSP), ps3dev ships no CMake toolchain file of
# its own -- PSL1GHT provides Makefile rules (ppu_rules/base_rules) only -- so
# this project supplies one. docker/Dockerfile.ps3 builds ps3dev from source and
# exports PS3DEV/PSL1GHT; the ps3-* presets assume those are already set in the
# shell/container cmake runs in, and gate themselves on PS3DEV.
#
# The split that makes this file longer than PS2.cmake: Clang compiles, ppu-g++
# links.
#
# Clang compiles because ps3toolchain's PPU GCC is 13.2.0, which has no #embed
# (GCC 15+), and demo/src/graphics/graphics.hpp needs it. Clang's powerpc64
# backend emits exactly what the PPU wants with no coaxing -- verified that
# `clang++ --target=powerpc64-unknown-elf` produces "ELF 64-bit MSB
# relocatable, 64-bit PowerPC, Power ELF V1 ABI" objects -- but Clang knows
# nothing about where ps3dev put newlib, libstdc++ or libgcc, so those are
# pointed at explicitly below.
#
# ppu-g++ links because the GCC driver is what owns the PS3 link: crt objects,
# its spec file, the default library set and the newlib/libgcc search paths.
# Reproducing all of that as raw ld flags would mean re-deriving ps3dev's specs
# by hand and keeping them in sync; handing the objects to ppu-g++ instead
# keeps that knowledge where ps3dev already maintains it. Clang-only compile
# flags (--target=, -std=gnu++23, ...) are therefore kept out of the link
# command -- see CMAKE_CXX_LINK_EXECUTABLE at the bottom.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR ppc64)

if(NOT DEFINED ENV{PS3DEV})
    message(FATAL_ERROR
        "PS3 build requires the PS3DEV env var (ps3dev toolchain root, e.g. "
        "/usr/local/ps3dev inside the pnes-ps3 Docker image -- see "
        "docker/Dockerfile.ps3 and the ps3-* services in compose.yml).")
endif()

set(PS3DEV "$ENV{PS3DEV}")
set(PS3_PPU_ROOT   "${PS3DEV}/ppu")
set(PS3_PPU_TRIPLE powerpc64-ps3-elf)
set(PS3_PPU_SYSROOT "${PS3_PPU_ROOT}/${PS3_PPU_TRIPLE}")
set(PS3_PPU_GXX    "${PS3_PPU_ROOT}/bin/${PS3_PPU_TRIPLE}-g++")

if(NOT EXISTS "${PS3_PPU_GXX}")
    message(FATAL_ERROR
        "PS3 build expected the ps3dev PPU driver at ${PS3_PPU_GXX} but it is "
        "not there. PS3DEV is set to '${PS3DEV}' -- is that really a ps3dev "
        "install built by ps3toolchain (see docker/Dockerfile.ps3)?")
endif()

# The libstdc++ headers and libgcc live in version-stamped directories, so the
# GCC version is discovered rather than hardcoded: ps3toolchain's PPU GCC is
# 13.2.0 today, and pinning that here would turn a toolchain bump into a
# confusing "file not found" much further along.
file(GLOB PS3_GCC_LIBDIRS "${PS3_PPU_ROOT}/lib/gcc/${PS3_PPU_TRIPLE}/*")
list(FILTER PS3_GCC_LIBDIRS EXCLUDE REGEX "\\.(a|o|so|la)$")
if(NOT PS3_GCC_LIBDIRS)
    message(FATAL_ERROR
        "PS3 build could not find a libgcc directory under "
        "${PS3_PPU_ROOT}/lib/gcc/${PS3_PPU_TRIPLE}/ -- the ps3dev install at "
        "'${PS3DEV}' looks incomplete.")
endif()
# Newest first, so a toolchain carrying two GCC versions picks the newer.
list(SORT PS3_GCC_LIBDIRS COMPARE NATURAL ORDER DESCENDING)
list(GET PS3_GCC_LIBDIRS 0 PS3_GCC_LIBDIR)
get_filename_component(PS3_GCC_VERSION "${PS3_GCC_LIBDIR}" NAME)

set(CMAKE_C_COMPILER   clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_ASM_COMPILER clang)

# powerpc64-unknown-elf, not -ps3-elf: Clang has no PS3 target of its own, and
# this is the triple that gets the right machine out of it (64-bit PowerPC,
# big-endian, ELF v1 ABI -- see the header comment). ps3dev's own triple only
# has to match on the ppu-g++ link side, where it does by construction.
set(PS3_CLANG_TARGET powerpc64-unknown-elf)
set(CMAKE_C_COMPILER_TARGET   ${PS3_CLANG_TARGET})
set(CMAKE_CXX_COMPILER_TARGET ${PS3_CLANG_TARGET})
set(CMAKE_ASM_COMPILER_TARGET ${PS3_CLANG_TARGET})

# Every include directory is named explicitly, and Clang's own system-include
# search is switched off with -nostdlibinc/-nostdinc++.
#
# The tempting shorter version is --gcc-toolchain=${PS3_PPU_ROOT} and letting
# Clang discover the GCC installation itself. That is not reliable here: Clang
# finds a GCC install by trying its own target triple against the candidate
# directory names, and this build's Clang triple (powerpc64-unknown-elf -- see
# below) is NOT the triple ps3dev installs under (powerpc64-ps3-elf). A missed
# detection does not fail loudly either; Clang falls back to whatever host GCC
# it can find and pulls in the host's glibc headers, which was observed while
# bringing this up (host bits/floatn.h erroring on __float128, and int64_t
# redefinitions between host glibc and Clang's own stdint.h). Spelling the
# paths out is what was actually verified to produce clean PowerPC objects.
#
# The four directories, in search order:
#   <sysroot>/include/c++/<ver>          libstdc++ headers
#   <sysroot>/include/c++/<ver>/<triple> its arch-specific bits/c++config.h
#   <sysroot>/include                    newlib
#   <ppu>/include                        PSL1GHT (sys/, rsx/, lv2/, io/, audio/)
#
# PSL1GHT's is separate from the sysroot for the same reason ps2sdk/gsKit are
# in the PS2 branch: it is installed outside the compiler's own prefix. All are
# -isystem rather than -I so third-party headers don't inherit this project's
# warning flags.
#
# -nostdlibinc still leaves Clang's own resource-dir builtins (stddef.h,
# stdint.h, ...) available, which is what newlib's headers expect to find.
#
# -ffreestanding is deliberately NOT set: newlib is a real hosted-ish libc here
# and the engine's shared code includes <cstdio>/<cstring>.
#
# -fno-use-init-array is as ABI-critical as -mlong-double-64 below, and fails
# far more quietly. C++ globals needing a runtime constructor are registered in
# a table the startup code walks, and the two compilers disagree about which
# table that is: ps3dev's GCC emits .ctors, Clang emits .init_array. PSL1GHT's
# crt0 only ever walks .ctors --
#
#     .init:  bl .frame_dummy ; bl .__do_global_ctors_aux ; blr
#
# and nothing in a PSL1GHT link references __libc_init_array or
# __init_array_start at all, so .init_array is read by nobody. Without this
# flag every Clang-compiled dynamic initialiser is silently dropped: the global
# keeps its zero-initialised bytes, nothing warns at compile or link time, and
# the first use faults somewhere unrelated to the constructor that never ran.
# With it, Clang emits .ctors carrying the same R_PPC64_ADDR64-against-.opd
# relocation GCC does, so the two halves of this link agree again.
file(GLOB PS3_CXX_INCDIRS "${PS3_PPU_SYSROOT}/include/c++/*")
list(FILTER PS3_CXX_INCDIRS EXCLUDE REGEX "\\.(h|hpp|tcc)$")
if(NOT PS3_CXX_INCDIRS)
    message(FATAL_ERROR
        "PS3 build could not find libstdc++ headers under "
        "${PS3_PPU_SYSROOT}/include/c++/ -- the ps3dev install at '${PS3DEV}' "
        "looks incomplete (was ps3toolchain step 002 run?).")
endif()
list(SORT PS3_CXX_INCDIRS COMPARE NATURAL ORDER DESCENDING)
list(GET PS3_CXX_INCDIRS 0 PS3_CXX_INCDIR)

# -mlong-double-64 is ABI-critical, not a tuning choice. ps3dev's GCC makes
# `long double` a plain 64-bit double on this target (__LDBL_MANT_DIG__ 53,
# sizeof 8) and every object in newlib's libc.a/libm.a and in libstdc++ is
# built that way. Clang's own default for powerpc64 is IBM double-double
# (__LDBL_MANT_DIG__ 106, sizeof 16), so without this flag the two halves
# disagree and ld refuses the link outright, once per libc/libm member it
# touches: "uses 64-bit long double, <obj> uses 128-bit long double / failed to
# merge target specific data". It is not a silent mismatch, but it only shows
# up at link time, long after every TU has compiled clean.
set(PS3_CLANG_FLAGS
    "--sysroot=${PS3_PPU_SYSROOT} -nostdlibinc -nostdinc++ -mlong-double-64 \
     -fno-use-init-array \
     -isystem ${PS3_CXX_INCDIR} \
     -isystem ${PS3_CXX_INCDIR}/${PS3_PPU_TRIPLE} \
     -isystem ${PS3_PPU_SYSROOT}/include \
     -isystem ${PS3_PPU_ROOT}/include")
string(REGEX REPLACE "[ \t\r\n]+" " " PS3_CLANG_FLAGS "${PS3_CLANG_FLAGS}")
set(CMAKE_C_FLAGS_INIT   "${PS3_CLANG_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${PS3_CLANG_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${PS3_CLANG_FLAGS}")

# Same reason as the PS2 toolchain file: a full executable link needs the
# PSL1GHT -L/-l set (added in the ps3 branch of CMakeLists.txt) which isn't
# available yet during CMake's own compiler-detection probes, so ask it to only
# prove it can produce a .a. Doubly necessary here, since the probe would
# otherwise run the clang->ppu-g++ handoff below before any of it is set up.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Hand the link to ppu-g++ (see the header comment for why). <FLAGS> is
# deliberately absent: that placeholder expands to the C++ *compile* flags,
# which here are Clang's (--target=powerpc64-unknown-elf, --gcc-toolchain=,
# -std=gnu++23) and would all be rejected by the GCC driver. <CMAKE_CXX_LINK_
# FLAGS> and <LINK_FLAGS> are the link-time ones and are kept.
set(CMAKE_CXX_LINK_EXECUTABLE
    "${PS3_PPU_GXX} <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_C_LINK_EXECUTABLE
    "${PS3_PPU_ROOT}/bin/${PS3_PPU_TRIPLE}-gcc <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

# Static libraries are archived with ps3dev's own ar/ranlib rather than the
# host's: the objects are PPC64 ELF v1, and host binutils on an x86-64 box has
# no reason to be able to index them.
set(CMAKE_AR     "${PS3_PPU_ROOT}/bin/${PS3_PPU_TRIPLE}-ar")
set(CMAKE_RANLIB "${PS3_PPU_ROOT}/bin/${PS3_PPU_TRIPLE}-ranlib")

set(CMAKE_FIND_ROOT_PATH "${PS3DEV}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
