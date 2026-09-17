message("Building probe_lgx_pccc_write_first.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

add_executable(probe_lgx_pccc_write_first ${CMAKE_CURRENT_SOURCE_DIR}/probe_lgx_pccc_write_first.c)

target_link_libraries(probe_lgx_pccc_write_first PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(probe_lgx_pccc_write_first PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
