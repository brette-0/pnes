cmake_minimum_required(VERSION 4.3)
project(blobman)

set(CMAKE_CXX_STANDARD 20)

find_package(tomlplusplus CONFIG REQUIRED)

add_executable(blobman main.cpp)
target_link_libraries(blobman PRIVATE tomlplusplus::tomlplusplus)
