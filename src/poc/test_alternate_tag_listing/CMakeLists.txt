message("Building test_alternate_tag_listing POC.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_ALTERNATE_TAG_LISTING_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_alternate_tag_listing.c
)

add_executable(test_alternate_tag_listing ${TEST_ALTERNATE_TAG_LISTING_FILES})


target_link_libraries(test_alternate_tag_listing PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_alternate_tag_listing PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
