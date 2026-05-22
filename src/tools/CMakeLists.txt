message("Building tools.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

# Build tool executables
add_subdirectory("ab_server")
add_subdirectory("tag_rw_deprecated")
add_subdirectory("tag_rw2")
add_subdirectory("list_tags_logix")
add_subdirectory("list_tags_micro8x0")
add_subdirectory("replay")
