message("Building ab_server tool.")

# Define the source files for ab_server
set(AB_SERVER_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/cip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cip.h
    ${CMAKE_CURRENT_SOURCE_DIR}/compat.h
    ${CMAKE_CURRENT_SOURCE_DIR}/cpf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cpf.h
    ${CMAKE_CURRENT_SOURCE_DIR}/eip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/eip.h
    ${CMAKE_CURRENT_SOURCE_DIR}/err.c
    ${CMAKE_CURRENT_SOURCE_DIR}/err.h
    ${CMAKE_CURRENT_SOURCE_DIR}/fairness.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fairness.h
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/memory.c
    ${CMAKE_CURRENT_SOURCE_DIR}/memory.h
    ${CMAKE_CURRENT_SOURCE_DIR}/mutex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mutex.h
    ${CMAKE_CURRENT_SOURCE_DIR}/pccc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/pccc.h
    ${CMAKE_CURRENT_SOURCE_DIR}/plc.h
    ${CMAKE_CURRENT_SOURCE_DIR}/slice.h
    ${CMAKE_CURRENT_SOURCE_DIR}/socket.c
    ${CMAKE_CURRENT_SOURCE_DIR}/socket.h
    ${CMAKE_CURRENT_SOURCE_DIR}/tcp_server.c
    ${CMAKE_CURRENT_SOURCE_DIR}/tcp_server.h
    ${CMAKE_CURRENT_SOURCE_DIR}/thread.c
    ${CMAKE_CURRENT_SOURCE_DIR}/thread.h
    ${CMAKE_CURRENT_SOURCE_DIR}/utils.c
    ${CMAKE_CURRENT_SOURCE_DIR}/utils.h
    ${CMAKE_CURRENT_SOURCE_DIR}/log.c
    ${CMAKE_CURRENT_SOURCE_DIR}/log.h
)

# Add ab_server executable
add_executable(ab_server ${AB_SERVER_FILES})
target_link_libraries(ab_server PUBLIC ${EXTRA_LINKER_LIBS})

# Link math library for ab_server (fairness.c uses math.h)
# Math functions are in libm on Unix but in the C runtime on Windows
if(UNIX)
    target_link_libraries(ab_server PUBLIC m)
endif()

target_link_options(ab_server PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
