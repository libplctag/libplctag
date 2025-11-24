cmake_minimum_required(VERSION 3.10)

project(modbus_server C)

# Set C11 standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Source files
set(SOURCES
    modbus_server.c
    register_storage.c
    modbus_protocol.c
    ../utils/buf.c
    ../utils/err.c
    ../utils/fsm.c
    ../utils/log.c
    ../utils/reactor.c
    ../utils/socket.c
    ../utils/args.c
    ../utils/utils.c
    ../utils/atomic_utils.c
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

        # Link atomic library for 64-bit atomic operations on 32-bit ARM
        # (atomic_utils.c uses __atomic_store_8, __atomic_load_8, etc.)
        target_link_libraries(modbus_server atomic)
    endif()

    # Link math library (modbus_server uses sqrt)
    # Use library name directly; CMake and linker will resolve it appropriately
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
