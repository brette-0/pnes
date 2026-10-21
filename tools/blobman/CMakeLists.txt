# blobman -- packs demo/chr/assets/**.chr into the cartridge's single CHR ROM
# image (demo/chr/chr.bin) from the layout in demo/chr/config.toml, and writes
# the symbolic tile ids beside it (demo/chr/chr.h).
#
# Both outputs are COMMITTED, and the main build also repacks them: the root
# CMakeLists.txt configures and builds this directory through a nested `cmake`
# of its own, then runs the tool, before anything compiles against the pair
# (search that file for "blobman --"). demo/chr/config.toml and the art under
# demo/chr/assets/ are CMAKE_CONFIGURE_DEPENDS there, so editing either
# repacks on the next build. CI's blobman-check job is what catches the
# COMMITTED pair drifting away from the manifest or the art -- the local
# repack updates the working tree, it cannot commit for you.
#
# 3.18 -- the lowest minimum anything in this project declares (see
# tools/luaLogBuilder and tools/measure_sections.cmake). Nothing here uses a
# feature newer than that.
cmake_minimum_required(VERSION 3.18)
project(blobman CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# toml++ is header-only, so either source of it works identically.
#
# Prefer a system package when there is one -- that is the fast path on a
# developer machine, and it keeps a local build offline. It cannot be REQUIRED,
# though: GitHub's runners have no toml++ package, so demanding one would make
# blobman unbuildable in CI. Fall back to fetching the release, the same way
# the top-level project already pulls intsh/tuple/consh.
find_package(tomlplusplus CONFIG QUIET)

if (NOT tomlplusplus_FOUND)
    message(STATUS "blobman: no system toml++, fetching it instead")
    include(FetchContent)
    FetchContent_Declare(tomlplusplus
            GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
            GIT_TAG v3.4.0
            GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(tomlplusplus)
endif()

add_executable(blobman main.cpp)
target_link_libraries(blobman PRIVATE tomlplusplus::tomlplusplus)
