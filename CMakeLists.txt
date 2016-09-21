#
#   Copyright 2016, OmanTek
#   Author: Kyle Hayes
#
#    This library is free software; you can redistribute it and/or
#    modify it under the terms of the GNU Library General Public
#    License as published by the Free Software Foundation; either
#    version 2 of the License, or (at your option) any later version.
#
#    This library is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
#    Library General Public License for more details.
#
#    You should have received a copy of the GNU Library General Public
#    License along with this library; if not, write to the Free Software
#    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
#    USA
#

cmake_minimum_required (VERSION 2.8)

# this is the root libplctag project
project (libplctag_project)

# the project is version 1.5
set (libplctag_VERSION_MAJOR 1)
set (libplctag_VERSION_MINOR 5)

# set the main paths.
set ( base_SRC_PATH "${PROJECT_SOURCE_DIR}/src" )
set ( lib_SRC_PATH "${base_SRC_PATH}/lib" )
set ( protocol_SRC_PATH "${base_SRC_PATH}/protocols" )
set ( ab_SRC_PATH "${protocol_SRC_PATH}/ab" )
set ( util_SRC_PATH "${base_SRC_PATH}/util" )
set ( example_SRC_PATH "${base_SRC_PATH}/examples" )

# OS-specific files for the platform code.
# FIXME - does this work for macOS?
if (UNIX)
    set ( platform_SRC_PATH "${base_SRC_PATH}/platform/linux" )
elseif (WIN32)
    set ( platform_SRC_PATH "${base_SRC_PATH}/platform/windows" )
endif()

MESSAGE("platform_SRC_PATH=${platform_SRC_PATH}")
MESSAGE("CMAKE_C_COMPILER_ID=${CMAKE_C_COMPILER_ID}")
MESSAGE("CMAKE_CXX_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}")

# C compiler specific settings
if (CMAKE_C_COMPILER_ID STREQUAL 'Clang')
    # using Clang
    add_definitions(-std=c99)
    add_definitions(-Wall)
    add_definitions(-fms-extensions)
    add_definitions(-fno-strict-aliasing)
    add_definitions(-D__USE_POSIX=1)
    add_definitions(-D_POSIX_C_SOURCE=200809L)
    add_definitions(-fPIC)
    add_definitions(-lpthread)
elseif (CMAKE_C_COMPILER_ID STREQUAL 'GNU')
    # using GCC
    add_definitions(-std=c99)
    add_definitions(-Wall)
    add_definitions(-fms-extensions)
    add_definitions(-fno-strict-aliasing)
    add_definitions(-D__USE_POSIX=1)
    add_definitions(-D_POSIX_C_SOURCE=200809L)
    add_definitions(-fPIC)
    add_definitions(-lpthread)
elseif (CMAKE_C_COMPILER_ID STREQUAL 'Intel')
    # using Intel C/C++
    # FIXME!
elseif (CMAKE_C_COMPILER_ID STREQUAL 'MSVC')
    # using Visual Studio C/C++
    #add_definitions(/MDd)
    add_definitions(/c)
    add_definitions(/DLIBPLCTAGDLL_EXPORTS=1)
    add_definitions(/W3)
endif()

# C++ compiler settings
if (CMAKE_CXX_COMPILER_ID STREQUAL 'Clang')
    # using Clang
    add_definitions(-Wall)
    add_definitions(-fms-extensions)
    add_definitions(-fno-strict-aliasing)
    add_definitions(-D__USE_POSIX=1)
    add_definitions(-D_POSIX_C_SOURCE=200809L)
    add_definitions(-fPIC)
    add_definitions(-lpthread)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL 'GNU')
    # using GCC
    add_definitions(-Wall)
    add_definitions(-fms-extensions)
    add_definitions(-fno-strict-aliasing)
    add_definitions(-D__USE_POSIX=1)
    add_definitions(-D_POSIX_C_SOURCE=200809L)
    add_definitions(-fPIC)
    add_definitions(-lpthread)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL 'Intel')
    # using Intel C++
    # FIXME!
elseif (CMAKE_CXX_COMPILER_ID STREQUAL 'MSVC')
    # using Visual Studio C++
    #add_definitions(/MDd)
    add_definitions(/DLIBPLCTAGDLL_EXPORTS=1)
    add_definitions(/W3)
endif()

# where to find include files.
include_directories("${base_SRC_PATH}")
include_directories("${platform_SRC_PATH}")
include_directories("${protocol_SRC_PATH}")


# set up the library sources
set ( libplctag_SRCS "${lib_SRC_PATH}/libplctag_tag.c"
                     "${ab_SRC_PATH}/ab_common.c"
                     "${ab_SRC_PATH}/cip.c"
                     "${ab_SRC_PATH}/connection.c"
                     "${ab_SRC_PATH}/eip.c"
                     "${ab_SRC_PATH}/eip_cip.c"
                     "${ab_SRC_PATH}/eip_dhp_pccc.c"
                     "${ab_SRC_PATH}/eip_pccc.c"
                     "${ab_SRC_PATH}/pccc.c"
                     "${ab_SRC_PATH}/request.c"
                     "${ab_SRC_PATH}/session.c"
                     "${util_SRC_PATH}/attr.c"
                     "${util_SRC_PATH}/debug.c"
                     "${platform_SRC_PATH}/platform.c" )

# set up the library
add_library(plctag SHARED ${libplctag_SRCS} )

# Windows need to link the library to the WINSOCK library
if (WIN32)
    target_link_libraries(plctag ws2_32)
endif()

# add the examples
if (UNIX)
    set ( example_PROGRAMS async
                           data_dumper
                           multithread
                           multithread_cached_read
                           multithread_plc5
                           multithread_plc5_dhp
                           plc5
                           simple
                           simple_dual
                           slc500
                           stress_test
                           string
                           toggle_bool
                           write_string
                           tag_rw )

    set ( example_PROG_UTIL utils_posix.c)
    set ( example_LIBRARIES plctag pthread )
elseif(WIN32)
    set ( example_PROGRAMS async
                           plc5
                           simple
                           simple_dual
                           slc500
                           string
                           toggle_bool
                           write_string
                           tag_rw )

    set ( example_PROG_UTIL utils_windows.c)
    set ( example_LIBRARIES plctag ws2_32 )
endif()

foreach ( example ${example_PROGRAMS} )
    add_executable( ${example} "${example_SRC_PATH}/${example}.c" "${example_SRC_PATH}/${example_PROG_UTIL}" )
    target_link_libraries(${example} ${example_LIBRARIES} )
endforeach(example)

# different because it is C++
add_executable (simple_cpp "${example_SRC_PATH}/simple_cpp.cpp" "${example_SRC_PATH}/${example_PROG_UTIL}" )
target_link_libraries (simple_cpp ${example_LIBRARIES} )

# for installation
install(TARGETS plctag DESTINATION lib)
install(FILES "${lib_SRC_PATH}/libplctag.h" DESTINATION include)
