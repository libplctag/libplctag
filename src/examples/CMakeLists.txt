message("Building examples.")

set(EXAMPLE_EXECUTABLES 
  async
  async_stress
  barcode_test
  busy_test
  data_dumper
  list_tags_logix
  list_tags_micro8x0
  list_tags_omron_incomplete
  multithread
  multithread_cached_read
  multithread_plc5
  multithread_plc5_dhp
  plc5
  simple
  simple_cpp
  simple_dual
  slc500
  stress_api_lock
  stress_rc_mem
  stress_test
  string_non_standard_udt
  string_standard
  test_alternate_tag_listing
  test_array_notation
  test_auto_sync
  test_callback
  test_callback_ex
  test_callback_ex_logix
  test_callback_ex_modbus
  test_connection_group
  test_emulator_performance
  test_event
  test_indexed_tags
  test_raw_cip
  test_reconnect
  test_shutdown
  test_special
  test_string
  test_tag_attributes
  test_tag_type_attribute
  thread_stress
  toggle_bit
  toggle_bool
  trigger_double_free
  write_string
  tag_rw_deprecated
  tag_rw2
)

foreach(example ${EXAMPLE_EXECUTABLES})
    if(example STREQUAL "simple_cpp")
        set(FILE_EXTENSION "cpp")
    else()
        set(FILE_EXTENSION "c")
    endif()

    add_executable(${example} "${CMAKE_SOURCE_DIR}/src/examples/${example}.${FILE_EXTENSION}" "${CMAKE_SOURCE_DIR}/src/examples/compat_utils.c")

    target_link_libraries(${example} plctag_static ${EXTRA_LINKER_LIBS})
endforeach()
