message("Building test_fairness.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_FAIRNESS_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_fairness.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_fairness ${TEST_FAIRNESS_FILES})

target_include_directories(test_fairness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_fairness PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_fairness PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})

# test_fairness needs stats.c for statistics calculations
target_sources(test_fairness PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../utils/stats.c")

# Link math library for tests that use sqrt() and other math functions
if(UNIX)
    target_link_libraries(test_fairness PUBLIC m)
endif()
