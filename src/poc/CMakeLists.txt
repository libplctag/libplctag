message("Building proofs of concept.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

# When building for x86, libyafl's project() resets CMAKE_SYSTEM_PROCESSOR to the
# host value. Override the triple so libyafl picks the right toolchain and assembly.
if(BUILD_32_BIT AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(TRIPLE "i386-pc-linux-gnu")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "Win32")
    set(TRIPLE "i386-pc-windows-msvc")
elseif(BUILD_32_BIT AND CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(TRIPLE "i386-pc-windows-gnu")
endif()

# Build POC programs
add_subdirectory("modbus_server")
add_subdirectory("modbus_server2")
add_subdirectory("modbus_server3")
add_subdirectory("scan_eip_network")
add_subdirectory("test_alternate_tag_listing")
add_subdirectory("list_tags_omron")
add_subdirectory("ab_server_fiber")
