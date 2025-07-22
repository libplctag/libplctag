message("Building for/on generic UNIX/POSIX platform.")


set(PLATFORM_SHIM_PATH "${CMAKE_CURRENT_LIST_DIR}/../src/platform/posix" )

set (EXTRA_LINKER_LIBS "${EXTRA_LINKER_LIBS}" pthread)

# message("EXTRA_LINKER_LIBS = ${EXTRA_LINKER_LIBS}")

set(POSIX True)
