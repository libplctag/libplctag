message("Building tag_rw2 tool.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TAG_RW2_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/tag_rw2.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(tag_rw2 ${TAG_RW2_FILES})

target_include_directories(tag_rw2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(tag_rw2 PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(tag_rw2 PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})

# Install tag_rw2 tool into the devel component
install(TARGETS tag_rw2
    RUNTIME DESTINATION bin
    COMPONENT devel)
