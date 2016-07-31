cmake_minimum_required (VERSION 2.6)

# this is the root libplctag project
project (libplctag_project)

# the project is version 2.0
set (libplctag_VERSION_MAJOR 2)
set (libplctag_VERSION_MINOR 0)


# OS-specific files for the platform code.
# FIXME - does this work for macOS?
if (UNIX)
    set ( platform_PATH "${PROJECT_SOURCE_DIR}/src/platform/linux" )
endif (UNIX)

if (MSVC)
    set ( platform_PATH "${PROJECT_SOURCE_DIR}/src/platform/windows" )
endif (MSVC)

# where to find include files.
include_directories("${PROJECT_SOURCE_DIR}/src/lib")
include_directories("${PROJECT_SOURCE_DIR}/src/lib/protocols/ab")
include_directories("${PROJECT_SOURCE_DIR}/src/lib/util")
include_directories("${platform_PATH}")

# set up the library sources
set ( libplctag_SRCS "${PROJECT_SOURCE_DIR}/src/lib/libplctag_tag.c"
                     "${PROJECT_SOURCE_DIR}/src/lib/protocols/ab/ab_common.c"
                     "${PROJECT_SOURCE_DIR}/src/lib/protocols/ab/cip.c"
                     "${PROJECT_SOURCE_DIR}/src/lib/protocols/ab/connection.c"
                     "${PROJECT_SOURCE_DIR}/src/lib/protocols/ab/eip.c"
                     "${PROJECT_SOURCE_DIR}/src/lib/protocols/ab/eip_cip.c"
                     "${PROJECT_SOURCE_DIR}/src/lib/protocols/ab/eip_dhp_pccc.c"
                     "${PROJECT_SOURCE_DIR}/src/lib/protocols/ab/eip_pccc.c"
                     "${PROJECT_SOURCE_DIR}/src/lib/protocols/ab/pccc.c"
                     "${PROJECT_SOURCE_DIR}/src/lib/protocols/ab/request.c"
                     "${PROJECT_SOURCE_DIR}/src/lib/protocols/ab/session.c"
                     "${PROJECT_SOURCE_DIR}/src/lib/util/attr.c"
                     "${platform_PATH}/platform.c" )

# set up the library
add_library(libplctag SHARED ${libplctag_SRCS} )



# add the examples
add_executable (async async.c)
target_link_libraries (async libplctag)

add_executable (data_dumper data_dumper.c)
target_link_libraries (data_dumper libplctag)

add_executable (multithread multithread.c)
target_link_libraries (multithread libplctag)

add_executable (multithread_cached_read multithread_cached_read.c)
target_link_libraries (multithread_cached_read libplctag)

add_executable (multithread_plc5 multithread_plc5.c)
target_link_libraries (multithread_plc5 libplctag)

add_executable (multithread_plc5_dhp multithread_plc5_dhp.c)
target_link_libraries (multithread_plc5_dhp libplctag)

add_executable (plc5 plc5.c)
target_link_libraries (plc5 libplctag)

add_executable (simple simple.c)
target_link_libraries (simple libplctag)

add_executable (simple_cpp simple.cpp)
target_link_libraries (simple_cpp libplctag)

add_executable (simple_dual simple_dual.c)
target_link_libraries (simple_dual libplctag)

add_executable (slc500 slc500.c)
target_link_libraries (slc500 libplctag)

add_executable (string string.c)
target_link_libraries (async libplctag)

add_executable (tag_rw tag_rw.c)
target_link_libraries (tag_rw libplctag)

add_executable (toggle_bool toggle_bool.c)
target_link_libraries (toggle_bool libplctag)

add_executable (write_string write_string.c)
target_link_libraries (write_string libplctag)




# for installation
install(TARGETS libplctag DESTINATION lib)
install(FILES "${PROJECT_SOURCE_DIR}/src/lib/libplctag.h" DESTINATION include)
