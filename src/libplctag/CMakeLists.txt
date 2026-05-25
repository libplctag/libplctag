message("Building libplctag library.")

# generate version file from CMake info.
CONFIGURE_FILE("${CMAKE_CURRENT_SOURCE_DIR}/lib/version.h.in" "${CMAKE_CURRENT_SOURCE_DIR}/lib/version.h" @ONLY)

# Parse libplctag.h to generate internal debug constants
# This creates add_custom_command for debug_generated.h and debug_module_names.c
# The .c file is included in libplctag_SRCS below, so CMake tracks the dependency automatically
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ParseLibplctagHeader.cmake")
parse_libplctag_header(
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/libplctag.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../utils/debug_generated.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../utils/debug_module_names.c"
)
# DEBUG_GENERATED_NAMES_C is now set and used in libplctag_SRCS

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
    set(EXTRA_LINK_FLAGS "${EXTRA_LINK_FLAGS} ${EXTRA_LINK_FLAGS_DEBUG}")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${EXTRA_LINK_FLAGS_DEBUG}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${EXTRA_LINK_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
    set(EXTRA_LINK_FLAGS "${EXTRA_LINK_FLAGS} ${EXTRA_LINK_FLAGS_MINSIZEREL}")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${EXTRA_LINK_FLAGS_MINSIZEREL}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${EXTRA_LINK_FLAGS_MINSIZEREL}")
endif()

# message("CMAKE_C_FLAGS = ${CMAKE_C_FLAGS}")

message("Adding AB protocol sources to build.")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab")

message("Adding ENIP protocol sources to build.")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/protocols/enip")

# set up the library sources
set ( libplctag_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/lib/init.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/lib/init.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/lib/libplctag.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/lib/lib.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/lib/tag.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/lib/version.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/lib/version.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/omron/omron.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/omron/omron_common.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/omron/omron_common.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/omron/omron_connection_tag.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/omron/omron_connection_tag.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/omron/cip.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/omron/cip.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/omron/conn.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/omron/conn.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/omron/defs.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/omron/omron_raw_tag.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/omron/omron_raw_tag.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/omron/omron_standard_tag.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/omron/omron_standard_tag.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/omron/tag.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/mb/modbus.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/mb/modbus.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/system/system.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/system/system.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/system/tag.h"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/atomic_utils.c"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/atomic_utils.h"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/attr.c"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/attr.h"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/byteorder.h"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/debug.c"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/debug.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/../utils/debug_module_names.c"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/hash.c"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/hash.h"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/hashtable.c"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/hashtable.h"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/enip_wait.c"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/enip_wait.h"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/arena.c"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/arena.h"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/bytes.c"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/bytes.h"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/macros.h"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/random_utils.c"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/random_utils.h"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/rc.c"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/rc.h"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/vector.c"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/vector.h"
                     "${PLATFORM_SHIM_PATH}/platform.c"
                     "${PLATFORM_SHIM_PATH}/platform.h" )

list(APPEND libplctag_SRCS "${AB_PROTOCOL_SOURCES}")
list(APPEND libplctag_SRCS "${ENIP_PROTOCOL_SOURCES}")


macro(print_all_ar_link_variables)
    message(STATUS "print_all_ar_link_variables------------------------------------------{")
    get_cmake_property(_variableNames VARIABLES)
    foreach (_variableName ${_variableNames})
        string(REGEX MATCH "^CMAKE_AR.*" MATCHED_STR "${_variableName}")
        if(MATCHED_STR)
            message(STATUS "${_variableName}=${${_variableName}}")
        endif()
            
        string(REGEX MATCH ".*LINK.*" MATCHED_STR "${_variableName}")
        if(MATCHED_STR)
            message(STATUS "${_variableName}=${${_variableName}}")
        endif()
    endforeach()
    message(STATUS "print_all_ar_link_variables------------------------------------------}")
endmacro()

# Debugging
# print_all_ar_link_variables()

# shared library
# Note: No add_dependencies needed - DEBUG_GENERATED_NAMES_C in sources triggers generation automatically
add_library(plctag_dyn SHARED ${libplctag_SRCS} )
target_link_libraries(plctag_dyn ${EXTRA_LINKER_LIBS})
# Link atomic library for 64-bit atomic operations on 32-bit ARM
# Link math library (main library may use sqrt in some places)
if(UNIX AND NOT APPLE)
    target_link_libraries(plctag_dyn atomic)
    target_link_libraries(plctag_dyn m)
elseif(UNIX)
    # macOS: link math library
    target_link_libraries(plctag_dyn m)
endif()
set_target_properties(plctag_dyn PROPERTIES
    VERSION "${libplctag_VERSION_MAJOR}.${libplctag_VERSION_MINOR}.${libplctag_VERSION_PATCH}"
    SOVERSION "${libplctag_VERSION_MAJOR}.${libplctag_VERSION_MINOR}"
    OUTPUT_NAME "plctag"
)

# if(EXTRA_LINK_FLAGS)
#     target_link_options(plctag_dyn PUBLIC "${EXTRA_LINK_FLAGS}")
# endif()

if(PLATFORM_WINDOWS)
    target_compile_definitions(plctag_dyn PUBLIC -D_CRT_SECURE_NO_WARNINGS)
endif(PLATFORM_WINDOWS)

if(LIBPLCTAG_ENABLE_COVERAGE)
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(plctag_dyn PRIVATE --coverage)
        target_link_options(plctag_dyn PUBLIC --coverage)
    endif()
endif()

install(TARGETS plctag_dyn
    LIBRARY DESTINATION lib${LIB_SUFFIX}
    COMPONENT runtime)

# static library
# Note: No add_dependencies needed - DEBUG_GENERATED_NAMES_C in sources triggers generation automatically
add_library(plctag_static STATIC ${libplctag_SRCS} )
target_link_libraries(plctag_static ${EXTRA_LINKER_LIBS})
# Link atomic library for 64-bit atomic operations on 32-bit ARM
# Link math library (main library may use sqrt in some places)
if(UNIX AND NOT APPLE)
    target_link_libraries(plctag_static atomic)
    target_link_libraries(plctag_static m)
elseif(UNIX)
    # macOS: link math library
    target_link_libraries(plctag_static m)
endif()
set_target_properties(plctag_static PROPERTIES SOVERSION "${libplctag_VERSION_MAJOR}.${libplctag_VERSION_MINOR}" OUTPUT_NAME "plctag_static")
target_compile_definitions(plctag_static PUBLIC -DLIBPLCTAG_STATIC=1)

# if(STATIC_LINK_OPTIONS)
#     target_link_options(plctag_static PUBLIC "${EXTRA_LINK_FLAGS} ${STATIC_LINK_OPTIONS}")
# endif()

if(PLATFORM_WINDOWS)
    target_compile_definitions(plctag_static PUBLIC -D_CRT_SECURE_NO_WARNINGS)
endif(PLATFORM_WINDOWS)

if(LIBPLCTAG_ENABLE_COVERAGE)
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(plctag_static PRIVATE --coverage)
        target_link_options(plctag_static PUBLIC --coverage)
    endif()
endif()

# make sure the .h file is in the output directory
CONFIGURE_FILE("${CMAKE_CURRENT_SOURCE_DIR}/lib/libplctag.h" "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libplctag.h" COPYONLY)

install(TARGETS plctag_static
    ARCHIVE DESTINATION lib${LIB_SUFFIX}
    COMPONENT devel)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/lib/libplctag.h"
    DESTINATION include
    COMPONENT devel)

if(EXISTS "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libplctag.pc")
    install(FILES "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libplctag.pc"
            DESTINATION "lib${LIB_SUFFIX}/pkgconfig"
            COMPONENT devel)
endif()
