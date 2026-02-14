message("Building list_tags_omron_incomplete tool.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(LIST_TAGS_OMRON_INCOMPLETE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/list_tags_omron_incomplete.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(list_tags_omron_incomplete ${LIST_TAGS_OMRON_INCOMPLETE_FILES})

target_include_directories(list_tags_omron_incomplete PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(list_tags_omron_incomplete PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(list_tags_omron_incomplete PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
