message("Building test utilities.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

# message("CMAKE_C_FLAGS = ${CMAKE_C_FLAGS}")


# Define the source files for ab_server
set(AB_SERVER_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/cip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/cip.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/compat.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/cpf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/cpf.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/eip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/eip.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/memory.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/memory.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/mutex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/mutex.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/pccc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/pccc.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/plc.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/slice.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/socket.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/socket.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/tcp_server.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/tcp_server.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/thread.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/thread.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/utils.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ab_server/src/utils.h
)

# Add ab_server executable
add_executable(ab_server ${AB_SERVER_FILES})
target_link_libraries(ab_server PUBLIC ${EXTRA_LINKER_LIBS})

# if(STATIC_LINK_OPTIONS)
#     set(EXTRA_LINK_OPTIONS "${EXTRA_LINK_OPTIONS} ${STATIC_LINK_OPTIONS}")
# endif()

# if(EXTRA_LINK_OPTIONS)
    # target_link_options(ab_server PUBLIC ${EXTRA_LINK_OPTIONS})
    target_link_options(ab_server PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
# endif()

if(POSIX AND BUILD_MODBUS_EMULATOR)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBMODBUS REQUIRED libmodbus)

    # Add modbus_server executable
    add_executable(modbus_server ${CMAKE_CURRENT_SOURCE_DIR}/modbus_server/modbus_server.c)

    target_include_directories(modbus_server PRIVATE ${LIBMODBUS_INCLUDE_DIRS})

    target_link_libraries(modbus_server ${LIBMODBUS_LINK_LIBRARIES})

    macro(print_all_modbus_variables)
        message(STATUS "print_all_modbus_variables------------------------------------------{")
        get_cmake_property(_variableNames VARIABLES)
        foreach (_variableName ${_variableNames})
            string(REGEX MATCH "^LIBMODBUS.*" MATCHED_STR "${_variableName}")
            if(MATCHED_STR)
                message(STATUS "${_variableName}=${${_variableName}}")
            endif()
        endforeach()
        message(STATUS "print_all_modbus_variables------------------------------------------}")
    endmacro()

    # Debugging
    # print_all_modbus_variables()
else()
    message("Not building Modbus test server.")
endif()
