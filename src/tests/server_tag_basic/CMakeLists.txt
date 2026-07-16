message("Building server_tag_basic test.")

add_executable(server_tag_basic ${CMAKE_CURRENT_SOURCE_DIR}/server_tag_basic.c)

target_include_directories(server_tag_basic PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/lib)

target_link_libraries(server_tag_basic PRIVATE plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(server_tag_basic PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
