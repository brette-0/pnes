cmake_minimum_required(VERSION 3.21)

# 'pnes' Animation Designer -- standalone desktop app, built independently of
# the pnes engine's own CMakeLists.txt (no add_subdirectory from the root
# build, no shared cache variables), exactly like tools/uitk. Configure/build
# this directory on its own:
#   cmake -B build -S tools/animator
#   cmake --build build
#
# Two targets: `animator`, the designer, and `animator-export`, the same export
# with no GUI and no Qt (see exporter.hpp). The root build does drive this
# directory for that second one -- a nested `cmake` of its own at configure
# time, with ANIMATOR_GUI=OFF, to regenerate the committed animation headers
# (see the animator-export step in the root CMakeLists.txt) -- which is a
# subprocess, not a subdirectory: nothing of this file is shared with that
# build, and it still configures against the host compiler even when the
# project that invoked it is cross-compiling.
#
# It targets whichever of Windows/macOS/Linux it's configured on -- there is
# no cross-compiling story here (unlike the root project's NES target), so
# CMake's default host toolchain is used as-is.
project(animator CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

# Widgets is used via QMainWindow; MOC is needed even for this stub because
# any QObject-derived class (QApplication/QMainWindow included) pulls Qt's
# metaobject machinery into the build.
set(CMAKE_AUTOMOC ON)

# ON demands Qt and fails without it; OFF builds the headless exporter alone;
# AUTO (the default) builds the GUI when Qt is there and says so when it is
# not. AUTO is what lets one nested `cmake -S tools/animator` serve both a
# developer's machine and a cross-build Docker image that has no Qt at all --
# the root CMakeLists.txt's animator-export step configures this directory on
# every build, and only ever needs the exporter target out of it.
set(ANIMATOR_GUI "AUTO" CACHE STRING "Build the designer GUI: ON, OFF or AUTO")
set_property(CACHE ANIMATOR_GUI PROPERTY STRINGS ON OFF AUTO)

# Prefer Qt6, fall back to Qt5 -- both are dynamically linked (Qt6Widgets /
# Qt5Widgets shared libs), same "link the system's copy" approach uitk and the
# root project (SDL3 on Linux) take. Qt is also what picks the platform
# backend: xcb/wayland on Linux, Direct3D on Windows, Metal on macOS -- none of
# that is spelled out here.
if(ANIMATOR_GUI)
    if(ANIMATOR_GUI STREQUAL "AUTO")
        find_package(QT NAMES Qt6 Qt5 QUIET COMPONENTS Widgets)
    else()
        find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
    endif()
    if(QT_FOUND)
        find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)
    endif()
endif()

# toml++ reads blobman's manifests (chr_table.cpp). Same two-source dance
# tools/blobman uses: prefer a system package (fast, and keeps a local build
# offline), fall back to fetching the release, because GitHub's runners have no
# toml++ package and a REQUIRED find_package would make this unbuildable there.
find_package(tomlplusplus CONFIG QUIET)

if(NOT tomlplusplus_FOUND)
    message(STATUS "animator: no system toml++, fetching it instead")
    include(FetchContent)
    FetchContent_Declare(tomlplusplus
            GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
            GIT_TAG v3.4.0
            GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(tomlplusplus)
endif()

# The export pipeline, which is deliberately Qt-free end to end: the manifest
# reader, the project format, the code generator and the headless driver over
# them. Kept as a list because both targets below are built from it -- the GUI
# so that File > Export All and `animator-export` cannot diverge, the exporter
# so it needs no Qt.
set(ANIMATOR_CORE_SOURCES
        chr_table.cpp
        codegen.cpp
        exporter.cpp
        palette.cpp
        project.cpp
)

# animator-export -- the designer's export with no designer attached: no Qt, no
# window, no display server, just toml++. Always built, including on a host
# with no Qt, because this is the target the root build's configure-time
# animator-export step runs to regenerate demo/animations/gen/ (see the root
# CMakeLists.txt), and most of the cross-build images have no Qt in them.
add_executable(animator-export
        exporter_main.cpp
        ${ANIMATOR_CORE_SOURCES}
)
target_link_libraries(animator-export PRIVATE tomlplusplus::tomlplusplus)
# AUTOMOC is on for the GUI's sake (above); this target has no QObject in it,
# so running moc over it would be pure build time -- and it must keep compiling
# with no Qt installed at all, which is exactly what moc would need.
set_target_properties(animator-export PROPERTIES AUTOMOC OFF)

if(QT_FOUND)
    add_executable(animator
            main.cpp
            animation_preview.cpp
            palette_store.cpp
            palette_window.cpp
            metasprite_field.cpp
            metasprite_frame.cpp
            metasprite_page.cpp
            tile_canvas.cpp
            tile_image.cpp
            ${ANIMATOR_CORE_SOURCES}
    )
    target_link_libraries(animator PRIVATE
            Qt${QT_VERSION_MAJOR}::Widgets
            tomlplusplus::tomlplusplus
    )

    if(WIN32)
        set_target_properties(animator PROPERTIES WIN32_EXECUTABLE TRUE)
    elseif(APPLE)
        set_target_properties(animator PROPERTIES
                MACOSX_BUNDLE TRUE
                INSTALL_RPATH "@executable_path")
    endif()
elseif(ANIMATOR_GUI STREQUAL "AUTO")
    # STATUS, not WARNING: a build that only wants the exporter (every
    # configure the root project makes of this directory) is the common case,
    # and a warning there would be permanent noise. Still said out loud, so a
    # developer who expected the designer knows why they did not get it.
    message(STATUS
            "animator: no Qt Widgets found -- building animator-export only. "
            "Install Qt6 (or Qt5) for the designer GUI itself, or pass "
            "-DANIMATOR_GUI=ON to make a missing Qt an error instead.")
endif()
