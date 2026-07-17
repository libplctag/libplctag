message("Building server_udt test (role=server udt=/elem_type=@name attributes).")

add_executable(server_udt ${CMAKE_CURRENT_SOURCE_DIR}/server_udt.c)

target_include_directories(server_udt PRIVATE
    ${CMAKE_BINARY_DIR}/generated/libplctag/lib)

target_link_libraries(server_udt PRIVATE plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(server_udt PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
