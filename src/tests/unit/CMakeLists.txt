# Define a top-level unit test directory

message("Building unit tests with CMocka.")

# Fetch CMocka
include(FetchContent)
FetchContent_Declare(
    cmocka
    GIT_REPOSITORY https://git.cryptomilk.org/projects/cmocka.git
    GIT_TAG        cmocka-1.1.7
)
# Suppress CMocka's own tests and examples from polluting our build
set(UNIT_TESTING OFF CACHE BOOL "Disable CMocka tests" FORCE)
set(WITH_EXAMPLES OFF CACHE BOOL "Disable CMocka examples" FORCE)
# Build static cmocka: the project uses -fvisibility=hidden which hides cmocka's
# internal symbols (_will_return etc.) in the shared library, breaking linking on
# Linux. Setting BUILD_SHARED_LIBS=OFF makes cmocka's add_library() produce a
# static archive where all symbols are accessible to the linker regardless of
# visibility attributes.
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(cmocka)

# We want to mock out initialize_modules dependencies using GNU linker wraps
set(MOCK_LINK_FLAGS 
    "-Wl,--wrap=refcount_startup"
    "-Wl,--wrap=lib_init"
    "-Wl,--wrap=ab_init"
    "-Wl,--wrap=mb_init"
    "-Wl,--wrap=omron_init"
)

# test_init: tests initialize_modules()
add_executable(test_init 
    test_init.c 
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/lib/init.c
)

# Add necessary includes
target_include_directories(test_init PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag
    ${cmocka_SOURCE_DIR}/include
)

# If you have platform headers or utils
target_include_directories(test_init PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/protocols
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
    ${PLATFORM_SHIM_PATH}
)

# Link against cmocka (static archive when BUILD_SHARED_LIBS=OFF)
target_link_libraries(test_init PRIVATE cmocka plctag_static ${EXTRA_LINKER_LIBS})

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    if(NOT APPLE)
        target_link_options(test_init PRIVATE ${MOCK_LINK_FLAGS})
    endif()
endif()

# Register the unit test with CTest
add_test(NAME unit_test_init COMMAND test_init)

# test_cbor: tests utils/cbor.c against RFC 8949 examples (pure, no I/O).
# Links plctag_static directly (cbor.c is an unconditional utils/ source, not
# gated by any protocol feature flag) rather than compiling cbor.c a second
# time, since it has no protocol-specific dependencies to isolate.
add_executable(test_cbor test_cbor.c)

target_include_directories(test_cbor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
    ${cmocka_SOURCE_DIR}/include
    ${PLATFORM_SHIM_PATH}
)

target_link_libraries(test_cbor PRIVATE cmocka plctag_static ${EXTRA_LINKER_LIBS})

add_test(NAME unit_test_cbor COMMAND test_cbor)

# test_enip_type: tests enip_type_decode() (pure, no I/O)
add_executable(test_enip_type
    test_enip_type.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/protocols/enip/client/enip_type.c
)

target_include_directories(test_enip_type PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/protocols
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
    ${cmocka_SOURCE_DIR}/include
    ${PLATFORM_SHIM_PATH}
)

target_link_libraries(test_enip_type PRIVATE cmocka plctag_static ${EXTRA_LINKER_LIBS})

add_test(NAME unit_test_enip_type COMMAND test_enip_type)

# test_enip_cip_path: tests enip_cip_encode_path() (pure, no I/O)
add_executable(test_enip_cip_path
    test_enip_cip_path.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/protocols/enip/client/enip_cip.c
)

target_include_directories(test_enip_cip_path PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/protocols
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
    ${cmocka_SOURCE_DIR}/include
    ${PLATFORM_SHIM_PATH}
)

target_link_libraries(test_enip_cip_path PRIVATE cmocka plctag_static ${EXTRA_LINKER_LIBS})

add_test(NAME unit_test_enip_cip_path COMMAND test_enip_cip_path)

# test_enip_eip_cpf: tests enip_eip_*/enip_cpf_* encode/decode round trips (pure, no I/O)
add_executable(test_enip_eip_cpf
    test_enip_eip_cpf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/protocols/enip/client/enip_eip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/protocols/enip/client/enip_cpf.c
)

target_include_directories(test_enip_eip_cpf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/protocols
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
    ${cmocka_SOURCE_DIR}/include
    ${PLATFORM_SHIM_PATH}
)

target_link_libraries(test_enip_eip_cpf PRIVATE cmocka plctag_static ${EXTRA_LINKER_LIBS})

add_test(NAME unit_test_enip_eip_cpf COMMAND test_enip_eip_cpf)

# test_enip_pccc_addr: tests PCCC address parse/encode (pure, no I/O)
add_executable(test_enip_pccc_addr
    test_enip_pccc_addr.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/protocols/enip/client/enip_pccc_addr.c
)

target_include_directories(test_enip_pccc_addr PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/protocols
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
    ${cmocka_SOURCE_DIR}/include
    ${PLATFORM_SHIM_PATH}
)

target_link_libraries(test_enip_pccc_addr PRIVATE cmocka plctag_static ${EXTRA_LINKER_LIBS})

add_test(NAME unit_test_enip_pccc_addr COMMAND test_enip_pccc_addr)

# test_udt_template: tests the class 0x6C UDT template registry
# (device_sim_add_udt_type/device_udt_find, pure logic, no sockets). Needs
# device_sim.c and friends actually compiled into plctag_static, which only
# happens under LIBPLCTAG_FEATURE_SERVER + LIBPLCTAG_FEATURE_ENIP -- unlike
# the enip client-side tests above, this one is not self-contained enough to
# compile its target sources directly, so it is gated to when those features
# are on (mirrors the top-level CMakeLists.txt LIBPLCTAG_FEATURE_SERVER guard).
set(UNIT_TESTS_LIST test_init test_cbor test_enip_type test_enip_cip_path test_enip_eip_cpf test_enip_pccc_addr)

if(LIBPLCTAG_FEATURE_SERVER AND LIBPLCTAG_FEATURE_ENIP)
    add_executable(test_udt_template test_udt_template.c)

    target_include_directories(test_udt_template PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
        ${cmocka_SOURCE_DIR}/include
        ${PLATFORM_SHIM_PATH}
    )

    target_link_libraries(test_udt_template PRIVATE cmocka plctag_static ${EXTRA_LINKER_LIBS})

    add_test(NAME unit_test_udt_template COMMAND test_udt_template)

    list(APPEND UNIT_TESTS_LIST test_udt_template)
endif()

# Create a convenient `make unit_tests` target
add_custom_target(unit_tests
    DEPENDS ${UNIT_TESTS_LIST}
    COMMAND ${CMAKE_CTEST_COMMAND} -R "unit_test" --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running unit tests"
)