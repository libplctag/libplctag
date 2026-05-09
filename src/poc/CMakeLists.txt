message("Building proofs of concept.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

# Build POC programs
add_subdirectory("modbus_server")
add_subdirectory("modbus_server2")
add_subdirectory("modbus_server3")
add_subdirectory("scan_eip_network")
add_subdirectory("test_alternate_tag_listing")
add_subdirectory("list_tags_omron")
add_subdirectory("ab_server_fiber")
