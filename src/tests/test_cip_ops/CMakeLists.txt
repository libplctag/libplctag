message("Building test_cip_ops.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_CIP_OPS_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_cip_ops.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_cip_ops ${TEST_CIP_OPS_FILES})

target_include_directories(test_cip_ops PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_cip_ops PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_cip_ops PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
