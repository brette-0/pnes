# 3.18 -- the lowest minimum anything in this project declares (see
# tools/luaLogBuilder and tools/measure_sections.cmake). Nothing here uses a
# feature newer than that, and a tool that may be configured by a nested cmake
# from inside the main build should never be the thing that demands a newer
# CMake than the build itself.
cmake_minimum_required(VERSION 3.18)
project(blobman)

set(CMAKE_CXX_STANDARD 20)

find_package(tomlplusplus CONFIG REQUIRED)

add_executable(blobman main.cpp)
target_link_libraries(blobman PRIVATE tomlplusplus::tomlplusplus)
