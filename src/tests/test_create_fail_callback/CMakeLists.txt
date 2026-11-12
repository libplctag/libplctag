message("Building test_create_fail_callback.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_CREATE_FAIL_CALLBACK_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_create_fail_callback.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_create_fail_callback ${TEST_CREATE_FAIL_CALLBACK_FILES})

target_include_directories(test_create_fail_callback PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_create_fail_callback PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_create_fail_callback PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
