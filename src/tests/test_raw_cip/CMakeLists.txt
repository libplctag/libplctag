message("Building test_raw_cip.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_RAW_CIP_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_raw_cip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_raw_cip ${TEST_RAW_CIP_FILES})

target_include_directories(test_raw_cip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_raw_cip PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_raw_cip PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
