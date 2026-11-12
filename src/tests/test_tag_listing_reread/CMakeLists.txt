message("Building test_tag_listing_reread.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_TAG_LISTING_REREAD_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_tag_listing_reread.c
)

add_executable(test_tag_listing_reread ${TEST_TAG_LISTING_REREAD_FILES})


target_link_libraries(test_tag_listing_reread PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_tag_listing_reread PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
