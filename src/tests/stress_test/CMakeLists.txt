message("Building stress_test.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(STRESS_TEST_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/stress_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(stress_test ${STRESS_TEST_FILES})

target_include_directories(stress_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(stress_test PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(stress_test PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
