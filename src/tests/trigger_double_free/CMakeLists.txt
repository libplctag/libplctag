message("Building trigger_double_free test.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TRIGGER_DOUBLE_FREE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/trigger_double_free.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(trigger_double_free ${TRIGGER_DOUBLE_FREE_FILES})

target_include_directories(trigger_double_free PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(trigger_double_free PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(trigger_double_free PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
