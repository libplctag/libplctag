#   Copyright (C) 2025 by Kyle Hayes
#   Author Kyle Hayes  kyle.hayes@gmail.com
#
# This software is available under either the Mozilla Public license
# version 2.0 (MPL 2.0) or the GNU LGPL version 2 (or later) license, whichever
# you choose.
#
# Alpine Linux with musl C library toolchain configuration

message("Building for Alpine Linux with musl C library.")

# Set the platform shim path
set(PLATFORM_SHIM_PATH "${CMAKE_CURRENT_LIST_DIR}/../src/platform/posix" )

# Alpine/musl specific settings
set(POSIX True)
set(ALPINE_MUSL True)

# Detect if we're running on Alpine natively
if(EXISTS "/etc/alpine-release")
    set(NATIVE_ALPINE True)
    message("Native Alpine Linux build detected.")
else()
    set(NATIVE_ALPINE False)
    message("Cross-compiling for Alpine Linux.")
endif()

# Function to detect musl C library
function(detect_musl_libc OUTPUT_VAR)
    # Try to detect musl by checking ldd version output
    execute_process(
        COMMAND ldd --version
        OUTPUT_VARIABLE LDD_OUTPUT
        ERROR_VARIABLE LDD_ERROR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE LDD_RESULT
    )
    
    if(LDD_OUTPUT MATCHES "musl" OR LDD_ERROR MATCHES "musl")
        set(${OUTPUT_VAR} True PARENT_SCOPE)
        message("musl C library detected via ldd.")
    else()
        # Fallback: check if musl-gcc is available
        find_program(MUSL_GCC musl-gcc)
        if(MUSL_GCC)
            set(${OUTPUT_VAR} True PARENT_SCOPE)
            message("musl-gcc wrapper found: ${MUSL_GCC}")
        else()
            set(${OUTPUT_VAR} False PARENT_SCOPE)
        endif()
    endif()
endfunction()

# Architecture detection and cross-compilation setup
if(NOT CMAKE_SYSTEM_PROCESSOR)
    execute_process(
        COMMAND uname -m
        OUTPUT_VARIABLE DETECTED_ARCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    set(CMAKE_SYSTEM_PROCESSOR ${DETECTED_ARCH})
endif()

message("Target architecture: ${CMAKE_SYSTEM_PROCESSOR}")

# Set up cross-compilation if not building natively on Alpine
if(NOT NATIVE_ALPINE)
    # Detect host system to set up appropriate cross-compilation
    if(APPLE)
        message("Setting up Alpine cross-compilation from macOS.")
        # On macOS, we'll need to use Docker or a cross-compilation toolchain
        # Check for common musl cross-compiler locations
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
            find_program(MUSL_CC x86_64-linux-musl-gcc)
            find_program(MUSL_CXX x86_64-linux-musl-g++)
            if(NOT MUSL_CC)
                # Try alternative naming
                find_program(MUSL_CC x86_64-musl-linux-gnu-gcc)
                find_program(MUSL_CXX x86_64-musl-linux-gnu-g++)
            endif()
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
            find_program(MUSL_CC aarch64-linux-musl-gcc)
            find_program(MUSL_CXX aarch64-linux-musl-g++)
            if(NOT MUSL_CC)
                # Try alternative naming
                find_program(MUSL_CC aarch64-musl-linux-gnu-gcc)
                find_program(MUSL_CXX aarch64-musl-linux-gnu-g++)
            endif()
        endif()
    elseif(UNIX AND NOT APPLE)
        message("Setting up Alpine cross-compilation from Linux.")
        # On Linux, check for musl cross-compilers
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
            find_program(MUSL_CC x86_64-linux-musl-gcc)
            find_program(MUSL_CXX x86_64-linux-musl-g++)
            # Also try musl-gcc wrapper
            if(NOT MUSL_CC)
                find_program(MUSL_CC musl-gcc)
                find_program(MUSL_CXX musl-g++)
            endif()
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
            find_program(MUSL_CC aarch64-linux-musl-gcc)
            find_program(MUSL_CXX aarch64-linux-musl-g++)
        endif()
    endif()
    
    # Set cross-compilation variables if we found the toolchain
    if(MUSL_CC AND MUSL_CXX)
        set(CMAKE_SYSTEM_NAME Linux)
        set(CMAKE_C_COMPILER ${MUSL_CC})
        set(CMAKE_CXX_COMPILER ${MUSL_CXX})
        message("Using musl cross-compiler: ${MUSL_CC}")
        
        # Get the toolchain directory for finding libraries
        get_filename_component(MUSL_TOOLCHAIN_DIR ${MUSL_CC} DIRECTORY)
        get_filename_component(MUSL_TOOLCHAIN_DIR ${MUSL_TOOLCHAIN_DIR} DIRECTORY)
        set(CMAKE_FIND_ROOT_PATH ${MUSL_TOOLCHAIN_DIR})
        
        # Search for programs in the build host directories
        set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
        # Search for libraries and headers in the target directories
        set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
        set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
        set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
    else()
        message(WARNING "musl cross-compiler not found. You may need to install it:")
        message("  On Ubuntu/Debian: sudo apt-get install musl-tools musl-dev")
        message("  On macOS with Homebrew: brew install FiloSottile/musl-cross/musl-cross")
        message("  Or build from source: https://github.com/richfelker/musl-cross-make")
    endif()
else()
    # Native Alpine build - detect musl
    detect_musl_libc(HAS_MUSL)
    if(NOT HAS_MUSL)
        message(WARNING "musl C library not detected on this Alpine system.")
    endif()
endif()

# Alpine/musl specific compiler flags
set(ALPINE_MUSL_FLAGS "")

# musl-specific feature test macros
set(ALPINE_MUSL_FLAGS "${ALPINE_MUSL_FLAGS} -D_GNU_SOURCE")
set(ALPINE_MUSL_FLAGS "${ALPINE_MUSL_FLAGS} -D_POSIX_C_SOURCE=200809L")
set(ALPINE_MUSL_FLAGS "${ALPINE_MUSL_FLAGS} -D_XOPEN_SOURCE=700")

# Static linking is preferred for Alpine/musl distributions for better portability
set(ALPINE_STATIC_FLAGS "")
if(NOT BUILD_SHARED_LIBS)
    set(ALPINE_STATIC_FLAGS "-static")
    message("Configuring for static linking (recommended for Alpine).")
endif()

# Threading library for musl
set(EXTRA_LINKER_LIBS "${EXTRA_LINKER_LIBS}" pthread)

# Set Alpine-specific compile flags
set(EXTRA_COMPILE_FLAGS_MINSIZEREL "${EXTRA_COMPILE_FLAGS_MINSIZEREL} ${ALPINE_MUSL_FLAGS}")
set(EXTRA_COMPILE_FLAGS_DEBUG "${EXTRA_COMPILE_FLAGS_DEBUG} ${ALPINE_MUSL_FLAGS}")

# Set Alpine-specific link flags
set(EXTRA_LINK_FLAGS_MINSIZEREL "${EXTRA_LINK_FLAGS_MINSIZEREL} ${ALPINE_STATIC_FLAGS}")
set(EXTRA_LINK_FLAGS_DEBUG "${EXTRA_LINK_FLAGS_DEBUG}")

# Include the GCC/Clang configuration but override some settings for musl
include("${CMAKE_CURRENT_LIST_DIR}/clang_or_gcc.cmake")

# Override some settings from clang_or_gcc.cmake for musl compatibility
if(NOT APPLE AND ALPINE_STATIC_FLAGS)
    set(STATIC_C_LINKER_OPTIONS "${ALPINE_STATIC_FLAGS}")
    set(STATIC_CXX_LINKER_OPTIONS "${ALPINE_STATIC_FLAGS}")
endif()

# Disable sanitizers for static builds as they don't work well with musl static linking
if(USE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug" AND ALPINE_STATIC_FLAGS)
    message(WARNING "Disabling sanitizers for Alpine static build as they don't work well with musl static linking.")
    # Clear sanitizer flags that might have been set in clang_or_gcc.cmake
    string(REGEX REPLACE "-fsanitize=[a-zA-Z,]*" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    string(REGEX REPLACE "-fsanitize=[a-zA-Z,]*" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REGEX REPLACE "-fsanitize=[a-zA-Z,]*" "" EXTRA_COMPILE_FLAGS_DEBUG "${EXTRA_COMPILE_FLAGS_DEBUG}")
endif()

# musl-specific optimizations
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    # musl benefits from size optimizations
    set(EXTRA_COMPILE_FLAGS_MINSIZEREL "${EXTRA_COMPILE_FLAGS_MINSIZEREL} -ffunction-sections -fdata-sections")
    if(ALPINE_STATIC_FLAGS)
        set(EXTRA_LINK_FLAGS_MINSIZEREL "${EXTRA_LINK_FLAGS_MINSIZEREL} -Wl,--gc-sections")
    endif()
endif()

message("Alpine/musl configuration complete.")
message("  Static linking: ${ALPINE_STATIC_FLAGS}")
message("  musl flags: ${ALPINE_MUSL_FLAGS}")
message("  Extra linker libs: ${EXTRA_LINKER_LIBS}")