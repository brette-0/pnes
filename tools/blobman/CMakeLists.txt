# blobman -- host-native CHR ROM packer. Built and run as a REQUIRED step of
# every pnes target's build (see the blobman block in the top-level
# CMakeLists.txt), as its own project through a nested cmake invocation, so it
# is never built with whatever console cross-compiler the outer configure
# selected.
#
# 3.21 to match the top-level project's own minimum rather than exceed it:
# this is configured by a nested `cmake` from inside that build, so a minimum
# higher than the outer project's would make blobman the thing that fails on an
# older CMake, for no feature it actually uses.
cmake_minimum_required(VERSION 3.21)
project(blobman CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# toml++ is header-only, so either source of it works identically.
#
# Prefer a system package when there is one -- that's the fast path on a
# developer machine, and it keeps a local build offline. It cannot be REQUIRED,
# though: none of the docker/ images package toml++, and neither do GitHub's
# Windows or macOS runners, so demanding it would make blobman unbuildable in
# CI on every platform. Fall back to fetching the release, the same way the
# top-level project already pulls intsh/tuple.
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
