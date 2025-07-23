message("Building on/for macOS")

set(CMAKE_MACOSX_RPATH ON)

set(PLATFORM_SHIM_PATH "${CMAKE_CURRENT_LIST_DIR}/../src/platform/posix" )

set (EXTRA_LINKER_LIBS "${EXTRA_LINKER_LIBS}" pthread)

# message("EXTRA_LINKER_LIBS = ${EXTRA_LINKER_LIBS}")

set(POSIX True)
