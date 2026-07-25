message("Building omron_aphyt_metadata test (OMRON aphyt-compat kind filter + symbolic metadata).")

add_executable(omron_aphyt_metadata ${CMAKE_CURRENT_SOURCE_DIR}/omron_aphyt_metadata.c)

target_include_directories(omron_aphyt_metadata PRIVATE
    ${CMAKE_BINARY_DIR}/generated/libplctag/lib)

target_link_libraries(omron_aphyt_metadata PRIVATE plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(omron_aphyt_metadata PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
