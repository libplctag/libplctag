# CMake toolchain file for cross-compiling the 64-bit Windows build from a POSIX
# host (macOS or Linux) with MinGW-w64.
#
# This is a compile check, not a test runner: the binaries it produces do not run
# on the host.  Its job is to put a Windows compiler over the ~1,100 lines of
# #ifdef _WIN32 code that the POSIX build never sees -- see 3.1 in
# docs/deferred_fixes.md for the inventory.
#
# Prerequisites:
#   macOS:  brew install mingw-w64
#   Debian: apt install mingw-w64
#
# Usage:
#   cmake -S . -B build_mingw \
#         -DCMAKE_TOOLCHAIN_FILE=cmake_toolchains/mingw_x86_64_cross.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build_mingw -j8
#
# Use Release or MinSizeRel.  The Debug build turns on ASan/UBSan, and MinGW-w64
# ships no sanitizer runtime.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

# Look for target libraries and headers in the cross toolchain, but keep using
# host programs (make, ninja) to drive the build.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
