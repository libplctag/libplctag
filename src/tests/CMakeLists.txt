message("Building test utilities.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

# message("CMAKE_C_FLAGS = ${CMAKE_C_FLAGS}")


# Define the source files for ab_server
set(AB_SERVER_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/cip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/cip.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/compat.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/cpf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/cpf.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/eip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/eip.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/err.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/err.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/fairness.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/fairness.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/memory.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/memory.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/mutex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/mutex.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/pccc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/pccc.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/plc.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/slice.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/socket.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/socket.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/tcp_server.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/tcp_server.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/thread.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/thread.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/utils.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/utils.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/log.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/log.h
)

# Add ab_server executable
add_executable(ab_server ${AB_SERVER_FILES})
target_link_libraries(ab_server PUBLIC ${EXTRA_LINKER_LIBS})

# Link math library for ab_server (fairness.c uses math.h)
# Math functions are in libm on Unix but in the C runtime on Windows
if(UNIX)
    target_link_libraries(ab_server PUBLIC m)
endif()

# if(STATIC_LINK_OPTIONS)
#     set(EXTRA_LINK_OPTIONS "${EXTRA_LINK_OPTIONS} ${STATIC_LINK_OPTIONS}")
# endif()

# if(EXTRA_LINK_OPTIONS)
    # target_link_options(ab_server PUBLIC ${EXTRA_LINK_OPTIONS})
    target_link_options(ab_server PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
# endif()

# Build the Modbus TCP server
# This is a lightweight implementation using the coro_net event-based reactor
message("Building Modbus TCP server.")
add_subdirectory("modbus_server")

# Build scan_eip_network utility
message("Building scan_eip_network utility.")
set(SCAN_EIP_NETWORK_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/scan_eip_network/scan_eip_network.c
)
add_executable(scan_eip_network ${SCAN_EIP_NETWORK_FILES}
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/socket.c
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/socket.h
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/buf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/buf.h
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/args.c
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/args.h
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/log.c
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/log.h
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/log_modules.def
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/utils.c
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/utils.h
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/coro_net.c
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/coro_net.h
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/err.c
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/err.h
)
target_link_libraries(scan_eip_network PUBLIC ${EXTRA_LINKER_LIBS})
target_link_options(scan_eip_network PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})


