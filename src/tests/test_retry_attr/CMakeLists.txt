message("Building test_retry_attr.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_RETRY_ATTR_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_retry_attr.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_retry_attr ${TEST_RETRY_ATTR_FILES})

target_include_directories(test_retry_attr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_retry_attr PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_retry_attr PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
