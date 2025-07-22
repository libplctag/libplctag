message("Building libplctag library.")

# generate version file from CMake info.
CONFIGURE_FILE("${CMAKE_CURRENT_SOURCE_DIR}/lib/version.h.in" "${CMAKE_CURRENT_SOURCE_DIR}/lib/version.h" @ONLY)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
    set(EXTRA_LINK_FLAGS "${EXTRA_LINK_FLAGS} ${EXTRA_LINK_FLAGS_DEBUG}")
    set(CMAKE_C_LINK_FLAGS "${CMAKE_C_LINK_FLAGS} ${EXTRA_LINK_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
    set(EXTRA_LINK_FLAGS "${EXTRA_LINK_FLAGS} ${EXTRA_LINK_FLAGS_MINSIZEREL}")
    set(CMAKE_C_LINK_FLAGS "${CMAKE_C_LINK_FLAGS} ${EXTRA_LINK_FLAGS_MINSIZEREL}")
endif()

# message("CMAKE_C_FLAGS = ${CMAKE_C_FLAGS}")


# set up the library sources
set ( libplctag_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/lib/init.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/lib/init.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/lib/libplctag.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/lib/lib.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/lib/tag.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/lib/version.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/lib/version.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/ab.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/ab_common.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/ab_common.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/cip.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/cip.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/defs.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/eip_cip.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/eip_cip.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/eip_cip_special.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/eip_cip_special.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/eip_lgx_pccc.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/eip_lgx_pccc.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/eip_plc5_dhp.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/eip_plc5_dhp.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/eip_plc5_pccc.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/eip_plc5_pccc.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/eip_slc_dhp.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/eip_slc_dhp.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/eip_slc_pccc.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/eip_slc_pccc.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/error_codes.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/error_codes.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/pccc.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/pccc.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/session.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/session.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ab/tag.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/omron/omron.h"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/omron/omron_common.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/protocols/omron/omron_common.h"
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
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/hash.c"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/hash.h"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/hashtable.c"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/hashtable.h"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/macros.h"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/random_utils.c"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/random_utils.h"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/rc.c"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/rc.h"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/vector.c"
                     "${CMAKE_CURRENT_LIST_DIR}/../utils/vector.h"
                     "${PLATFORM_SHIM_PATH}/platform.c"
                     "${PLATFORM_SHIM_PATH}/platform.h" )


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
add_library(plctag_dyn SHARED ${libplctag_SRCS} )
target_link_libraries(plctag_dyn ${EXTRA_LINKER_LIBS})
set_target_properties(plctag_dyn PROPERTIES SOVERSION "${libplctag_VERSION_MAJOR}.${libplctag_VERSION_MINOR}" OUTPUT_NAME "plctag")

# if(EXTRA_LINK_FLAGS)
#     target_link_options(plctag_dyn PUBLIC "${EXTRA_LINK_FLAGS}")
# endif()

if(PLATFORM_WINDOWS)
    target_compile_definitions(plctag_dyn PUBLIC -D_CRT_SECURE_NO_WARNINGS)
endif(PLATFORM_WINDOWS)

install(TARGETS plctag_dyn DESTINATION lib${LIB_SUFFIX})

# static library
add_library(plctag_static STATIC ${libplctag_SRCS} )
target_link_libraries(plctag_static ${EXTRA_LINKER_LIBS})
set_target_properties(plctag_static PROPERTIES SOVERSION "${libplctag_VERSION_MAJOR}.${libplctag_VERSION_MINOR}" OUTPUT_NAME "plctag_static")
target_compile_definitions(plctag_static PUBLIC -DLIBPLCTAG_STATIC=1)

# if(STATIC_LINK_OPTIONS)
#     target_link_options(plctag_static PUBLIC "${EXTRA_LINK_FLAGS} ${STATIC_LINK_OPTIONS}")
# endif()

if(PLATFORM_WINDOWS)
    target_compile_definitions(plctag_static PUBLIC -D_CRT_SECURE_NO_WARNINGS)
endif(PLATFORM_WINDOWS)

# make sure the .h file is in the output directory
CONFIGURE_FILE("${CMAKE_CURRENT_SOURCE_DIR}/lib/libplctag.h" "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libplctag.h" COPYONLY)

install(TARGETS plctag_static DESTINATION lib${LIB_SUFFIX})

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/lib/libplctag.h" DESTINATION include)

if(EXISTS "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libplctag.pc")
    install(FILES "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libplctag.pc" DESTINATION "lib${LIB_SUFFIX}/pkgconfig")
endif()
