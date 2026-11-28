message("Building test_ab_server_framing.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

add_executable(test_ab_server_framing ${CMAKE_CURRENT_SOURCE_DIR}/test_ab_server_framing.c)

target_link_libraries(test_ab_server_framing PUBLIC ${EXTRA_LINKER_LIBS})
target_link_options(test_ab_server_framing PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
