#   Copyright (C) 2025 by Kyle Hayes
#   Author Kyle Hayes  kyle.hayes@gmail.com
#
# This software is available under either the Mozilla Public license
# version 2.0 (MPL 2.0) or the GNU LGPL version 2 (or later) license, whichever
# you choose.
#
# MPL 2.0:
#
#   This Source Code Form is subject to the terms of the Mozilla Public
#   License, v. 2.0. If a copy of the MPL was not distributed with this
#   file, You can obtain one at http://mozilla.org/MPL/2.0/.
#
#
# LGPL 2:
#
#   This program is free software; you can redistribute it and/or modify
#   it under the terms of the GNU Library General Public License as
#   published by the Free Software Foundation; either version 2 of the
#   License, or (at your option) any later version.
#
#   This program is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#
#   You should have received a copy of the GNU Library General Public
#   License along with this program; if not, write to the
#   Free Software Foundation, Inc.,
#   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

set(CMAKE_VERBOSE_MAKEFILE On)

cmake_minimum_required (VERSION 3.10)
cmake_policy(SET CMP0054 NEW)


# this is the root libplctag project
project (libplctag_project)

# we use C11 now
set(C_STANDARD 11)

# make sure our outputs are going somewhere sane
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin_dist)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin_dist)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin_dist)


# the project version
set (libplctag_VERSION_MAJOR 2)
set (libplctag_VERSION_MINOR 6)
set (libplctag_VERSION_PATCH 4)
set (VERSION "${libplctag_VERSION_MAJOR}.${libplctag_VERSION_MINOR}.${libplctag_VERSION_PATCH}")

set (LIB_NAME_SUFFIX "${libplctag_VERSION_MAJOR}.${libplctag_VERSION_MINOR}.${libplctag_VERSION_PATCH}")


# build examples flag
set(BUILD_EXAMPLES 1 CACHE BOOL "Build example executables or not")
set(BUILD_TESTS 1 CACHE BOOL "Build test executables or not")
set(BUILD_32_BIT 0 CACHE BOOL "Linux 32-bit build selector")

set(USE_SANITIZERS 1 CACHE BOOL "Build with debug sanitizers or not")



# set a variable based on Android use
if(ANDROID_ABI OR ANDROID)
    message("Building on Android.")
    set(ANDROID_BUILD 1)
else()
    message("Not building on Android.")
    set(ANDROID_BUILD 0)
endif()


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

# set compiler and flags for 32-bit MinGW builds
if (${CMAKE_GENERATOR} MATCHES "MinGW Makefiles")
    SET(CMAKE_C_COMPILER gcc.exe)
    SET(CMAKE_C_FLAGS "-m32 -mno-ms-bitfields -D_WIN32_WINNT=0x0600")
    SET(CMAKE_CXX_COMPILER g++.exe)
    SET(CMAKE_CXX_FLAGS "-m32 -mno-ms-bitfields -D_WIN32_WINNT=0x0600")
endif()


message("CMAKE_GENERATOR = ${CMAKE_GENERATOR}")

# set flags for MacOSX
if (APPLE)
    set(CMAKE_MACOSX_RPATH ON)
endif (APPLE)



# we need threads
find_package(Threads REQUIRED)

# set the main paths.
set ( base_SRC_PATH "${CMAKE_SOURCE_DIR}/src" )
set ( lib_SRC_PATH "${CMAKE_SOURCE_DIR}/src/libplctag/lib" )
set ( protocol_SRC_PATH "${CMAKE_SOURCE_DIR}/src/libplctag/protocols" )
set ( ab_SRC_PATH "${protocol_SRC_PATH}/ab" )
set ( mb_SRC_PATH "${protocol_SRC_PATH}/mb" )
set ( util_SRC_PATH "${CMAKE_SOURCE_DIR}/src/util" )
set ( example_SRC_PATH "${base_SRC_PATH}/src/examples" )
set ( test_SRC_PATH "${base_SRC_PATH}/src/tests" )
set ( cli_SRC_PATH "${base_SRC_PATH}/src/contrib/cli" )

# OS-specific files for the platform code.
if (UNIX OR APPLE)
    set ( platform_SRC_PATH "${base_SRC_PATH}/platform/posix" )
elseif (WIN32)
    set ( platform_SRC_PATH "${base_SRC_PATH}/platform/windows" )
else()
    message("No platform code path set!")
    return()
endif()


# where to find include files.
include_directories("${base_SRC_PATH}")
include_directories("${platform_SRC_PATH}")
include_directories("${protocol_SRC_PATH}")

# C compiler specific settings
if(CMAKE_C_COMPILER_ID STREQUAL "Clang" OR CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
    # using Clang

    SET(STR_OP_OVERFLOW_ON "")
    SET(STR_OP_OVERFLOW_OFF "")

    set(CMAKE_C_FLAGS_RELEASE_INIT "-Os -DNDEBUG -Wall -pedantic -Wextra -Wconversion -fms-extensions -fno-strict-aliasing -fvisibility=hidden ${STR_OP_OVERFLOW_OFF}")
    set(CMAKE_C_FLAGS_DEBUG_INIT " -O0 -g -Wall -pedantic -Wextra -Wconversion -fno-strict-aliasing -fvisibility=hidden -fno-omit-frame-pointer ${STR_OP_OVERFLOW_ON}")
    set(BASE_RELEASE_LINK_FLAGS "")
    set(BASE_DEBUG_LINK_FLAGS "")

    if(USE_SANITIZERS)
        if(APPLE)
            SET(SANITIZERS_FLAGS " -fsanitize=address -fsanitize=undefined")
        else()
            SET(SANITIZERS_FLAGS " -fsanitize=address -fsanitize=leak -fsanitize=undefined")
        endif()
        STRING(APPEND CMAKE_C_FLAGS_DEBUG_INIT ${SANITIZERS_FLAGS})
        STRING(APPEND BASE_DEBUG_LINK_FLAGS ${SANITIZERS_FLAGS})
    endif()

    if(APPLE)
        set(CMAKE_C_FLAGS_RELEASE_INIT "${CMAKE_C_FLAGS_RELEASE_INIT} -D_DARWIN_C_SOURCE")
        set(CMAKE_C_FLAGS_DEBUG_INIT "${CMAKE_C_FLAGS_DEBUG_INIT} -D_DARWIN_C_SOURCE")
    else()
        set(CMAKE_C_FLAGS_RELEASE_INIT "${CMAKE_C_FLAGS_RELEASE_INIT} -D__USE_POSIX=1 -D_XOPEN_SOURCE=700 -D_POSIX_C_SOURCE=200809L")
        set(CMAKE_C_FLAGS_DEBUG_INIT "${CMAKE_C_FLAGS_DEBUG_INIT} -D__USE_POSIX=1 -D_XOPEN_SOURCE=700 -D_POSIX_C_SOURCE=200809L")
    endif()


    # check to see if we are building 32-bit or 64-bit
    if(BUILD_32_BIT)
        set(CMAKE_C_FLAGS_RELEASE_INIT "${CMAKE_C_FLAGS_RELEASE_INIT} -m32")
        set(CMAKE_C_FLAGS_DEBUG_INIT "${CMAKE_C_FLAGS_DEBUG_INIT} -m32")
        set(BASE_RELEASE_LINK_FLAGS "${BASE_RELEASE_LINK_FLAGS} -m32")
        set(BASE_DEBUG_LINK_FLAGS "${BASE_DEBUG_LINK_FLAGS} -m32")
    endif()
elseif (CMAKE_C_COMPILER_ID STREQUAL "GNU")
    # using GCC

	SET(C11_CHECK "")

	# get GCC version
	if(${CMAKE_VERSION} VERSION_GREATER 2.8.9)
		MESSAGE(STATUS "CMake is newer, using a sane way to get the GCC version..")
		SET(COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
	else()
		# old CMake.   Do this the hard way.
		MESSAGE(STATUS "CMake is old, using the old way to get the GCC version.")
		EXECUTE_PROCESS(COMMAND ${CMAKE_C_COMPILER} -dumpversion OUTPUT_VARIABLE COMPILER_VERSION)
		MESSAGE(STATUS "COMPILER_VERSION=" ${COMPILER_VERSION})
	endif()

	if(${COMPILER_VERSION} VERSION_GREATER 5.1)
		# The compiler has the C99-c11-compat check.
		MESSAGE(STATUS "Compiler supports C99/C11 compatibility check." )
	else()
		MESSAGE(STATUS "Compiler does not support C99/C11 compatibility check." )
	endif()

    if(${COMPILER_VERSION} VERSION_GREATER 8.0 AND ${COMPILER_VERSION} VERSION_LESS 11.0)
        # the compiler supports the -Wstringop-overflow flag.
        # There is clearly a bug in the compiler that throws this warning inconsistently.
        # Suppress it for RELEASE builds.
        MESSAGE(STATUS "Compiler version ${COMPILER_VERSION} supports string op overflow checking.  Suppress for Release builds.")
        SET(STR_OP_OVERFLOW_ON "-Wstringop-overflow")
        SET(STR_OP_OVERFLOW_OFF "-Wno-stringop-overflow")
    else()
        MESSAGE(STATUS "Compiler version ${COMPILER_VERSION} does not support string op overflow checking.")
        SET(STR_OP_OVERFLOW_ON "")
        SET(STR_OP_OVERFLOW_OFF "")
    endif()

    SET(CMAKE_C_FLAGS_RELEASE " -Os -DNDEBUG -std=c11 -Wall -pedantic -Wextra -Wconversion -fno-strict-aliasing -fvisibility=hidden ${STR_OP_OVERFLOW_OFF} -D_XOPEN_SOURCE=700 -D__USE_POSIX=1 -D_POSIX_C_SOURCE=200809L")
    SET(CMAKE_C_FLAGS_DEBUG "  -O0  -g -std=c11 -Wall -pedantic -Wextra -Wconversion -fno-strict-aliasing -fvisibility=hidden ${STR_OP_OVERFLOW_ON} -D_XOPEN_SOURCE=700 -D__USE_POSIX=1 -D_POSIX_C_SOURCE=200809L")
    SET(BASE_RELEASE_LINK_FLAGS "")
    SET(BASE_DEBUG_LINK_FLAGS "")

    if(USE_SANITIZERS)
        SET(SANITIZERS_FLAGS " -fsanitize=address -fsanitize=leak -fsanitize=undefined")
        STRING(APPEND CMAKE_C_FLAGS_DEBUG_INIT ${SANITIZERS_FLAGS})
        STRING(APPEND BASE_DEBUG_LINK_FLAGS ${SANITIZERS_FLAGS})
    endif()

    # check to see if we are building 32-bit or 64-bit
    if(BUILD_32_BIT)
        set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -m32")
        set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -m32")
        set(BASE_RELEASE_LINK_FLAGS "${BASE_RELEASE_LINK_FLAGS} -m32")
        set(BASE_DEBUG_LINK_FLAGS "${BASE_DEBUG_LINK_FLAGS} -m32")
    endif()
elseif (CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    # using Visual Studio C/C++
    set(CMAKE_C_FLAGS_RELEASE "/O2 /Ob2 /DNDEBUG /W3 /MT /c /std:c11 -D_CRT_SECURE_NO_WARNINGS")
    set(CMAKE_C_FLAGS_DEBUG "/Zi /Ob0 /Od /W3 /MDd /c /std:c11 -D_CRT_SECURE_NO_WARNINGS")
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Ob2 /DNDEBUG /W3 /MT -D_CRT_SECURE_NO_WARNINGS")
    set(CMAKE_CXX_FLAGS_DEBUG "/Zi /Ob0 /Od /W3 /MDd -D_CRT_SECURE_NO_WARNINGS")

    add_definitions(-D_CRT_SECURE_NO_WARNINGS)

    set(EXTRA_LINKER_LIBS ws2_32 bcrypt)

    if(USE_SANITIZERS)
        set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /RTCu")
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /RTCu")

        set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /RTCu")
        set(CMAKE_STATIC_LINKER_FLAGS_RELEASE "${CMAKE_STATIC_LINKER_FLAGS_RELEASE} /RTCu")
    endif()

else()
    message("Unsupported compiler ${CMAKE_COMPILER_ID}!")
    return()
endif()

message("CMAKE_C_FLAGS_RELEASE = ${CMAKE_C_FLAGS_RELEASE}")
message("CMAKE_C_FLAGS_DEBUG = ${CMAKE_C_FLAGS_DEBUG}")
message("CMAKE_CXX_FLAGS_RELEASE = ${CMAKE_C_FLAGS_RELEASE}")
message("CMAKE_CXX_FLAGS_DEBUG = ${CMAKE_C_FLAGS_DEBUG}")

message("CMAKE_SHARED_LINKER_FLAGS_RELEASE = ${CMAKE_SHARED_LINKER_FLAGS_RELEASE}")
message("CMAKE_STATIC_LINKER_FLAGS_RELEASE = ${CMAKE_STATIC_LINKER_FLAGS_RELEASE}")

message("EXTRA_LINKER_LIBS = ${EXTRA_LINKER_LIBS}")

add_subdirectory("${CMAKE_SOURCE_DIR}/src/libplctag")

if(BUILD_EXAMPLES)
    message("Building examples.")
    add_subdirectory("${CMAKE_SOURCE_DIR}/src/examples")
endif(BUILD_EXAMPLES)

#if(BUILD_TESTS)
#    message("Building tests.")
#    add_subdirectory("${CMAKE_SOURCE_DIR}/src/tests")
#endif(BUILD_TESTS)


#    set(CLI_FILES ${cli_SRC_PATH}/cli.c
#        ${cli_SRC_PATH}/cli.h
#        ${cli_SRC_PATH}/getline.h
#        ${cli_SRC_PATH}/uthash.h
#        ${libplctag_SRCS}
#    )

#    foreach(CLI_FILES ${CLI_FILES})
#    set_source_files_properties("${CLI_FILES}" PROPERTIES COMPILE_FLAGS "${BASE_C_FLAGS}" )
#    endforeach()

#    add_executable(cli ${CLI_FILES} "${example_SRC_PATH}/${example_PROG_UTIL}" "${example_SRC_PATH}/utils.h" )

#    target_link_libraries(cli ${example_LIBRARIES} )

#    if(BASE_LINK_FLAGS)
#    set_target_properties(cli PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
#    endif()

# Generate files from templates
CONFIGURE_FILE("${CMAKE_SOURCE_DIR}/libplctag.pc.in" "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libplctag.pc" @ONLY)

# build the GitHub Actions config file
CONFIGURE_FILE("${CMAKE_SOURCE_DIR}/.github/workflows/ci.yml.in" "${CMAKE_SOURCE_DIR}/.github/workflows/ci.yml" @ONLY)


# for installation

macro(print_all_variables)
    message(STATUS "print_all_variables------------------------------------------{")
    get_cmake_property(_variableNames VARIABLES)
    foreach (_variableName ${_variableNames})
        message(STATUS "${_variableName}=${${_variableName}}")
    endforeach()
    message(STATUS "print_all_variables------------------------------------------}")
endmacro()

# Debugging
# print_all_variables()
