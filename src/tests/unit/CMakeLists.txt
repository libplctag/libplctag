# Define a top-level unit test directory

message("Building unit tests with mini_mock (local CMocka-macro stand-in, see mini_mock.h).")

# Redirect initialize_modules()'s dependencies to the mocks in test_init.c at compile
# time. This works because init.c is listed in add_executable() below and so is compiled
# here rather than taken from the prebuilt archive -- we control its preprocessing.
#
# The previous approach used -Wl,--wrap, which is GNU ld only and kept this test off
# macOS and MSVC entirely. Plain -D uses no linker features at all, so it works
# everywhere. See the comment in test_init.c for why link-order shadowing is not an
# option (the mocked *_init functions share object files with the *_teardown functions
# that destroy_modules() genuinely needs).
#
# Caveat: this only reaches code compiled into this target. Mocking a function called
# from deep inside the prebuilt library would need something else.
set(MOCK_DEFINITIONS
    "refcount_startup=mock_refcount_startup"
    "lib_init=mock_lib_init"
    "ab_init=mock_ab_init"
    "mb_init=mock_mb_init"
    "omron_init=mock_omron_init"
)

# test_init: tests initialize_modules()
add_executable(test_init 
    test_init.c 
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/lib/init.c
)

# Add necessary includes
target_include_directories(test_init PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag
)

# If you have platform headers or utils
target_include_directories(test_init PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/protocols
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
)

target_link_libraries(test_init PRIVATE plctag_static ${EXTRA_LINKER_LIBS})

target_compile_definitions(test_init PRIVATE ${MOCK_DEFINITIONS})

# test_platform_str: tests str_to_int()/str_to_float() in the platform shim.
# No mocks, so no MOCK_DEFINITIONS -- it links and calls the real functions.
add_executable(test_platform_str test_platform_str.c)

target_include_directories(test_platform_str PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/protocols
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
)

target_link_libraries(test_platform_str PRIVATE plctag_static ${EXTRA_LINKER_LIBS})

# test_tag_name_bounds: tests the CIP tag name encoder's bounds checks.
# Tag creation encodes the name before any network I/O, so this needs no PLC.
add_executable(test_tag_name_bounds test_tag_name_bounds.c)

target_include_directories(test_tag_name_bounds PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/protocols
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
)

target_link_libraries(test_tag_name_bounds PRIVATE plctag_static ${EXTRA_LINKER_LIBS})

# test_atomic_utils: checks the utils/atomic_utils contract (set and compare-and-set
# return the ORIGINAL value) and that set is a real atomic exchange rather than a read
# followed by a write, by building a spin lock on it and hammering it with threads.
#
# atomic_utils.c holds two separate implementations -- a C11 one and a fallback for
# compilers without <stdatomic.h> -- and a build only ever compiles one of them. So the
# test is built twice. The first links the library and exercises whatever this compiler
# selected; the second compiles atomic_utils.c directly with __STDC_NO_ATOMICS__ to force
# the fallback, which is otherwise dead code on every platform that has C11 atomics.
#
# The forced target still links plctag_static for thread_create()/thread_join(), but its
# own atomic_utils.o resolves the atomic_* symbols first, so the archive member is never
# pulled in and there is no duplicate definition.
add_executable(test_atomic_utils test_atomic_utils.c)

target_include_directories(test_atomic_utils PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/protocols
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
)

target_link_libraries(test_atomic_utils PRIVATE plctag_static ${EXTRA_LINKER_LIBS})

add_executable(test_atomic_utils_no_c11
    test_atomic_utils.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../utils/atomic_utils.c
)

target_include_directories(test_atomic_utils_no_c11 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/protocols
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
)

target_compile_definitions(test_atomic_utils_no_c11 PRIVATE __STDC_NO_ATOMICS__)

target_link_libraries(test_atomic_utils_no_c11 PRIVATE plctag_static ${EXTRA_LINKER_LIBS})

# test_byte_buf: the length-carrying byte buffer in utils/byte_buf.h.  Header-only, so
# this checks the edges: error propagation through a chain, bounds refusal at each
# width, and the reserve-then-fill split that lets an outer header be written after
# the payload whose length it carries.
add_executable(test_byte_buf test_byte_buf.c)

target_include_directories(test_byte_buf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/protocols
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
)

target_link_libraries(test_byte_buf PRIVATE plctag_static ${EXTRA_LINKER_LIBS})

# test_cip_path_encode: byte-exact expectations for cip_encode_path(), pinning the
# current encoder before it is rewritten as a recursive descent parser over port/node
# pairs. Path encoding is almost untested elsewhere -- the simulator suite runs "1,0"
# and little else, and DH+/CIP bridging paths only appear against real hardware.
add_executable(test_cip_path_encode test_cip_path_encode.c)

target_include_directories(test_cip_path_encode PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/protocols
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
)

target_link_libraries(test_cip_path_encode PRIVATE plctag_static ${EXTRA_LINKER_LIBS})

# test_eip_response_validation: checks that a response whose EIP header does not answer the
# request we sent is rejected. It uses POSIX sockets directly to stand up a hostile server,
# because the library's own socket shim is internal, so it is skipped on Windows.
set(EXTRA_UNIT_TESTS "")

if(NOT WIN32)
    add_executable(test_eip_response_validation test_eip_response_validation.c)

    target_include_directories(test_eip_response_validation PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag
        ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/protocols
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
    )

    target_link_libraries(test_eip_response_validation PRIVATE plctag_static ${EXTRA_LINKER_LIBS})

    add_test(NAME unit_test_eip_response_validation COMMAND test_eip_response_validation)

    set(EXTRA_UNIT_TESTS test_eip_response_validation)
endif()

# Register the unit tests with CTest
# test_poller: socket_fd and poller over real loopback socket pairs rather than mocks.
# There is no meaningful way to mock a readiness answer -- the question these layers
# exist to answer is what the kernel does. Each case pins a way an event loop wedges
# when the layer is wrong: a phantom event on an idle wait, a wake redelivered forever
# because it was never drained, an event carrying the wrong connection's context, a
# removed socket resurfacing, a polite hangup read as an error.
add_executable(test_poller test_poller.c)

target_include_directories(test_poller PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/protocols
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
)

target_link_libraries(test_poller PRIVATE plctag_static ${EXTRA_LINKER_LIBS})

add_test(NAME unit_test_init COMMAND test_init)
add_test(NAME unit_test_platform_str COMMAND test_platform_str)
add_test(NAME unit_test_tag_name_bounds COMMAND test_tag_name_bounds)
add_test(NAME unit_test_cip_path_encode COMMAND test_cip_path_encode)
add_test(NAME unit_test_byte_buf COMMAND test_byte_buf)
add_test(NAME unit_test_atomic_utils COMMAND test_atomic_utils)
add_test(NAME unit_test_atomic_utils_no_c11 COMMAND test_atomic_utils_no_c11)
add_test(NAME unit_test_poller COMMAND test_poller)

# Create a convenient `make unit_tests` target
add_custom_target(unit_tests
    DEPENDS test_init test_platform_str test_tag_name_bounds test_cip_path_encode test_byte_buf test_atomic_utils
            test_atomic_utils_no_c11 test_poller
            ${EXTRA_UNIT_TESTS}
    COMMAND ${CMAKE_CTEST_COMMAND} -R "unit_test" --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running unit tests"
)
# test_pccc_type_encode: byte-exact expectations for the PCCC type descriptor encoder.
# pccc_encode_dt_byte() had no callers at all until 1.10, so nothing had ever run it;
# three of the cases pin bugs that were in it the whole time. The anchor case is a real
# ControlLogix reply, which is the only part of this that is not derived from the code.
add_executable(test_pccc_type_encode test_pccc_type_encode.c)

target_include_directories(test_pccc_type_encode PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/protocols
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
)

target_link_libraries(test_pccc_type_encode PRIVATE plctag_static ${EXTRA_LINKER_LIBS})
