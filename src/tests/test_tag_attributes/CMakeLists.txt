message("Building test_tag_attributes.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_TAG_ATTRIBUTES_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_tag_attributes.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_tag_attributes ${TEST_TAG_ATTRIBUTES_FILES})

target_include_directories(test_tag_attributes PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_tag_attributes PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_tag_attributes PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
