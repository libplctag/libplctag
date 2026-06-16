message("Building ab_server_fiber POC (fiber_net + yafl stackful fibers).")

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

# ---- embeddable shared-library variant ----
# Opt-in: build the simulator as a shared library exposing
# ab_server_main()/ab_server_stop() (guarded by AB_SERVER_LIB in
# ab_server_fiber.c) so a host process can run it in-process on its own thread.
# OFF by default, so the standalone executable build is unaffected.
option(AB_SERVER_FIBER_LIB "Also build the fiber ab_server as an embeddable shared library" OFF)
if(AB_SERVER_FIBER_LIB)
    # yafl is a static lib; to link it into a shared library it must be PIC.
    # Without -fPIC its _Thread_local access uses the local-exec TLS model
    # (R_*_TLSLE), which the linker rejects in a shared object.
    set_target_properties(yafl PROPERTIES POSITION_INDEPENDENT_CODE ON)

    # Distinct target name so it does not collide with the standalone
    # src/tools/ab_server executable target in a full libplctag build; the
    # output is still libab_server so embedders link -lab_server.
    add_library(ab_server_fiber_lib SHARED ${SOURCES})
    set_target_properties(ab_server_fiber_lib PROPERTIES
        OUTPUT_NAME ab_server
        WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
    target_link_libraries(ab_server_fiber_lib PRIVATE yafl)
    target_compile_definitions(ab_server_fiber_lib PRIVATE AB_SERVER_LIB)
    target_include_directories(ab_server_fiber_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    if(WIN32)
        target_link_libraries(ab_server_fiber_lib PRIVATE ws2_32)
        target_compile_definitions(ab_server_fiber_lib PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
    elseif(UNIX)
        if(NOT APPLE)
            find_library(RT_LIBRARY rt)
            if(RT_LIBRARY)
                target_link_libraries(ab_server_fiber_lib PRIVATE ${RT_LIBRARY})
            endif()
        endif()
        target_link_libraries(ab_server_fiber_lib PRIVATE m)
    endif()
endif()
