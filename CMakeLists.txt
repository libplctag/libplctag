#
#   Copyright 2018, Kyle Hayes
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

# prevent expansion of quoted things that could be variables in if()
if(${CMAKE_VERSION} VERSION_GREATER 3.1)
    cmake_policy(SET CMP0054 NEW)
endif()

# this is the root libplctag project
project (libplctag_project)

# the project is version 1.5
set (libplctag_VERSION_MAJOR 1)
set (libplctag_VERSION_MINOR 6)
set (libplctag_VERSION_PATCH 0)
set (VERSION "${libplctag_VERSION_MAJOR}.${libplctag_VERSION_MINOR}.${libplctag_VERSION_PATCH}")


message("MSVC = ${MSVC}")
if(MSVC)
    message("CMAKE generator: ${CMAKE_GENERATOR}")
    message("CMAKE generator version: ${MSVC_VERSION}")

    if(${MSVC_VERSION} EQUAL 1800)
        set(COMPILE_AS_C 0)
    else()
        set(COMPILE_AS_C 1)
    endif()
endif()




# set the main paths.
set ( base_SRC_PATH "${PROJECT_SOURCE_DIR}/src" )
set ( lib_SRC_PATH "${base_SRC_PATH}/lib" )
set ( protocol_SRC_PATH "${base_SRC_PATH}/protocols" )
set ( ab_SRC_PATH "${protocol_SRC_PATH}/ab" )
set ( util_SRC_PATH "${base_SRC_PATH}/util" )
set ( example_SRC_PATH "${base_SRC_PATH}/examples" )
set ( test_SRC_PATH "${base_SRC_PATH}/tests" )

# OS-specific files for the platform code.
# FIXME - does this work for macOS?
if (UNIX)
    set ( platform_SRC_PATH "${base_SRC_PATH}/platform/linux" )
elseif (WIN32)
    set ( platform_SRC_PATH "${base_SRC_PATH}/platform/windows" )
endif()


# where to find include files.
include_directories("${base_SRC_PATH}")
include_directories("${platform_SRC_PATH}")
include_directories("${protocol_SRC_PATH}")


MESSAGE("base_SRC_PATH=${base_SRC_PATH}")
MESSAGE("platform_SRC_PATH=${platform_SRC_PATH}")
MESSAGE("protocol_SRC_PATH=${protocol_SRC_PATH}")

MESSAGE("CMAKE_C_COMPILER_ID=${CMAKE_C_COMPILER_ID}")
MESSAGE("CMAKE_CXX_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}")


# C compiler specific settings
if (CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # using Clang
    set(BASE_RELEASE_FLAGS "${CMAKE_C_FLAGS} -Wall -pedantic -Wextra -Wc++-compat -Wc99-c11-compat -Wconversion -fms-extensions -fno-strict-aliasing -D__USE_POSIX=1 -D_POSIX_C_SOURCE=200809L")
    set(BASE_DEBUG_FLAGS "${CMAKE_C_FLAGS}  -g -Wall -pedantic -Wextra -Wc++-compat -Wc99-c11-compat -Wconversion -fms-extensions -fno-strict-aliasing -D__USE_POSIX=1 -D_POSIX_C_SOURCE=200809L")
elseif (CMAKE_C_COMPILER_ID STREQUAL "GNU")
    # using GCC
    set(BASE_RELEASE_FLAGS "${CMAKE_C_FLAGS} -Wall -pedantic -Wextra -Wc99-c11-compat -Wconversion -fms-extensions -fno-strict-aliasing -D__USE_POSIX=1 -D_POSIX_C_SOURCE=200809L")
    set(BASE_DEBUG_FLAGS "${CMAKE_C_FLAGS}  -g -Wall -pedantic -Wextra -Wc99-c11-compat -Wconversion -fms-extensions -fno-strict-aliasing -D__USE_POSIX=1 -D_POSIX_C_SOURCE=200809L")
elseif (CMAKE_C_COMPILER_ID STREQUAL "Intel")
    # using Intel C/C++
    MESSAGE("Intel C compiler not supported!")
elseif (CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    # using Visual Studio C/C++
    set(BASE_RELEASE_FLAGS "${CMAKE_C_FLAGS} /DLIBPLCTAGDLL_EXPORTS=1 /W3")
    set(BASE_DEBUG_FLAGS "${CMAKE_C_FLAGS} /DLIBPLCTAGDLL_EXPORTS=1 /W3")
    # /MD$<$<STREQUAL:$<CONFIGURATION>,Debug>:d>
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(BASE_FLAGS "${BASE_DEBUG_FLAGS}")
else()
    set(BASE_FLAGS "${BASE_RELEASE_FLAGS}")
endif()

MESSAGE("BASE_FLAGS=${BASE_FLAGS}")

if (CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    # MSVC 2012 can compile the library as C++, but not C
    if(COMPILE_AS_C)
        message("MSVC can handle C99, compiling code as C")
        set(BASE_C_FLAGS "${BASE_FLAGS} /c")
    else()
        message("MSVC cannot handle C99, compiling code as C++")
        set(BASE_C_FLAGS "${BASE_FLAGS}")
    endif()
else()
    set(BASE_C_FLAGS "${BASE_FLAGS} -std=c99")
endif()

set(BASE_CXX_FLAGS "${BASE_FLAGS}")

MESSAGE("BASE_C_FLAGS=${BASE_C_FLAGS}")
MESSAGE("BASE_CXX_FLAGS=${BASE_CXX_FLAGS}")

# set up the library sources
set ( libplctag_SRCS "${lib_SRC_PATH}/init.c"
                     "${lib_SRC_PATH}/init.h"
                     "${lib_SRC_PATH}/libplctag.h"
                     "${lib_SRC_PATH}/lib.c"
                     "${lib_SRC_PATH}/tag.h"
                     "${ab_SRC_PATH}/ab.h"
                     "${ab_SRC_PATH}/ab_common.c"
                     "${ab_SRC_PATH}/ab_common.h"
                     "${ab_SRC_PATH}/cip.c"
                     "${ab_SRC_PATH}/cip.h"
                     "${ab_SRC_PATH}/defs.h"
                     "${ab_SRC_PATH}/eip.c"
                     "${ab_SRC_PATH}/eip.h"
                     "${ab_SRC_PATH}/eip_cip.c"
                     "${ab_SRC_PATH}/eip_cip.h"
                     "${ab_SRC_PATH}/eip_dhp_pccc.c"
                     "${ab_SRC_PATH}/eip_dhp_pccc.h"
                     "${ab_SRC_PATH}/eip_pccc.c"
                     "${ab_SRC_PATH}/eip_pccc.h"
                     "${ab_SRC_PATH}/error_codes.c"
                     "${ab_SRC_PATH}/error_codes.h"
                     "${ab_SRC_PATH}/pccc.c"
                     "${ab_SRC_PATH}/pccc.h"
                     "${ab_SRC_PATH}/request.c"
                     "${ab_SRC_PATH}/request.h"
                     "${ab_SRC_PATH}/session.c"
                     "${ab_SRC_PATH}/session.h"
                     "${ab_SRC_PATH}/tag.h"
                     "${protocol_SRC_PATH}/system/system.c"
                     "${protocol_SRC_PATH}/system/system.h"
                     "${protocol_SRC_PATH}/system/tag.h"
                     "${util_SRC_PATH}/attr.c"
                     "${util_SRC_PATH}/attr.h"
                     "${util_SRC_PATH}/byteorder.h"
                     "${util_SRC_PATH}/debug.c"
                     "${util_SRC_PATH}/debug.h"
                     "${util_SRC_PATH}/hash.c"
                     "${util_SRC_PATH}/hash.h"
                     "${util_SRC_PATH}/macros.h"
                     "${util_SRC_PATH}/rc.c"
                     "${util_SRC_PATH}/rc.h"
                     "${util_SRC_PATH}/vector.c"
                     "${util_SRC_PATH}/vector.h"
                     "${platform_SRC_PATH}/platform.c"
                     "${platform_SRC_PATH}/platform.h" )

# set the compiler flags
FOREACH( lib_src ${libplctag_SRCS} )
    set_source_files_properties(${lib_src} PROPERTIES COMPILE_FLAGS ${BASE_C_FLAGS})
ENDFOREACH()

# shared library
add_library(plctag SHARED ${libplctag_SRCS} )

# static Library
#SET(BUILD_SHARED_LIBS OFF)
#add_library(plctag STATIC ${libplctag_SRCS} )
#SET(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
#SET(CMAKE_EXE_LINKER_FLAGS "-static")



# Windows needs to link the library to the WINSOCK library
if (WIN32)
    target_link_libraries(plctag ws2_32)
    target_link_libraries(plctag_static ws2_32)
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
                           stress_api_lock
                           stress_test
                           string
                           test_special
                           toggle_bool
                           write_string
                           tag_rw )

    set ( example_PROG_UTIL utils_posix.c )
    set ( example_LIBRARIES plctag pthread )

    # Logix simulator
    set ( lgx_sim_FILES "${test_SRC_PATH}/lgx_sim/log.h"
                        "${test_SRC_PATH}/lgx_sim/main.c"
                        "${test_SRC_PATH}/lgx_sim/packet.h"
                        "${test_SRC_PATH}/lgx_sim/main.c"
                        "${test_SRC_PATH}/lgx_sim/session.c"
                        "${test_SRC_PATH}/lgx_sim/session.h"
                        "${test_SRC_PATH}/lgx_sim/tags.c"
                        "${test_SRC_PATH}/lgx_sim/tags.h" )

    foreach ( file ${lgx_sim_FILES} )
        set_source_files_properties("${file}" PROPERTIES COMPILE_FLAGS "${BASE_FLAGS} -std=c11")
    endforeach ( file )

    add_executable ( lgx_sim ${lgx_sim_FILES} )
    target_link_libraries ( lgx_sim pthread )

elseif(WIN32)
    set ( example_PROGRAMS async
                           plc5
                           simple
                           simple_dual
                           slc500
                           string
                           test_special
                           toggle_bool
                           write_string
                           tag_rw )

    set ( example_PROG_UTIL utils_windows.c)
    set ( example_LIBRARIES plctag ws2_32 )
endif()

# simple.cpp is different because it is C++
set_source_files_properties("${example_SRC_PATH}/simple_cpp.cpp" PROPERTIES COMPILE_FLAGS ${BASE_CXX_FLAGS})
add_executable (simple_cpp "${example_SRC_PATH}/simple_cpp.cpp" "${example_SRC_PATH}/${example_PROG_UTIL}" "${example_SRC_PATH}/utils.h" )
target_link_libraries (simple_cpp ${example_LIBRARIES} )

foreach ( example ${example_PROGRAMS} )
    set_source_files_properties("${example_SRC_PATH}/${example}.c" PROPERTIES COMPILE_FLAGS ${BASE_C_FLAGS})
    add_executable( ${example} "${example_SRC_PATH}/${example}.c" "${example_SRC_PATH}/${example_PROG_UTIL}" "${example_SRC_PATH}/utils.h" )
    target_link_libraries(${example} ${example_LIBRARIES} )
endforeach(example)


# Generate files from templates
CONFIGURE_FILE("${CMAKE_CURRENT_SOURCE_DIR}/libplctag.pc.in" "${CMAKE_CURRENT_BINARY_DIR}/libplctag.pc" @ONLY)

# for installation
install(TARGETS plctag DESTINATION lib${LIB_SUFFIX})
install(FILES "${lib_SRC_PATH}/libplctag.h" DESTINATION include)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/libplctag.pc" DESTINATION "lib${LIB_SUFFIX}/pkgconfig")
