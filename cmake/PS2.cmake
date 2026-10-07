# CMAKE_TOOLCHAIN_FILE for TARGET_PLATFORM=ps2.
#
# Unlike devkitPro (3DS/GameCube/Wii/Switch/Wii U/DS/DSi/GBA) and pspdev
# (PSP), ps2dev does not ship its own CMake toolchain file, so this project
# supplies one. The ps2dev/ps2dev Docker image (see docker/Dockerfile.ps2)
# exports PS2DEV/PS2SDK/GSKIT and puts the cross-compiler on PATH; this file
# only needs those three env vars, which the ps2-*/presets below assume are
# already set in the shell/container cmake is invoked from.
#
# mips64r5900el-ps2-elf-gcc/g++ has a built-in sysroot/spec file that already
# handles crt0/linking/EE flags (-march=r5900 -mhard-float -msingle-float
# etc.) with no toolchain-side flags needed -- confirmed by linking a bare
# `int main(){ return 0; }` and getting back a real MIPS N32 ELF. All that's
# missing from the default search path is ps2sdk's own headers/libs
# ($PS2SDK/ee) and gsKit ($GSKIT), which are NOT under the compiler's sysroot
# and are added as ordinary include/link dirs in the ps2 branch of the
# top-level CMakeLists.txt instead of here.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR mips)

if(NOT DEFINED ENV{PS2DEV})
    message(FATAL_ERROR
        "PS2 build requires the PS2DEV env var (ps2dev toolchain root, e.g. "
        "/usr/local/ps2dev inside the platform-nes-ps2 Docker image -- see "
        "docker/Dockerfile.ps2 and the ps2-* services in compose.yml).")
endif()

set(PS2DEV "$ENV{PS2DEV}")
set(CMAKE_C_COMPILER   "${PS2DEV}/ee/bin/mips64r5900el-ps2-elf-gcc")
set(CMAKE_CXX_COMPILER "${PS2DEV}/ee/bin/mips64r5900el-ps2-elf-g++")
set(CMAKE_ASM_COMPILER "${PS2DEV}/ee/bin/mips64r5900el-ps2-elf-gcc")

# A full executable link needs ps2sdk/gsKit's own -I/-L (set in the ps2
# branch of CMakeLists.txt, since they live outside this compiler's sysroot)
# which aren't available yet during CMake's own compiler-detection probes;
# ask it to only prove it can produce a .a instead of a full link.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH "${PS2DEV}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
