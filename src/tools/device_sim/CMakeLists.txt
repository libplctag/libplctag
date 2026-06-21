message("Building libdevsim library and device_sim tool.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

# ============================================================================
# libdevsim — static library with all protocol and API logic.
# Top-level build product, parallel to libplctag.  Embedded programs link
# against it and include device_sim.h.
#
# Static only: it uses libplctag's INTERNAL symbols (platform, mem, socket,
# arena, bytes, debug), which the shared libplctag hides via -fvisibility=hidden.
# It therefore links plctag_static, and is itself distributed static.
# ============================================================================

set(DEVSIM_CORE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../utils/arena.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../utils/arena.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../utils/bytes.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../utils/bytes.h
    ${CMAKE_CURRENT_SOURCE_DIR}/device_sim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/device_sim.h
    ${CMAKE_CURRENT_SOURCE_DIR}/device.h
    ${CMAKE_CURRENT_SOURCE_DIR}/cip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cip.h
    ${CMAKE_CURRENT_SOURCE_DIR}/cpf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cpf.h
    ${CMAKE_CURRENT_SOURCE_DIR}/discovery.c
    ${CMAKE_CURRENT_SOURCE_DIR}/discovery.h
    ${CMAKE_CURRENT_SOURCE_DIR}/eip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/eip.h
    ${CMAKE_CURRENT_SOURCE_DIR}/identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/identity.h
    ${CMAKE_CURRENT_SOURCE_DIR}/pccc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/pccc.h
    ${CMAKE_CURRENT_SOURCE_DIR}/server.c
    ${CMAKE_CURRENT_SOURCE_DIR}/server.h
)

add_library(devsim STATIC ${DEVSIM_CORE_FILES})

target_include_directories(devsim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../utils
)

target_link_libraries(devsim PUBLIC plctag_static ${EXTRA_LINKER_LIBS})

set_target_properties(devsim PROPERTIES OUTPUT_NAME "devsim")

# make the public header available in the output directory, like libplctag.h
CONFIGURE_FILE("${CMAKE_CURRENT_SOURCE_DIR}/device_sim.h" "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/device_sim.h" COPYONLY)

install(TARGETS devsim
    ARCHIVE DESTINATION lib${LIB_SUFFIX}
    COMPONENT devel)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/device_sim.h"
    DESTINATION include
    COMPONENT devel)

# ============================================================================
# device_sim — CLI executable; thin wrapper over libdevsim.
# ============================================================================

set(DEVICE_SIM_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/args.c
    ${CMAKE_CURRENT_SOURCE_DIR}/args.h
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
)

add_executable(device_sim ${DEVICE_SIM_FILES})

target_include_directories(device_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../utils
)

target_link_libraries(device_sim PRIVATE devsim)
target_link_options(device_sim PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
