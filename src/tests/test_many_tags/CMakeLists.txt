message("Building test_many_tags.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_MANY_TAGS_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_many_tags.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_many_tags ${TEST_MANY_TAGS_FILES})

target_include_directories(test_many_tags PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_many_tags PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_many_tags PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
