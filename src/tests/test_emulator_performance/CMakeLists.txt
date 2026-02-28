message("Building test_emulator_performance.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_EMULATOR_PERFORMANCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_emulator_performance.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_emulator_performance ${TEST_EMULATOR_PERFORMANCE_FILES})

target_include_directories(test_emulator_performance PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_emulator_performance PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_emulator_performance PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
