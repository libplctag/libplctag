# Define the utility source file and libraries based on the OS

# compile the compatibility code separately.
add_library(compat_utils OBJECT "${CMAKE_SOURCE_DIR}/src/examples/compat_utils.c" "${CMAKE_SOURCE_DIR}/src/examples/compat_utils.h")



if (UNIX)
    set(example_LIBRARIES compat_utils ${tool_lib} pthread)
elseif (WIN32)
    set(example_LIBRARIES compat_utils ${tool_lib} ws2_32)
elseif()
    message("Unsupported platform!")
    return()
endif()

message("BASE_C_FLAGS = ${BASE_C_FLAGS}")
message("CMAKE_BUILD_TYPE = ${CMAKE_BUILD_TYPE}")


# Set the compile flags for the utilities file.
#set_source_files_properties("${CMAKE_SOURCE_DIR}/src/examples/${example_PROG_UTIL}" PROPERTIES COMPILE_FLAGS "${BASE_C_FLAGS}")

# Common source files and header
#set(EXAMPLE_COMMON_SOURCES "${CMAKE_SOURCE_DIR}/src/examples/${example_PROG_UTIL}" "${CMAKE_SOURCE_DIR}/src/examples/compat_utils.h")

# Set the compile and link properties for all examples.
message("Building examples.")
message("CMAKE_C_FLAGS_DEBUG = ${CMAKE_C_FLAGS_DEBUG}")
message("CMAKE_C_FLAGS_RELEASE = ${CMAKE_C_FLAGS_RELEASE}")
message("BASE_C_FLAGS = ${BASE_C_FLAGS}")

# Define each executable as an independent target
add_executable(async "${CMAKE_SOURCE_DIR}/src/examples/async.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(async
  PRIVATE
    $<$<CONFIG:Debug>:${COMMON_COMPILE_FLAGS} ${BASE_DEBUG_FLAGS}> # Debug flags
    $<$<NOT:$<CONFIG:Debug>>:${COMMON_COMPILE_FLAGS} ${BASE_RELEASE_FLAGS}> # Release flags
)
target_link_libraries(async ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(async PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(async_stress "${CMAKE_SOURCE_DIR}/src/examples/async_stress.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(async_stress
  PRIVATE
    $<$<CONFIG:Debug>:${COMMON_COMPILE_FLAGS} ${BASE_DEBUG_FLAGS}> # Debug flags
    $<$<NOT:$<CONFIG:Debug>>:${COMMON_COMPILE_FLAGS} ${BASE_RELEASE_FLAGS}> # Release flags
)
target_link_libraries(async_stress ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(async_stress PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(barcode_test "${CMAKE_SOURCE_DIR}/src/examples/barcode_test.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(barcode_test
  PRIVATE
    $<$<CONFIG:Debug>:${COMMON_COMPILE_FLAGS} ${BASE_DEBUG_FLAGS}> # Debug flags
    $<$<NOT:$<CONFIG:Debug>>:${COMMON_COMPILE_FLAGS} ${BASE_RELEASE_FLAGS}> # Release flags
)
target_link_libraries(barcode_test ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(barcode_test PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(busy_test "${CMAKE_SOURCE_DIR}/src/examples/busy_test.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(busy_test
  PRIVATE
    $<$<CONFIG:Debug>:${COMMON_COMPILE_FLAGS} ${BASE_DEBUG_FLAGS}> # Debug flags
    $<$<NOT:$<CONFIG:Debug>>:${COMMON_COMPILE_FLAGS} ${BASE_RELEASE_FLAGS}> # Release flags
)
target_link_libraries(busy_test ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(busy_test PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(data_dumper "${CMAKE_SOURCE_DIR}/src/examples/data_dumper.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(data_dumper
  PRIVATE
    $<$<CONFIG:Debug>:${COMMON_COMPILE_FLAGS} ${BASE_DEBUG_FLAGS}> # Debug flags
    $<$<NOT:$<CONFIG:Debug>>:${COMMON_COMPILE_FLAGS} ${BASE_RELEASE_FLAGS}> # Release flags
)
target_link_libraries(data_dumper ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(data_dumper PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(list_tags_logix "${CMAKE_SOURCE_DIR}/src/examples/list_tags_logix.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(list_tags_logix
  PRIVATE
    $<$<CONFIG:Debug>:${COMMON_COMPILE_FLAGS} ${BASE_DEBUG_FLAGS}> # Debug flags
    $<$<NOT:$<CONFIG:Debug>>:${COMMON_COMPILE_FLAGS} ${BASE_RELEASE_FLAGS}> # Release flags
)
target_link_libraries(list_tags_logix ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(list_tags_logix PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(list_tags_micro8x0 "${CMAKE_SOURCE_DIR}/src/examples/list_tags_micro8x0.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(list_tags_micro8x0
  PRIVATE
    $<$<CONFIG:Debug>:${COMMON_COMPILE_FLAGS} ${BASE_DEBUG_FLAGS}> # Debug flags
    $<$<NOT:$<CONFIG:Debug>>:${COMMON_COMPILE_FLAGS} ${BASE_RELEASE_FLAGS}> # Release flags
)
target_link_libraries(list_tags_micro8x0 ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(list_tags_micro8x0 PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(list_tags_omron_incomplete "${CMAKE_SOURCE_DIR}/src/examples/list_tags_omron_incomplete.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(list_tags_omron_incomplete
  PRIVATE
    $<$<CONFIG:Debug>:${COMMON_COMPILE_FLAGS} ${BASE_DEBUG_FLAGS}> # Debug flags
    $<$<NOT:$<CONFIG:Debug>>:${COMMON_COMPILE_FLAGS} ${BASE_RELEASE_FLAGS}> # Release flags
)
target_link_libraries(list_tags_omron_incomplete ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(list_tags_omron_incomplete PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(multithread "${CMAKE_SOURCE_DIR}/src/examples/multithread.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(multithread
  PRIVATE
    $<$<CONFIG:Debug>:${COMMON_COMPILE_FLAGS} ${BASE_DEBUG_FLAGS}> # Debug flags
    $<$<NOT:$<CONFIG:Debug>>:${COMMON_COMPILE_FLAGS} ${BASE_RELEASE_FLAGS}> # Release flags
)
target_link_libraries(multithread ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(multithread PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(multithread_cached_read "${CMAKE_SOURCE_DIR}/src/examples/multithread_cached_read.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(multithread_cached_read
  PRIVATE
    $<$<CONFIG:Debug>:${COMMON_COMPILE_FLAGS} ${BASE_DEBUG_FLAGS}> # Debug flags
    $<$<NOT:$<CONFIG:Debug>>:${COMMON_COMPILE_FLAGS} ${BASE_RELEASE_FLAGS}> # Release flags
)
target_link_libraries(multithread_cached_read ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(multithread_cached_read PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(multithread_plc5 "${CMAKE_SOURCE_DIR}/src/examples/multithread_plc5.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(multithread_plc5
  PRIVATE
    $<$<CONFIG:Debug>:${COMMON_COMPILE_FLAGS} ${BASE_DEBUG_FLAGS}> # Debug flags
    $<$<NOT:$<CONFIG:Debug>>:${COMMON_COMPILE_FLAGS} ${BASE_RELEASE_FLAGS}> # Release flags
)
target_link_libraries(multithread_plc5 ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(multithread_plc5 PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(multithread_plc5_dhp "${CMAKE_SOURCE_DIR}/src/examples/multithread_plc5_dhp.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(multithread_plc5_dhp
  PRIVATE
    $<$<CONFIG:Debug>:${COMMON_COMPILE_FLAGS} ${BASE_DEBUG_FLAGS}> # Debug flags
    $<$<NOT:$<CONFIG:Debug>>:${COMMON_COMPILE_FLAGS} ${BASE_RELEASE_FLAGS}> # Release flags
)
target_link_libraries(multithread_plc5_dhp ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(multithread_plc5_dhp PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(plc5 "${CMAKE_SOURCE_DIR}/src/examples/plc5.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(plc5
  PRIVATE
    $<$<CONFIG:Debug>:${COMMON_COMPILE_FLAGS} ${BASE_DEBUG_FLAGS}> # Debug flags
    $<$<NOT:$<CONFIG:Debug>>:${COMMON_COMPILE_FLAGS} ${BASE_RELEASE_FLAGS}> # Release flags
)
target_link_libraries(plc5 ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(plc5 PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(simple "${CMAKE_SOURCE_DIR}/src/examples/simple.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(simple
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(simple ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(simple PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(simple_dual "${CMAKE_SOURCE_DIR}/src/examples/simple_dual.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(simple_dual
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(simple_dual ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(simple_dual PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(slc500 "${CMAKE_SOURCE_DIR}/src/examples/slc500.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(slc500
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(slc500 ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(slc500 PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(stress_api_lock "${CMAKE_SOURCE_DIR}/src/examples/stress_api_lock.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(stress_api_lock
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(stress_api_lock ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(stress_api_lock PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(stress_rc_mem "${CMAKE_SOURCE_DIR}/src/examples/stress_rc_mem.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(stress_rc_mem
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(stress_rc_mem ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(stress_rc_mem PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(stress_test "${CMAKE_SOURCE_DIR}/src/examples/stress_test.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(stress_test
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(stress_test ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(stress_test PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(string_non_standard_udt "${CMAKE_SOURCE_DIR}/src/examples/string_non_standard_udt.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(string_non_standard_udt
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(string_non_standard_udt ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(string_non_standard_udt PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(string_standard "${CMAKE_SOURCE_DIR}/src/examples/string_standard.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(string_standard
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(string_standard ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(string_standard PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(test_alternate_tag_listing "${CMAKE_SOURCE_DIR}/src/examples/test_alternate_tag_listing.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(test_alternate_tag_listing
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(test_alternate_tag_listing ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(test_alternate_tag_listing PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(test_array_notation "${CMAKE_SOURCE_DIR}/src/examples/test_array_notation.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(test_array_notation
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(test_array_notation ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(test_array_notation PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(test_auto_sync "${CMAKE_SOURCE_DIR}/src/examples/test_auto_sync.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(test_auto_sync
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(test_auto_sync ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(test_auto_sync PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(test_callback "${CMAKE_SOURCE_DIR}/src/examples/test_callback.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(test_callback
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(test_callback ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(test_callback PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(test_callback_ex "${CMAKE_SOURCE_DIR}/src/examples/test_callback_ex.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(test_callback_ex
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(test_callback_ex ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(test_callback_ex PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(test_callback_ex_logix "${CMAKE_SOURCE_DIR}/src/examples/test_callback_ex_logix.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(test_callback_ex_logix
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(test_callback_ex_logix ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(test_callback_ex_logix PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(test_callback_ex_modbus "${CMAKE_SOURCE_DIR}/src/examples/test_callback_ex_modbus.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(test_callback_ex_modbus
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(test_callback_ex_modbus ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(test_callback_ex_modbus PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(test_connection_group "${CMAKE_SOURCE_DIR}/src/examples/test_connection_group.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(test_connection_group
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(test_connection_group ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(test_connection_group PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(test_emulator_performance "${CMAKE_SOURCE_DIR}/src/examples/test_emulator_performance.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(test_emulator_performance
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(test_emulator_performance ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(test_emulator_performance PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(test_event "${CMAKE_SOURCE_DIR}/src/examples/test_event.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(test_event
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(test_event ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(test_event PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(test_indexed_tags "${CMAKE_SOURCE_DIR}/src/examples/test_indexed_tags.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(test_indexed_tags
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(test_indexed_tags ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(test_indexed_tags PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(test_raw_cip "${CMAKE_SOURCE_DIR}/src/examples/test_raw_cip.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(test_raw_cip
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(test_raw_cip ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(test_raw_cip PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(test_reconnect "${CMAKE_SOURCE_DIR}/src/examples/test_reconnect.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(test_reconnect
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(test_reconnect ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(test_reconnect PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(test_shutdown "${CMAKE_SOURCE_DIR}/src/examples/test_shutdown.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(test_shutdown
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(test_shutdown ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(test_shutdown PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(test_special "${CMAKE_SOURCE_DIR}/src/examples/test_special.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(test_special
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(test_special ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(test_special PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(test_string "${CMAKE_SOURCE_DIR}/src/examples/test_string.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(test_string
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(test_string ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(test_string PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(test_tag_attributes "${CMAKE_SOURCE_DIR}/src/examples/test_tag_attributes.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(test_tag_attributes
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(test_tag_attributes ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(test_tag_attributes PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(test_tag_type_attribute "${CMAKE_SOURCE_DIR}/src/examples/test_tag_type_attribute.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(test_tag_type_attribute
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(test_tag_type_attribute ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(test_tag_type_attribute PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(thread_stress "${CMAKE_SOURCE_DIR}/src/examples/thread_stress.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(thread_stress
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(thread_stress ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(thread_stress PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(toggle_bit "${CMAKE_SOURCE_DIR}/src/examples/toggle_bit.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(toggle_bit
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(toggle_bit ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(toggle_bit PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(toggle_bool "${CMAKE_SOURCE_DIR}/src/examples/toggle_bool.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(toggle_bool
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(toggle_bool ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(toggle_bool PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(trigger_double_free "${CMAKE_SOURCE_DIR}/src/examples/trigger_double_free.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(trigger_double_free
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(trigger_double_free ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(trigger_double_free PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(write_string "${CMAKE_SOURCE_DIR}/src/examples/write_string.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(write_string
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(write_string ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(write_string PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(tag_rw_deprecated "${CMAKE_SOURCE_DIR}/src/examples/tag_rw_deprecated.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(tag_rw_deprecated
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(tag_rw_deprecated ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(tag_rw_deprecated PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

add_executable(tag_rw2 "${CMAKE_SOURCE_DIR}/src/examples/tag_rw2.c" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(tag_rw2
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_RELEASE_FLAGS}>
)
target_link_libraries(tag_rw2 ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    set_target_properties(tag_rw2 PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()

# simple.cpp is different because it is C++
add_executable(simple_cpp "${CMAKE_SOURCE_DIR}/src/examples/simple_cpp.cpp" ${EXAMPLE_COMMON_SOURCES})
target_compile_options(tag_rw2
  PRIVATE
    $<$<CONFIG:Debug>:${BASE_CXX_DEBUG_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:${BASE_CXX_RELEASE_FLAGS}>
)
target_link_libraries(simple_cpp ${example_LIBRARIES})
if(BASE_LINK_FLAGS)
    message("BASE_LINK_FLAGS=${BASE_LINK_FLAGS}")
    set_target_properties(simple_cpp PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
endif()
