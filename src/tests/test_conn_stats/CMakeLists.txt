message("Building test_conn_stats.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_CONN_STATS_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_conn_stats.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_conn_stats ${TEST_CONN_STATS_FILES})

target_include_directories(test_conn_stats PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_conn_stats PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_conn_stats PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
