message("Building devsim_with_plctag POC.")

add_executable(devsim_with_plctag ${CMAKE_CURRENT_SOURCE_DIR}/devsim_with_plctag.c)

target_include_directories(devsim_with_plctag PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/lib)

# libdevsim bundles plctag_static, so linking it reaches both the device_sim
# and the plc_tag public APIs from one executable.
target_link_libraries(devsim_with_plctag PRIVATE devsim ${EXTRA_LINKER_LIBS})
target_link_options(devsim_with_plctag PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
