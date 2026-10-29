cmake_minimum_required(VERSION 3.21)

# 'pnes' Animation Designer -- standalone desktop app, built independently of
# the pnes engine's own CMakeLists.txt (no add_subdirectory from the root
# build, no shared cache variables), exactly like tools/uitk. Configure/build
# this directory on its own:
#   cmake -B build -S tools/animator
#   cmake --build build
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

# Prefer Qt6, fall back to Qt5 -- both are dynamically linked (Qt6Widgets /
# Qt5Widgets shared libs), same "link the system's copy" approach uitk and the
# root project (SDL3 on Linux) take. Qt is also what picks the platform
# backend: xcb/wayland on Linux, Direct3D on Windows, Metal on macOS -- none of
# that is spelled out here.
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

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

add_executable(animator
        main.cpp
        animation_preview.cpp
        chr_table.cpp
        palette.cpp
        palette_store.cpp
        palette_window.cpp
        metasprite_field.cpp
        metasprite_frame.cpp
        metasprite_page.cpp
        project.cpp
        tile_canvas.cpp
        tile_image.cpp
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
