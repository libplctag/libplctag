message("Building perf_benchmark.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(PERF_BENCHMARK_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/perf_benchmark.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/stats.c
)

add_executable(perf_benchmark ${PERF_BENCHMARK_FILES})

target_include_directories(perf_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(perf_benchmark PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(perf_benchmark PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})

# Link math library for stats.c (sqrt, etc.)
if(UNIX)
    target_link_libraries(perf_benchmark PUBLIC m)
endif()
