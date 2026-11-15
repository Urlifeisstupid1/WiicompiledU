# Wii U port bring-up projects (no Nintendo code; safe to build and share).
# Build:
#   cmake -B build -S wiiu -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/WiiU.cmake
#   cmake --build build
cmake_minimum_required(VERSION 3.13)
project(wiicompiled_wiiu_bringup C CXX)

set(CMAKE_CXX_STANDARD 17)

add_subdirectory(hello)
add_subdirectory(tests/stdtest)
add_subdirectory(tests/bigtext)
add_subdirectory(tests/codegen)
add_subdirectory(tests/overlay)
