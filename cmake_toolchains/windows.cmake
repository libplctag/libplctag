
set(PLATFORM_WINDOWS True)

set(PLATFORM_SHIM_PATH "${CMAKE_CURRENT_LIST_DIR}/../src/platform/windows" )

set (EXTRA_LINKER_LIBS "${EXTRA_LINKER_LIBS}" ws2_32 bcrypt)
