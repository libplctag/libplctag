message("Building test_auto_sync_param.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_AUTO_SYNC_PARAM_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_auto_sync_param.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_auto_sync_param ${TEST_AUTO_SYNC_PARAM_FILES})

target_include_directories(test_auto_sync_param PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_auto_sync_param PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_auto_sync_param PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
