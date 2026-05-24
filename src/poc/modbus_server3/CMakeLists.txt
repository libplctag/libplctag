message("Building modbus_server3 POC (fiber_net + yafl stackful fibers).")

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

# ---- Shared source files from the original modbus_server POC ----
set(MODBUS_SERVER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../modbus_server)

set(SOURCES
    modbus_server3.c
    modbus_protocol3.c
    fiber_net.c
    arena.c
    bytes.c
    log.c
    ${MODBUS_SERVER_SRC}/register_storage.c
    ${MODBUS_SERVER_SRC}/err.c
    ${MODBUS_SERVER_SRC}/args.c
    ${MODBUS_SERVER_SRC}/utils.c
)

# Local directory first so log.h (buf_t-free copy) shadows modbus_server/log.h.
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MODBUS_SERVER_SRC}
)

add_executable(modbus_server3 ${SOURCES})

# yafl PUBLIC include dirs propagate automatically via target_link_libraries.
target_link_libraries(modbus_server3 yafl)

if(WIN32)
    target_link_libraries(modbus_server3 ws2_32)
    target_compile_definitions(modbus_server3 PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _WINSOCK_DEPRECATED_NO_WARNINGS
    )
elseif(UNIX)
    # librt (needed on older Linux for clock_gettime)
    if(NOT APPLE)
        find_library(RT_LIBRARY rt)
        if(RT_LIBRARY)
            target_link_libraries(modbus_server3 ${RT_LIBRARY})
        endif()
    endif()

    # math library (used in statistics output)
    target_link_libraries(modbus_server3 m)

    target_compile_options(modbus_server3 PRIVATE -Wall -Wextra)
endif()

# ---- Null-fiber context-switch benchmark ----
add_executable(bench_fiber_switch
    bench_fiber_switch.c
    ${MODBUS_SERVER_SRC}/utils.c
)
include_directories(bench_fiber_switch PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MODBUS_SERVER_SRC}
)
target_link_libraries(bench_fiber_switch yafl)
if(UNIX AND NOT APPLE)
    find_library(RT_LIB rt)
    if(RT_LIB)
        target_link_libraries(bench_fiber_switch ${RT_LIB})
    endif()
endif()
