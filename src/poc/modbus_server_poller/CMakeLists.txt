message("Building modbus_server_poller POC.")

# A fork of src/tools/modbus_server rewritten onto utils/poller.h and
# utils/socket_fd.h. The original is untouched and is still what the test suite
# runs; this one exists to shape the poller against a real caller, which is the
# condition docs/socket_layering_design.md puts on step 3.
#
# Only the server loop is forked. The Modbus framing, the register storage and
# the buffer/error/log/args utilities are the same files the original builds,
# not copies -- so any behavioural difference between the two servers is a
# difference in the socket layer and nothing else.

# Override the inherited client flags: this is a standalone process, so its
# sanitizer choice is independent -- see USE_SERVER_MEM_SANITIZERS /
# USE_SERVER_THREAD_SANITIZERS.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG_BASE} ${SERVER_SANITIZE_FLAGS}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(MODBUS_SERVER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/modbus_server")
set(TOOLS_UTILS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/utils")

set(SOURCES
    modbus_server_poller.c

    # shared with the original server, unmodified
    "${MODBUS_SERVER_DIR}/modbus_protocol.c"
    "${MODBUS_SERVER_DIR}/register_storage.c"

    # shared utilities; note that socket.c and coro_net.c are deliberately
    # absent -- replacing them is the point of this fork
    "${TOOLS_UTILS_DIR}/buf.c"
    "${TOOLS_UTILS_DIR}/err.c"
    "${TOOLS_UTILS_DIR}/log.c"
    "${TOOLS_UTILS_DIR}/args.c"
    "${TOOLS_UTILS_DIR}/utils.c"
)

# socket_fd and poller are the new layers under test. They come from
# plctag_static rather than being compiled in here: they are library code, and
# building a second copy of them would be the start of exactly the duplication
# 2.4 exists to undo.

add_executable(modbus_server_poller ${SOURCES})

target_include_directories(modbus_server_poller PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MODBUS_SERVER_DIR}
    ${TOOLS_UTILS_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libplctag
)

target_link_libraries(modbus_server_poller PRIVATE plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(modbus_server_poller PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})

if(WIN32)
    target_link_libraries(modbus_server_poller PRIVATE ws2_32)

    target_compile_definitions(modbus_server_poller PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _WINSOCK_DEPRECATED_NO_WARNINGS
    )
elseif(UNIX)
    if(NOT APPLE)
        find_library(RT_LIBRARY rt)
        if(RT_LIBRARY)
            target_link_libraries(modbus_server_poller PRIVATE ${RT_LIBRARY})
        endif()
    endif()

    target_link_libraries(modbus_server_poller PRIVATE m)
endif()

if(MSVC)
    target_compile_options(modbus_server_poller PRIVATE /wd4996)
else()
    target_compile_options(modbus_server_poller PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter
    )
endif()

install(TARGETS modbus_server_poller
    RUNTIME DESTINATION bin
)
