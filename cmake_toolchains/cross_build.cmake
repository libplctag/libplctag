

if(${CROSS_BUILD_TYPE} MATCHES "Linux-Arm6")
    # this also announces that we are cross compiling
    set(CMAKE_SYSTEM_NAME Linux)

    # set the CPU/architecture type
    set(CMAKE_SYSTEM_PROCESSOR armv6)

    # where are we going to get the cross compiler?
    set(CMAKE_C_COMPILER "/usr/bin/arm-linux-gnueabihf-gcc")
    set(CMAKE_CXX_COMPILER "/usr/bin/arm-linux-gnueabihf-g++")

    # where are the include files and libraries?
    set(CMAKE_FIND_ROOT_PATH "/usr/arm-linux-gnueabihf")

    # search programs in the host environment only.
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

    message("Building for Linux (Ubuntu/Debian) Arm v6")
elseif(${CROSS_BUILD_TYPE} MATCHES "Linux-Arm7")
    # this also announces that we are cross compiling
    set(CMAKE_SYSTEM_NAME Linux)

    # set the CPU/architecture type
    set(CMAKE_SYSTEM_PROCESSOR armv7l)

    # where are we going to get the cross compiler?
    set(CMAKE_C_COMPILER "/usr/bin/arm-linux-gnueabihf-gcc")
    set(CMAKE_CXX_COMPILER "/usr/bin/arm-linux-gnueabihf-g++")

    # where are the include files and libraries?
    set(CMAKE_FIND_ROOT_PATH "/usr/arm-linux-gnueabihf")

    # search programs in the host environment only.
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

    message("Building for Linux (Ubuntu/Debian) Arm v7 32-bit")
elseif(${CROSS_BUILD_TYPE} MATCHES "Linux-Aarch64")
    # this also announces that we are cross compiling
    set(CMAKE_SYSTEM_NAME Linux)

    # set the CPU/architecture type
    set(CMAKE_SYSTEM_PROCESSOR ARM64)

    # where are we going to get the cross compiler?
    set(CMAKE_C_COMPILER "/usr/bin/aarch64-linux-gnu-gcc")
    set(CMAKE_CXX_COMPILER "/usr/bin/aarch64-linux-gnu-g++")

    # where are the include files and libraries?
    set(CMAKE_FIND_ROOT_PATH "/usr/aarch64-linux-gnu")

    # search programs in the host environment only.
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

    message("Building for Linux (Ubuntu/Debian) Arm v8 64-bit")
elseif(${CROSS_BUILD_TYPE})
    message("Unsupported cross-compilation build type: ${CROSS_BUILD_TYPE}!")
    message("   Use -DCROSS_BUILD_TYPE=<type>")
    message("   where <type> is one of:")
    message("      Linux-Arm6 - older Linux 32-bit Arm v6 systems.")
    message("      Linux-Arm7 - newer Linux 32-bit Arm v7l systems.")
    message("      Linux-Aarch64 - newer Linux 64-bit Arm v8 systems.")
    return()
endif()

# pull in the GCC config
include("${CMAKE_CURRENT_LIST_DIR}/clang_or_gcc.cmake")

set(POSIX True)