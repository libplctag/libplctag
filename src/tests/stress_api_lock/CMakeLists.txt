message("Building stress_api_lock.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(STRESS_API_LOCK_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/stress_api_lock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(stress_api_lock ${STRESS_API_LOCK_FILES})

target_include_directories(stress_api_lock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(stress_api_lock PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(stress_api_lock PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
