# Define a top-level unit test directory

# test_init mocks initialize_modules()'s dependencies via -Wl,--wrap, a GNU ld feature.
# MSVC's linker has no equivalent, so building here would silently link the real
# refcount_startup/lib_init/etc. instead of the mocks -- a test that runs and reports a
# result, just the wrong one. Bail out before that can happen instead of relying on the
# caller's platform gate to keep MSVC out.
if(NOT (CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID MATCHES "Clang"))
    message(STATUS "Skipping unit tests: -Wl,--wrap requires GCC or Clang (using GNU ld), not ${CMAKE_C_COMPILER_ID}.")
    return()
endif()

message("Building unit tests with mini_mock (local CMocka-macro stand-in, see mini_mock.h).")

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
)

# If you have platform headers or utils
target_include_directories(test_init PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag/protocols
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
    ${PLATFORM_SHIM_PATH}
)

target_link_libraries(test_init PRIVATE plctag_static ${EXTRA_LINKER_LIBS})

target_link_options(test_init PRIVATE ${MOCK_LINK_FLAGS})

# Register the unit test with CTest
add_test(NAME unit_test_init COMMAND test_init)

# Create a convenient `make unit_tests` target
add_custom_target(unit_tests
    DEPENDS test_init
    COMMAND ${CMAKE_CTEST_COMMAND} -R "unit_test" --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running unit tests"
)