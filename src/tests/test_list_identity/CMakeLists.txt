message("Building test_list_identity.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

add_executable(test_list_identity ${CMAKE_CURRENT_SOURCE_DIR}/test_list_identity.c)

if(WIN32)
    target_link_libraries(test_list_identity PRIVATE ws2_32)
endif()

target_link_options(test_list_identity PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
