message("Building devsim_with_plctag test.")

add_executable(devsim_with_plctag ${CMAKE_CURRENT_SOURCE_DIR}/devsim_with_plctag.c)

target_include_directories(devsim_with_plctag PRIVATE
    ${CMAKE_BINARY_DIR}/generated/libplctag/lib)

# The device_sim_* and plc_tag_* APIs are both compiled into plctag_static
# (server-tag sources under LIBPLCTAG_FEATURE_SERVER), so linking it alone
# reaches both from one executable.
target_link_libraries(devsim_with_plctag PRIVATE plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(devsim_with_plctag PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
