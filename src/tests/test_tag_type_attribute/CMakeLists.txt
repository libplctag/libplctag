message("Building test_tag_type_attribute.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_TAG_TYPE_ATTRIBUTE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_tag_type_attribute.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_tag_type_attribute ${TEST_TAG_TYPE_ATTRIBUTE_FILES})

target_include_directories(test_tag_type_attribute PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_tag_type_attribute PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_tag_type_attribute PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
