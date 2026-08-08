message("Building ab_server_fiber POC (fiber_net + yafl stackful fibers).")

# Override the inherited client flags: ab_server_fiber is a standalone process, so its
# sanitizer choice is independent -- see USE_SERVER_MEM_SANITIZERS / USE_SERVER_THREAD_SANITIZERS.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    # Override CMAKE_C_FLAGS_DEBUG too: CMake auto-appends it on top of CMAKE_C_FLAGS for
    # Debug configs, which would otherwise reintroduce the client's sanitizer flags here.
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG_BASE} ${SERVER_SANITIZE_FLAGS}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

cmake_minimum_required(VERSION 3.15)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---- yafl stackful fiber library ----
if(NOT TARGET yafl)
    set(YAFL_DIR "${CMAKE_CURRENT_LIST_DIR}/../../vendor/libyafl")
    if(NOT EXISTS "${YAFL_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "libyafl not found; expected vendored copy at src/vendor/libyafl")
    endif()
    add_subdirectory("${YAFL_DIR}" "${CMAKE_BINARY_DIR}/vendor/libyafl")
endif()
if(NOT TARGET yafl)
    message(FATAL_ERROR "yafl target not found; expected to be defined by the libyafl CMakeLists.txt")
    return()
endif()

set(SOURCES
    ab_server_fiber.c
    eip.c
    cpf.c
    cip.c
    pccc.c
    fiber_net.c
    arena.c
    bytes.c
    log.c
    args.c
    err.c
    utils.c
)

# Local directory first so our log.h (buf_t-free) takes precedence.
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(ab_server_fiber ${SOURCES})

target_link_libraries(ab_server_fiber yafl)

if(WIN32)
    target_link_libraries(ab_server_fiber ws2_32)
    target_compile_definitions(ab_server_fiber PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _WINSOCK_DEPRECATED_NO_WARNINGS
    )
elseif(UNIX)
    if(NOT APPLE)
        find_library(RT_LIBRARY rt)
        if(RT_LIBRARY)
            target_link_libraries(ab_server_fiber ${RT_LIBRARY})
        endif()
    endif()
    target_link_libraries(ab_server_fiber m)
    target_compile_options(ab_server_fiber PRIVATE -Wall -Wextra)
endif()
