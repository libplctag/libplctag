message("Building ab_server_fiber POC (fiber_net + yafl stackful fibers).")

cmake_minimum_required(VERSION 3.15)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---- yafl stackful fiber library ----
set(YAFL_DIR "$ENV{HOME}/Projects/yafl")
if(NOT TARGET yafl)
    add_subdirectory(${YAFL_DIR} ${CMAKE_CURRENT_BINARY_DIR}/yafl)
    if(TARGET yafl)
        target_compile_options(yafl PRIVATE
            -Wno-error=sign-conversion
            -Wno-error=conversion
        )
    endif()
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
