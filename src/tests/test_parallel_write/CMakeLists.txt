message("Building test_parallel_write.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_PARALLEL_WRITE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_parallel_write.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_parallel_write ${TEST_PARALLEL_WRITE_FILES})

target_include_directories(test_parallel_write PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_parallel_write PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_parallel_write PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
