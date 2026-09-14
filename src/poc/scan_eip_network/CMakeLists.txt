message("Building scan_eip_network POC.")

set(SCAN_EIP_NETWORK_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/scan_eip_network.c
)

add_executable(scan_eip_network ${SCAN_EIP_NETWORK_FILES}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/utils/socket.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/utils/socket.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/utils/buf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/utils/buf.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/utils/args.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/utils/args.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/utils/log.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/utils/log.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/utils/utils.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/utils/utils.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/utils/coro_net.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/utils/coro_net.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/utils/err.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/utils/err.h
)

# Link base libraries
target_link_libraries(scan_eip_network PUBLIC ${EXTRA_LINKER_LIBS})
target_link_options(scan_eip_network PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})

# Handle Windows-specific libraries from pragmas
# The pragmas in scan_eip_network.c reference:
#   ws2_32.lib (Winsock2) - already in EXTRA_LINKER_LIBS on Windows
#   iphlpapi.lib (IP Helper API) - needs to be added explicitly for non-MSVC compilers
if(WIN32)
    # MSVC processes pragma comments automatically, but MinGW and other compilers need explicit linking
    if(NOT MSVC)
        target_link_libraries(scan_eip_network PUBLIC iphlpapi)
    endif()
endif()
