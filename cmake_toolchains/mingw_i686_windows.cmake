# CMake toolchain file for 32-bit MinGW (i686-w64-mingw32) on Windows.
# Uses the MSYS2 mingw32 environment for the compiler and 32-bit system libraries.
#
# Usage (CI):
#   cmake .. -G "MinGW Makefiles" \
#            -DCMAKE_TOOLCHAIN_FILE=cmake_toolchains/mingw_i686_windows.cmake \
#            -DCMAKE_BUILD_TYPE=MinSizeRel
#
# Prerequisites: install the MSYS2 i686 toolchain before configuring:
#   C:\msys64\usr\bin\pacman.exe -S --noconfirm --needed mingw-w64-i686-gcc

set(CMAKE_SYSTEM_NAME    Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(CMAKE_C_COMPILER C:/msys64/mingw32/bin/gcc.exe)

# Tell CMake where to find 32-bit headers and libraries.
set(CMAKE_FIND_ROOT_PATH C:/msys64/mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Signal the rest of the build system that this is a 32-bit build so that
# clang_or_gcc.cmake adds -m32 and src/poc/CMakeLists.txt selects the
# i386-pc-windows-gnu triple for libyafl.
set(BUILD_32_BIT ON CACHE BOOL "Build 32-bit code")
