message("Building on/for macOS")

set(CMAKE_MACOSX_RPATH ON)

set(PLATFORM_SHIM_PATH "${CMAKE_CURRENT_LIST_DIR}/../src/platform/posix" )

set (EXTRA_LINKER_LIBS "${EXTRA_LINKER_LIBS}" pthread)

# set(STATIC_C_LINKER_OPTIONS "-static-libgcc")
# set(STATIC_CXX_LINKER_OPTIONS "-static-libstdc++")

message("EXTRA_LINKER_LIBS = ${EXTRA_LINKER_LIBS}")

set(POSIX True)
