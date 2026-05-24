message("Building modbus_server2 POC (BSD sockets + select() + pthreads).")

cmake_minimum_required(VERSION 3.15)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Shared source files from the original modbus_server POC
set(MODBUS_SERVER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../modbus_server)

set(SOURCES
    modbus_server2.c
    ${MODBUS_SERVER_SRC}/modbus_protocol.c
    ${MODBUS_SERVER_SRC}/register_storage.c
    ${MODBUS_SERVER_SRC}/buf.c
    ${MODBUS_SERVER_SRC}/err.c
    ${MODBUS_SERVER_SRC}/log.c
    ${MODBUS_SERVER_SRC}/args.c
    ${MODBUS_SERVER_SRC}/utils.c
)

# Include both directories: modbus_server2/ (for any local headers) and
# modbus_server/ (for shared headers + log_modules.def)
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MODBUS_SERVER_SRC}
)

add_executable(modbus_server2 ${SOURCES})

if(WIN32)
    target_link_libraries(modbus_server2 ws2_32)
    target_compile_definitions(modbus_server2 PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _WINSOCK_DEPRECATED_NO_WARNINGS
    )
elseif(UNIX)
    # pthreads
    find_package(Threads REQUIRED)
    target_link_libraries(modbus_server2 Threads::Threads)

    # librt (needed on older Linux for clock_gettime)
    if(NOT APPLE)
        find_library(RT_LIBRARY rt)
        if(RT_LIBRARY)
            target_link_libraries(modbus_server2 ${RT_LIBRARY})
        endif()
    endif()

    # math library (used in statistics output)
    target_link_libraries(modbus_server2 m)

    target_compile_options(modbus_server2 PRIVATE -Wall -Wextra)
endif()
