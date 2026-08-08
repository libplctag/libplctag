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
  get_identity
  multithread
  multithread_cached_read
  multithread_plc5
  multithread_plc5_dhp
  plc5
  simple
  simple_cpp
  simple_dual
  slc500
  string_non_standard_udt
  string_standard
  toggle_bit
  toggle_bool
  write_string
)

# ENIP-only CLI tools (enip-udp discovery, PLCTAG_FORMAT_CBOR) -- exercised
# by src/tests/scripts/run_hardware_tests.sh and run_enip_tests.sh against
# real hardware, not built when the feature is off.
if(LIBPLCTAG_FEATURE_ENIP)
    list(APPEND EXAMPLE_EXECUTABLES discover_identity identity_cbor)
endif()

foreach(example ${EXAMPLE_EXECUTABLES})
    if(example STREQUAL "simple_cpp")
        set(FILE_EXTENSION "cpp")
    else()
        set(FILE_EXTENSION "c")
    endif()

    # message("${example}:${CMAKE_BUILD_TYPE} is being compiled with flags=${CMAKE_C_FLAGS}")

    add_executable(${example} "${CMAKE_CURRENT_SOURCE_DIR}/${example}.${FILE_EXTENSION}" "${CMAKE_CURRENT_SOURCE_DIR}/compat_utils.c")

    # STATIC_C_LINKER_OPTIONS/STATIC_CXX_LINKER_OPTIONS (cmake_toolchains/clang_or_gcc.cmake)
    # already go static-free when sanitizers are enabled, same as every other target
    # (tools, tests) that uses them.
    if(example STREQUAL "simple_cpp")
        target_link_options(${example} PRIVATE ${STATIC_CXX_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
    else()
        target_link_options(${example} PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
    endif()

    # Link with the static library and apply static linking flags
    target_link_libraries(${example} plctag_static ${EXTRA_LINKER_LIBS})

    # if(STATIC_LINK_OPTIONS)
    #     set(EXTRA_LINK_OPTIONS "${EXTRA_LINK_OPTIONS} ${STATIC_LINK_OPTIONS}")
    # endif()

    # if(EXTRA_LINK_OPTIONS)
    #     target_link_options(${example} PRIVATE ${EXTRA_LINK_OPTIONS})
    # endif()
endforeach()
