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
    ${PLATFORM_SHIM_PATH}
)

target_link_libraries(test_init PRIVATE plctag_static ${EXTRA_LINKER_LIBS})

target_compile_definitions(test_init PRIVATE ${MOCK_DEFINITIONS})

# Register the unit test with CTest
add_test(NAME unit_test_init COMMAND test_init)

# Create a convenient `make unit_tests` target
add_custom_target(unit_tests
    DEPENDS test_init
    COMMAND ${CMAKE_CTEST_COMMAND} -R "unit_test" --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running unit tests"
)