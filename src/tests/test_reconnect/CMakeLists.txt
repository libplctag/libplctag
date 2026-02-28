message("Building test_reconnect.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_RECONNECT_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_reconnect.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_reconnect ${TEST_RECONNECT_FILES})

target_include_directories(test_reconnect PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_reconnect PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_reconnect PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
