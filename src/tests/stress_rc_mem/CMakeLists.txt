message("Building stress_rc_mem.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(STRESS_RC_MEM_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/stress_rc_mem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(stress_rc_mem ${STRESS_RC_MEM_FILES})

target_include_directories(stress_rc_mem PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(stress_rc_mem PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(stress_rc_mem PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
