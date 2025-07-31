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

# make really sure we output the command lines used to build the project
set(CMAKE_VERBOSE_MAKEFILE On)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

cmake_minimum_required (VERSION 3.10)
cmake_policy(SET CMP0054 NEW)

# debugging help
set(CMAKE_BUILD_PARALLEL_LEVEL 1)


# this is the root libplctag project
project (libplctag_project)

# we use C11 now
set(C_STANDARD 11)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# message(STATUS "Compiler Implicit Includes: ${CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES}")

# make sure our outputs are going somewhere sane
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin_dist)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin_dist)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin_dist)


# the project version
set (libplctag_VERSION_MAJOR 2)
set (libplctag_VERSION_MINOR 6)
set (libplctag_VERSION_PATCH 9)
set (VERSION "${libplctag_VERSION_MAJOR}.${libplctag_VERSION_MINOR}.${libplctag_VERSION_PATCH}")

set (LIB_NAME_SUFFIX "${libplctag_VERSION_MAJOR}.${libplctag_VERSION_MINOR}.${libplctag_VERSION_PATCH}")


# build examples flag
set(BUILD_EXAMPLES 1 CACHE BOOL "Build example executables or not")
set(BUILD_TESTS 1 CACHE BOOL "Build test executables or not")
set(BUILD_32_BIT 0 CACHE BOOL "Linux 32-bit build selector")
set(BUILD_MODBUS_EMULATOR 0 CACHE BOOL "Build the Modbus emulator.  Requires libmodbus!")
set(USE_SANITIZERS 1 CACHE BOOL "Build with debug sanitizers or not")

# message("CMAKE_GENERATOR = ${CMAKE_GENERATOR}")

# we need threads
find_package(Threads REQUIRED)

# C compiler-specific settings
if(${CMAKE_GENERATOR} MATCHES "MinGW Makefiles")
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake_toolchains/mingw.cmake")
elseif(CMAKE_C_COMPILER_ID STREQUAL "Clang" OR CMAKE_C_COMPILER_ID STREQUAL "AppleClang" OR CMAKE_C_COMPILER_ID STREQUAL "GNU")
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake_toolchains/clang_or_gcc.cmake")
elseif (CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake_toolchains/msvc.cmake")
else()
    message("Unsupported compiler ${CMAKE_COMPILER_ID}!")
    return()
endif()


# Target platform-specific config
if(ANDROID_ABI OR ANDROID)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake_toolchains/android.cmake")
elseif(APPLE)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake_toolchains/apple.cmake")
elseif(UNIX)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake_toolchains/unix.cmake")
elseif(WIN32)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake_toolchains/windows.cmake")
else()
    message("Unsupported platform!")
    return()
endif()



# where to find include files.
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/src" "${PLATFORM_SHIM_PATH}")

# build the libplctag library, shared and static
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/src/libplctag")

if(BUILD_EXAMPLES)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/src/examples")
endif(BUILD_EXAMPLES)

if(BUILD_TESTS)
   add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/src/tests")
endif(BUILD_TESTS)


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
CONFIGURE_FILE("${CMAKE_CURRENT_SOURCE_DIR}/libplctag.pc.in" "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libplctag.pc" @ONLY)

# build the GitHub Actions config file
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)  # Only if this is the main project
  CONFIGURE_FILE("${CMAKE_CURRENT_SOURCE_DIR}/.github/workflows/ci.yml.in" "${CMAKE_CURRENT_SOURCE_DIR}/.github/workflows/ci.yml" @ONLY)
endif()


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
