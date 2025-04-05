

# generate version file from CMake info.
CONFIGURE_FILE("${CMAKE_SOURCE_DIR}/src/libplctag/lib/version.h.in" "${CMAKE_SOURCE_DIR}/src/libplctag/lib/version.h" @ONLY)

# set up the library sources
set ( libplctag_SRCS "${CMAKE_SOURCE_DIR}/src/libplctag/lib/init.c"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/lib/init.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/lib/libplctag.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/lib/lib.c"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/lib/tag.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/lib/version.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/lib/version.c"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/ab.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/ab_common.c"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/ab_common.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/cip.c"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/cip.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/defs.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/eip_cip.c"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/eip_cip.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/eip_cip_special.c"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/eip_cip_special.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/eip_lgx_pccc.c"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/eip_lgx_pccc.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/eip_plc5_dhp.c"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/eip_plc5_dhp.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/eip_plc5_pccc.c"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/eip_plc5_pccc.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/eip_slc_dhp.c"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/eip_slc_dhp.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/eip_slc_pccc.c"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/eip_slc_pccc.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/error_codes.c"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/error_codes.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/pccc.c"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/pccc.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/session.c"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/session.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/ab/tag.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/omron/omron.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/omron/omron_common.c"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/omron/omron_common.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/omron/cip.c"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/omron/cip.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/omron/conn.c"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/omron/conn.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/omron/defs.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/omron/omron_raw_tag.c"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/omron/omron_raw_tag.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/omron/omron_standard_tag.c"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/omron/omron_standard_tag.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/omron/tag.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/mb/modbus.c"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/mb/modbus.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/system/system.c"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/system/system.h"
                     "${CMAKE_SOURCE_DIR}/src/libplctag/protocols/system/tag.h"
                     "${CMAKE_SOURCE_DIR}/src/utils/atomic_utils.c"
                     "${CMAKE_SOURCE_DIR}/src/utils/atomic_utils.h"
                     "${CMAKE_SOURCE_DIR}/src/utils/attr.c"
                     "${CMAKE_SOURCE_DIR}/src/utils/attr.h"
                     "${CMAKE_SOURCE_DIR}/src/utils/byteorder.h"
                     "${CMAKE_SOURCE_DIR}/src/utils/debug.c"
                     "${CMAKE_SOURCE_DIR}/src/utils/debug.h"
                     "${CMAKE_SOURCE_DIR}/src/utils/hash.c"
                     "${CMAKE_SOURCE_DIR}/src/utils/hash.h"
                     "${CMAKE_SOURCE_DIR}/src/utils/hashtable.c"
                     "${CMAKE_SOURCE_DIR}/src/utils/hashtable.h"
                     "${CMAKE_SOURCE_DIR}/src/utils/macros.h"
                     "${CMAKE_SOURCE_DIR}/src/utils/random_utils.c"
                     "${CMAKE_SOURCE_DIR}/src/utils/random_utils.h"
                     "${CMAKE_SOURCE_DIR}/src/utils/rc.c"
                     "${CMAKE_SOURCE_DIR}/src/utils/rc.h"
                     "${CMAKE_SOURCE_DIR}/src/utils/vector.c"
                     "${CMAKE_SOURCE_DIR}/src/utils/vector.h"
                     "${platform_SRC_PATH}/platform.c"
                     "${platform_SRC_PATH}/platform.h" )

# shared library
add_library(plctag_dyn SHARED ${libplctag_SRCS} )
target_link_libraries(plctag_dyn ${EXTRA_LINKER_LIBS})
set_target_properties(plctag_dyn PROPERTIES SOVERSION "${libplctag_VERSION_MAJOR}.${libplctag_VERSION_MINOR}" OUTPUT_NAME "plctag")

# install(TARGETS plctag_dyn DESTINATION lib${LIB_SUFFIX})

# static library
add_library(plctag_static STATIC ${libplctag_SRCS} )
target_link_libraries(plctag_static ${EXTRA_LINKER_LIBS})
set_target_properties(plctag_static PROPERTIES SOVERSION "${libplctag_VERSION_MAJOR}.${libplctag_VERSION_MINOR}" OUTPUT_NAME "plctag_static")
target_compile_definitions(plctag_static PUBLIC -DLIBPLCTAGDLL_EXPORTS=1)    

# make sure the .h file is in the output directory
CONFIGURE_FILE("${CMAKE_SOURCE_DIR}/src/libplctag/lib/libplctag.h" "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libplctag.h" COPYONLY)

# install(TARGETS plctag_static DESTINATION lib${LIB_SUFFIX})

install(FILES "${CMAKE_SOURCE_DIR}/src/libplctag/lib/libplctag.h" DESTINATION include)

if(EXISTS "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libplctag.pc")
    install(FILES "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libplctag.pc" DESTINATION "lib${LIB_SUFFIX}/pkgconfig")
endif()
