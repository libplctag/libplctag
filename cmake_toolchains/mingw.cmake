# get defaults for GCC
include("${CMAKE_SOURCE_DIR}/cmake_toolchains/clang_or_gcc.cmake")

# pretend to be a POSIX platform
set(PLATFORM_SHIM_PATH "${CMAKE_SOURCE_DIR}/src/platform/posix" )
