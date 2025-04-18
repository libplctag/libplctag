set(MINGW True)

set(EXTRA_COMPILE_FLAGS_MINSIZEREL "${EXTRA_COMPILE_FLAGS_MINSIZEREL} -DMINGW=1")
set(EXTRA_COMPILE_FLAGS_DEBUG "${EXTRA_COMPILE_FLAGS_DEBUG} -DMINGW=1")


# get defaults for GCC
include("${CMAKE_SOURCE_DIR}/cmake_toolchains/clang_or_gcc.cmake")

# set up config for MinGW
# set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER gcc)
set(CMAKE_CXX_COMPILER g++)
set(CMAKE_MAKE_PROGRAM mingw32-make)
