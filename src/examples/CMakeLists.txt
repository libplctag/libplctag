message("Building examples.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_MINSIZEREL}")
endif()

 # message("CMAKE_C_FLAGS = ${CMAKE_C_FLAGS}")
 # message("CMAKE_CXX_FLAGS = ${CMAKE_CXX_FLAGS}")

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
  test_reconnect_after_outage
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
#   test_simultaneous_rw_ab_server
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

    # message("${example}:${CMAKE_BUILD_TYPE} is being compiled with flags=${CMAKE_C_FLAGS}")

    add_executable(${example} "${CMAKE_CURRENT_SOURCE_DIR}/${example}.${FILE_EXTENSION}" "${CMAKE_CURRENT_SOURCE_DIR}/compat_utils.c")

    if(example STREQUAL "simple_cpp")
        target_link_options(${example} PRIVATE "${STATIC_CXX_LINKER_OPTIONS}")
    else()
        target_link_options(${example} PRIVATE "${STATIC_C_LINKER_OPTIONS}")
    endif()

    # Link with the static library and apply static linking flags
    target_link_libraries(${example} plctag_static ${EXTRA_LINKER_LIBS})

    if(UNIX AND NOT APPLE)
        # Don't use static linking with sanitizers as they are incompatible
        if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT USE_SANITIZERS)
            target_link_options(${example} PRIVATE -static ${EXTRA_LINK_FLAGS})
        elseif(CMAKE_BUILD_TYPE STREQUAL "Debug" AND USE_SANITIZERS)
            # Use dynamic linking when sanitizers are enabled
            target_link_options(${example} PRIVATE ${EXTRA_LINK_FLAGS})
        else()
            target_link_options(${example} PRIVATE -static -s ${EXTRA_LINK_FLAGS})
        endif()
    endif()

    # if(STATIC_LINK_OPTIONS)
    #     set(EXTRA_LINK_OPTIONS "${EXTRA_LINK_OPTIONS} ${STATIC_LINK_OPTIONS}")
    # endif()

    # if(EXTRA_LINK_OPTIONS)
    #     target_link_options(${example} PRIVATE ${EXTRA_LINK_OPTIONS})
    # endif()
endforeach()
