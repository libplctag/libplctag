message("Building test_callback_destroy.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_CALLBACK_DESTROY_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_callback_destroy.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_callback_destroy ${TEST_CALLBACK_DESTROY_FILES})

target_include_directories(test_callback_destroy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_callback_destroy PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_callback_destroy PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
