
set(CMAKE_MACOSX_RPATH ON)

set(PLATFORM_SHIM_PATH "${CMAKE_SOURCE_DIR}/src/platform/posix" )

set (EXTRA_LINKER_LIBS "${EXTRA_LINKER_LIBS}" pthread)

set(POSIX True)
