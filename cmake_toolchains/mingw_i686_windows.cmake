# CMake toolchain file for 32-bit MinGW (i686-w64-mingw32) on Windows.
# Uses the MSYS2 mingw32 environment for the compiler and 32-bit system libraries.
#
# Usage (CI):
#   cmake .. -G "MinGW Makefiles" \
#            -DCMAKE_TOOLCHAIN_FILE=cmake_toolchains/mingw_i686_windows.cmake \
#            -DCMAKE_BUILD_TYPE=MinSizeRel
#
# Prerequisites: install the MSYS2 i686 toolchain before configuring.
# When run under an MSYS2 MINGW32 shell, gcc/g++ and mingw32-make are on PATH.

set(CMAKE_SYSTEM_NAME    Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(CMAKE_C_COMPILER gcc)
set(CMAKE_CXX_COMPILER g++)

# Tell CMake where to find 32-bit headers and libraries.
if(DEFINED ENV{MINGW_PREFIX})
	set(CMAKE_FIND_ROOT_PATH $ENV{MINGW_PREFIX})
else()
	set(CMAKE_FIND_ROOT_PATH /mingw32)
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Signal the rest of the build system that this is a 32-bit build so that
# clang_or_gcc.cmake adds -m32 and src/poc/CMakeLists.txt selects the
# i386-pc-windows-gnu triple for libyafl.
set(BUILD_32_BIT ON CACHE BOOL "Build 32-bit code")
