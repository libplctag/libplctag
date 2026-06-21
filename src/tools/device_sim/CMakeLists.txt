message("Building device_sim CLI tool.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

# ============================================================================
# device_sim — CLI executable; thin wrapper over libdevsim (built in
# src/libdevsim).  The devsim target exports its lib/ and protocols/ include
# dirs PUBLICly, so no include paths are needed here.
# ============================================================================

set(DEVICE_SIM_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/args.c
    ${CMAKE_CURRENT_SOURCE_DIR}/args.h
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
)

add_executable(device_sim ${DEVICE_SIM_FILES})

target_link_libraries(device_sim PRIVATE devsim)
target_link_options(device_sim PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
