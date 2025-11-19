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
)

# Add ab_server executable
add_executable(ab_server ${AB_SERVER_FILES})
target_link_libraries(ab_server PUBLIC ${EXTRA_LINKER_LIBS})

# if(STATIC_LINK_OPTIONS)
#     set(EXTRA_LINK_OPTIONS "${EXTRA_LINK_OPTIONS} ${STATIC_LINK_OPTIONS}")
# endif()

# if(EXTRA_LINK_OPTIONS)
    # target_link_options(ab_server PUBLIC ${EXTRA_LINK_OPTIONS})
    target_link_options(ab_server PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
# endif()

# Build the standalone Modbus TCP server (no external dependencies)
# This is a pure C11 implementation using only BSD sockets
message("Building Modbus TCP server.")
add_subdirectory("modbus_server")

# Register CTest tests for modbus_server
# Test that modbus_server binary exists and is executable
add_test(NAME modbus_server_binary_exists
         COMMAND test -x "${CMAKE_BINARY_DIR}/bin_dist/modbus_server")

# Test that modbus_server can start with single listen address
add_test(NAME modbus_server_single_listener
         COMMAND sh -c "${CMAKE_BINARY_DIR}/bin_dist/modbus_server --listen=127.0.0.1:9502 & sleep 1; kill \$! 2>/dev/null; wait 2>/dev/null; true"
         WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

# Test that modbus_server can start with multiple listen addresses
add_test(NAME modbus_server_multiple_listeners
         COMMAND sh -c "${CMAKE_BINARY_DIR}/bin_dist/modbus_server --listen=127.0.0.1:9502 --listen=127.0.0.1:9503 & sleep 1; kill \$! 2>/dev/null; wait 2>/dev/null; true"
         WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

# Test that modbus_server can start with debug level
add_test(NAME modbus_server_with_debug_level
         COMMAND sh -c "${CMAKE_BINARY_DIR}/bin_dist/modbus_server --listen=127.0.0.1:9504 --debug=DETAIL & sleep 1; kill \$! 2>/dev/null; wait 2>/dev/null; true"
         WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

set_tests_properties(modbus_server_binary_exists PROPERTIES TIMEOUT 5)
set_tests_properties(modbus_server_single_listener PROPERTIES TIMEOUT 5)
set_tests_properties(modbus_server_multiple_listeners PROPERTIES TIMEOUT 5)
set_tests_properties(modbus_server_with_debug_level PROPERTIES TIMEOUT 5)

