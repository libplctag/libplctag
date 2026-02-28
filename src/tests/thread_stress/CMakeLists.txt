message("Building thread_stress.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(THREAD_STRESS_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/thread_stress.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(thread_stress ${THREAD_STRESS_FILES})

target_include_directories(thread_stress PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(thread_stress PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(thread_stress PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
