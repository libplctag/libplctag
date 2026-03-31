message("Building test_device_tag.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_DEVICE_TAG_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_device_tag.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_device_tag ${TEST_DEVICE_TAG_FILES})

target_include_directories(test_device_tag PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_device_tag PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_device_tag PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
