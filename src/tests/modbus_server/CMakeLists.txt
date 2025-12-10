cmake_minimum_required(VERSION 3.10)

project(modbus_server C)

# Set C11 standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Source files
# Note: Uses coro_net instead of fsm and reactor
set(SOURCES
    modbus_server.c
    register_storage.c
    modbus_protocol.c
    ../utils/buf.c
    ../utils/err.c
    ../utils/log.c
    ../utils/args.c
    ../utils/utils.c
    ../utils/coro_net.c
    ../utils/socket.c
)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../utils)

# Create executable
add_executable(modbus_server ${SOURCES})

# Platform-specific settings
if(WIN32)
    # Windows requires Winsock2
    target_link_libraries(modbus_server ws2_32)

    # Suppress Windows-specific warnings
    target_compile_definitions(modbus_server PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _WINSOCK_DEPRECATED_NO_WARNINGS
    )
elseif(UNIX)
    # Unix-like systems may need additional libraries
    if(NOT APPLE)
        # Linux may need -lrt for some older systems
        find_library(RT_LIBRARY rt)
        if(RT_LIBRARY)
            target_link_libraries(modbus_server ${RT_LIBRARY})
        endif()
    endif()

    # Link math library (logging and protocol may use math functions)
    # Math functions are in libm on Unix but in the C runtime on Windows
    target_link_libraries(modbus_server m)
endif()

# Compiler-specific warnings
if(MSVC)
    # Replace /W3 with /W4 (CMake may warn about this override, but it's expected)
    string(REPLACE "/W3" "/W4" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
    string(REPLACE "/W3" "/W4" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    string(REPLACE "/W3" "/W4" CMAKE_C_FLAGS_MINSIZEREL "${CMAKE_C_FLAGS_MINSIZEREL}")
    target_compile_options(modbus_server PRIVATE /wd4996)  # Suppress deprecation warnings
else()
    target_compile_options(modbus_server PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter
    )
endif()

# Optional: Add debug symbols for debug builds
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(MSVC)
        target_compile_options(modbus_server PRIVATE /Zi)
    else()
        target_compile_options(modbus_server PRIVATE -g)
    endif()
endif()

# Installation
install(TARGETS modbus_server
    RUNTIME DESTINATION bin
)

# CMakeLists.txt for src/tests/test_utils/

enable_testing()

# # Helper function to add coro_net test
# function(add_coro_net_test test_name test_source)
#     add_executable(${test_name} ${test_source})
#     target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
#     target_link_libraries(${test_name} coro_net)
#     add_test(NAME ${test_name} COMMAND ${test_name})
# endfunction()

# # Test: Lifecycle
# add_coro_net_test(unit_coro_net_lifecycle test_coro_net_lifecycle.c)

# # Test: Tasks (replaces old sockets test)
# add_coro_net_test(unit_coro_net_tasks test_coro_net_tasks.c)

# # Test: Socket-less Tasks (replaces old timers test)
# add_coro_net_test(unit_coro_net_socketless test_coro_net_socketless.c)

# # Test: Macros
# add_coro_net_test(unit_coro_net_macros test_coro_net_macros.c)
