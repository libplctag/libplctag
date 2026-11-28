message("Building test_idle_peer_close.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_IDLE_PEER_CLOSE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_idle_peer_close.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_idle_peer_close ${TEST_IDLE_PEER_CLOSE_FILES})

target_include_directories(test_idle_peer_close PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_idle_peer_close PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_idle_peer_close PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
