message("Building test_conn_settings.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_CONN_SETTINGS_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_conn_settings.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_conn_settings ${TEST_CONN_SETTINGS_FILES})

target_include_directories(test_conn_settings PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_conn_settings PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_conn_settings PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
