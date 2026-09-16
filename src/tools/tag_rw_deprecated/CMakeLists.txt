message("Building tag_rw_deprecated tool.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TAG_RW_DEPRECATED_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/tag_rw_deprecated.c
)

add_executable(tag_rw_deprecated ${TAG_RW_DEPRECATED_FILES})


target_link_libraries(tag_rw_deprecated PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(tag_rw_deprecated PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
