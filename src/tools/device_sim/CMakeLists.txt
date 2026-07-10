message("Building device_sim CLI tool.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

# ============================================================================
# device_sim — CLI executable; thin wrapper over the server-tag sources that
# live in src/libplctag/protocols/enip/{common,server,dialects} and are
# compiled into plctag_static under LIBPLCTAG_FEATURE_SERVER (SERVER_TAGS.md).
# ============================================================================

set(DEVICE_SIM_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/args.c
    ${CMAKE_CURRENT_SOURCE_DIR}/args.h
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
)

add_executable(device_sim ${DEVICE_SIM_FILES})

target_link_libraries(device_sim PRIVATE plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(device_sim PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
