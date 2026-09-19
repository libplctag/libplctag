message("Building on/for macOS")

set(CMAKE_MACOSX_RPATH ON)

set (EXTRA_LINKER_LIBS "${EXTRA_LINKER_LIBS}" pthread)

# message("EXTRA_LINKER_LIBS = ${EXTRA_LINKER_LIBS}")

set(POSIX True)
