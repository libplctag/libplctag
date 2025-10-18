cmake_minimum_required(VERSION 3.10)

project(modbus_server C)

# Set C11 standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Source files
set(SOURCES
    modbus_server.c
    logger.c
    socket_utils.c
    config.c
    register_storage.c
    modbus_protocol.c
)

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
endif()

# Compiler-specific warnings
if(MSVC)
    target_compile_options(modbus_server PRIVATE /W4)
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
