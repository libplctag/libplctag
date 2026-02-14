message("Building test_reconnect_after_outage_sync.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_RECONNECT_AFTER_OUTAGE_SYNC_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_reconnect_after_outage_sync.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_reconnect_after_outage_sync ${TEST_RECONNECT_AFTER_OUTAGE_SYNC_FILES})

target_include_directories(test_reconnect_after_outage_sync PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_reconnect_after_outage_sync PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_reconnect_after_outage_sync PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
