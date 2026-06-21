message("Building libdevsim library.")

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
#
# Layout parallels libplctag:
#   lib/        public header (device_sim.h) + the device_sim_t API impl
#   protocols/  EIP/CPF/CIP/PCCC, discovery, identity, TCP server
# ============================================================================

set(DEVSIM_CORE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/arena.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/arena.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/bytes.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/bytes.h
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/device_sim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/device_sim.h
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/device.h
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/cip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/cip.h
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/cpf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/cpf.h
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/discovery.c
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/discovery.h
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/eip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/eip.h
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/identity.h
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/pccc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/pccc.h
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/server.c
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/server.h
)

add_library(devsim STATIC ${DEVSIM_CORE_FILES})

# lib/ and protocols/ both on the path so the existing local "header.h" includes
# resolve across the two directories without touching any source.
target_include_directories(devsim PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils
)

target_link_libraries(devsim PUBLIC plctag_static ${EXTRA_LINKER_LIBS})

set_target_properties(devsim PROPERTIES OUTPUT_NAME "devsim")

# make the public header available in the output directory, like libplctag.h
CONFIGURE_FILE("${CMAKE_CURRENT_SOURCE_DIR}/lib/device_sim.h" "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/device_sim.h" COPYONLY)

install(TARGETS devsim
    ARCHIVE DESTINATION lib${LIB_SUFFIX}
    COMPONENT devel)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/lib/device_sim.h"
    DESTINATION include
    COMPONENT devel)
