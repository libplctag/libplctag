#   Copyright (C) 2026 by Kyle Hayes
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

cmake_minimum_required (VERSION 3.15)
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
set (libplctag_VERSION_MINOR 7)
set (libplctag_VERSION_PATCH 0)
set (VERSION "${libplctag_VERSION_MAJOR}.${libplctag_VERSION_MINOR}.${libplctag_VERSION_PATCH}")

set (LIB_NAME_SUFFIX "${libplctag_VERSION_MAJOR}.${libplctag_VERSION_MINOR}.${libplctag_VERSION_PATCH}")


# build examples flag
set(BUILD_EXAMPLES 1 CACHE BOOL "Build example executables or not")
set(BUILD_TESTS 1 CACHE BOOL "Build test executables or not")
set(BUILD_32_BIT 0 CACHE BOOL "Linux 32-bit build selector")
set(USE_SANITIZERS 1 CACHE BOOL "Build with debug sanitizers or not")
set(BUILD_ALPINE_MUSL 0 CACHE BOOL "Build for Alpine Linux with musl C library")
set(MAX_DEBUG_LEVEL 4 CACHE STRING "Maximum debug level to include in the build (0-5)")
set(LIBPLCTAG_ENABLE_COVERAGE 0 CACHE BOOL "Enable code coverage measurement")

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
if(BUILD_ALPINE_MUSL)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake_toolchains/alpine.cmake")
elseif(ANDROID_ABI OR ANDROID)
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


# Generate files from templates
CONFIGURE_FILE("${CMAKE_CURRENT_SOURCE_DIR}/libplctag.pc.in" "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libplctag.pc" @ONLY)

# where to find include files.
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/src" "${PLATFORM_SHIM_PATH}")

# build the libplctag library, shared and static
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/src/libplctag")

if(BUILD_EXAMPLES)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/src/examples")
endif(BUILD_EXAMPLES)

if(BUILD_TESTS)
   enable_testing()
   add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/src/tests")
endif(BUILD_TESTS)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/src/tools")

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/src/poc")




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

# CPack configuration for generating packages

# Set package metadata
set(CPACK_PACKAGE_NAME "libplctag")
set(CPACK_PACKAGE_VENDOR "Kyle Hayes")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A library for communicating with Allen-Bradley/Rockwell PLCs")
set(CPACK_PACKAGE_DESCRIPTION "libplctag is a portable and simple library for accessing Allen-Bradley and compatible PLCs. It provides both synchronous and asynchronous access to tags in Logix-class PLCs.")

set(CPACK_PACKAGE_VERSION_MAJOR "${libplctag_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${libplctag_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${libplctag_VERSION_PATCH}")
set(CPACK_PACKAGE_VERSION "${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}")
set(CPACK_PACKAGE_CONTACT "Kyle Hayes <kyle.hayes@gmail.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/libplctag/libplctag")

# License information
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE-1.MPL")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

# Debian package configuration (global)
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_DEBIAN_PACKAGE_CONTACT "Kyle Hayes <kyle.hayes@gmail.com>")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Kyle Hayes <kyle.hayes@gmail.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "libs")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/libplctag/libplctag")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

# RPM package configuration  
set(CPACK_RPM_COMPONENT_INSTALL ON)
set(CPACK_RPM_PACKAGE_SUMMARY "Library for communicating with Allen-Bradley PLCs")
set(CPACK_RPM_PACKAGE_DESCRIPTION "libplctag is a portable and simple library for accessing Allen-Bradley and compatible PLCs. It provides both synchronous and asynchronous access to tags in Logix-class PLCs.")
set(CPACK_RPM_PACKAGE_GROUP "System Environment/Libraries")
set(CPACK_RPM_PACKAGE_LICENSE "LGPL-2.0 OR MPL-2.0")
set(CPACK_RPM_PACKAGE_URL "https://github.com/libplctag/libplctag")
set(CPACK_RPM_PACKAGE_REQUIRES "glibc >= 2.17")
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)

# Source package configuration
set(CPACK_SOURCE_GENERATOR "TGZ;ZIP")
set(CPACK_SOURCE_IGNORE_FILES
    ${CMAKE_BINARY_DIR}
    /.git/
    /\\.gitignore
    /\\.github/
    /build/
    /\\.vscode/
    /\\.DS_Store
)

# Component packaging: two packages (runtime, devel)
set(CPACK_COMPONENTS_ALL runtime devel)

# Component descriptions
set(CPACK_COMPONENT_RUNTIME_DESCRIPTION "Shared library for libplctag")
set(CPACK_COMPONENT_DEVEL_DESCRIPTION "Headers, static library, and tag_rw2 tool for libplctag development")

# Debian component package names and relationships
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME "libplctag")
set(CPACK_DEBIAN_DEVEL_PACKAGE_NAME "libplctag-dev")
set(CPACK_DEBIAN_RUNTIME_PACKAGE_DEPENDS "libc6 (>= 2.17)")
set(CPACK_DEBIAN_DEVEL_PACKAGE_DEPENDS "libplctag (= ${CPACK_PACKAGE_VERSION})")

# RPM component package names and relationships
set(CPACK_RPM_RUNTIME_PACKAGE_NAME "libplctag")
set(CPACK_RPM_DEVEL_PACKAGE_NAME "libplctag-devel")
set(CPACK_RPM_DEVEL_PACKAGE_REQUIRES "libplctag = ${CPACK_PACKAGE_VERSION}")

# Code Coverage Target
if(LIBPLCTAG_ENABLE_COVERAGE AND (CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID MATCHES "Clang"))
    find_program(LCOV_PATH lcov)
    find_program(GENHTML_PATH genhtml)
    if(LCOV_PATH AND GENHTML_PATH)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang")
            # If using clang, we might need llvm-cov. Try to find it.
            find_program(LLVM_COV_PATH llvm-cov)
            if(LLVM_COV_PATH)
                set(GCOV_TOOL "--gcov-tool=${CMAKE_CURRENT_SOURCE_DIR}/cmake/llvm-gcov.sh")
                # Create a simple wrapper for llvm-cov gcov since lcov needs an executable
                file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/llvm-gcov.sh "#!/bin/bash\nexec ${LLVM_COV_PATH} gcov \"$@\"")
                execute_process(COMMAND chmod +x ${CMAKE_CURRENT_SOURCE_DIR}/cmake/llvm-gcov.sh)
            endif()
        endif()
        
        add_custom_target(coverage
            COMMAND ${LCOV_PATH} --capture --directory . --output-file coverage.info ${GCOV_TOOL} --ignore-errors gcov,unsupported
            COMMAND ${LCOV_PATH} --remove coverage.info "*/usr/*" "*/tests/*" "*/examples/*" "*/poc/*" "*/tools/*" --output-file coverage_filtered.info --ignore-errors unused
            COMMAND ${GENHTML_PATH} coverage_filtered.info --output-directory coverage_report --ignore-errors unsupported
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Generating code coverage report in ${CMAKE_BINARY_DIR}/coverage_report/"
            VERBATIM
        )
    else()
        message(WARNING "lcov and/or genhtml not found. Coverage target not created.")
    endif()
endif()

# include the functions for CPack (after all CPACK_* variables are set)
include(CPack)


