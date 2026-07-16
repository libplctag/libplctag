message("Building omron_udt_walk test (OMRON @udt member-sibling-chain walk).")

add_executable(omron_udt_walk ${CMAKE_CURRENT_SOURCE_DIR}/omron_udt_walk.c)

target_include_directories(omron_udt_walk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/lib)

target_link_libraries(omron_udt_walk PRIVATE plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(omron_udt_walk PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
