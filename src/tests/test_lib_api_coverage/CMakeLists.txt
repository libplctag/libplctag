message("Building test_lib_api_coverage.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_LIB_API_COVERAGE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_lib_api_coverage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_lib_api_coverage ${TEST_LIB_API_COVERAGE_FILES})

target_include_directories(test_lib_api_coverage PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_lib_api_coverage PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_lib_api_coverage PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
if(UNIX)
    target_link_libraries(test_lib_api_coverage PUBLIC m)
endif()
