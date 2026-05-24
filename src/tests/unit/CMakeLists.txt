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

# Create a convenient `make unit_tests` target
add_custom_target(unit_tests
    DEPENDS test_init
    COMMAND ${CMAKE_CTEST_COMMAND} -R "unit_test" --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running unit tests"
)