message("Building for/on generic UNIX/POSIX platform.")


set (EXTRA_LINKER_LIBS "${EXTRA_LINKER_LIBS}" pthread)

# message("EXTRA_LINKER_LIBS = ${EXTRA_LINKER_LIBS}")

set(POSIX True)
