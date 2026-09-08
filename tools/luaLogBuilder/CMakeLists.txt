# lua_log_builder -- standalone, host-native CMake project. Deliberately NOT
# part of the top-level CMakeLists.txt's build: that project's NES presets
# set CMAKE_C_COMPILER/CMAKE_CXX_COMPILER to the llvm-mos 6502 cross-compiler
# (see CMakePresets.json's nes-release), and this tool has to run ON the
# machine building the ROM, reading the ELF that build just produced -- it
# must be compiled with the host's own native compiler, never the 6502 one.
#
# Build it on its own, once, with your system's default compiler:
#   cmake -S tools/luaLogBuilder -B tools/luaLogBuilder/build
#   cmake --build tools/luaLogBuilder/build
#
# Only tested on Linux so far.

cmake_minimum_required(VERSION 3.18)
project(lua_log_builder CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(lua_log_builder main.cpp)
