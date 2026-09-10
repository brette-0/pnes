cmake_minimum_required(VERSION 3.21)

# 'platform NES' UI Toolkit -- standalone desktop app, built independently of
# the platform-nes engine's own CMakeLists.txt (no add_subdirectory from the
# root build, no shared cache variables). Configure/build this directory on
# its own:
#   cmake -B build -S tools/uitk
#   cmake --build build
# It targets whichever of Windows/macOS/Linux it's configured on -- there is
# no cross-compiling story here (unlike the root project's NES target), so
# CMake's default host toolchain is used as-is.
project(uitk CXX)

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
# Qt5Widgets shared libs), same "link the system's copy" approach the root
# project takes with SDL3 on Linux.
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

add_executable(uitk main.cpp)
target_link_libraries(uitk PRIVATE Qt${QT_VERSION_MAJOR}::Widgets)

if(WIN32)
    set_target_properties(uitk PROPERTIES WIN32_EXECUTABLE TRUE)
elseif(APPLE)
    set_target_properties(uitk PROPERTIES
            MACOSX_BUNDLE TRUE
            INSTALL_RPATH "@executable_path")
endif()
