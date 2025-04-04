# src/tests/CMakeLists.txt

# This CMakeLists.txt file is for building executables in the src/tests directory.
# It is intended to be included as a subdirectory from the main CMakeLists.txt.

# Ensure C11 standard is used
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

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

message("BASE_C_FLAGS = ${BASE_C_FLAGS}")
message("CMAKE_C_FLAGS_DEBUG = ${CMAKE_C_FLAGS_DEBUG}")

# Set compile flags for ab_server (ensure C11 and static linking)
foreach(AB_SERVER_FILE ${AB_SERVER_FILES})
    set_source_files_properties("${AB_SERVER_FILE}" PROPERTIES COMPILE_FLAGS "${BASE_C_FLAGS}")
endforeach()

# set the include files for the ab_server
target_include_directories(ab_server PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ab_server")

# Set link flags for ab_server
if(BASE_LINK_FLAGS)
    set_target_properties(ab_server PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")
# else()
#     set_target_properties(ab_server PROPERTIES LINK_FLAGS "-static")
endif()

# Add modbus_server executable
add_executable(modbus_server ${CMAKE_CURRENT_SOURCE_DIR}/modbus_server/modbus_server.c)

if(NOT WIN32)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBMODBUS REQUIRED libmodbus)
else()
    # Manually specify paths or use find_package
endif()

# Set compile flags for modbus_server (ensure C11 and static linking)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/modbus_server/modbus_server.c PROPERTIES COMPILE_FLAGS "${BASE_C_FLAGS} ${LIBMODBUS_CFLAGS}")
message("C flags  = ${BASE_C_FLAGS} ${LIBMODBUS_CFLAGS}")

# set the link flags
message("BASE_LINK_FLAGS = ${BASE_LINK_FLAGS}")
set_target_properties(modbus_server PROPERTIES LINK_FLAGS "${BASE_LINK_FLAGS}")

# Link modbus_server
target_link_libraries(modbus_server ${LIBMODBUS_LINK_LIBRARIES})
message("Libraries = ${LIBMODBUS_LINK_LIBRARIES}")


macro(print_all_modbus_variables)
    message(STATUS "print_all_modbus_variables------------------------------------------{")
    get_cmake_property(_variableNames VARIABLES)
    foreach (_variableName ${_variableNames})
        string(REGEX MATCH "^libmodbus.*" MATCHED_STR "${_variableName}")
        if(MATCHED_STR)
            message(STATUS "${_variableName}=${${_variableName}}")
        endif()
    endforeach()
    message(STATUS "print_all_modbus_variables------------------------------------------}")
endmacro()

# Debugging
print_all_modbus_variables()
