message("Building ab_server.")

# Define the source files for ab_server
set(AB_SERVER_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cip.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/compat.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpf.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/eip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/eip.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/err.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/err.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fairness.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fairness.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/memory.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/memory.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mutex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mutex.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pccc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pccc.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/plc.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/slice.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/socket.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/socket.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tcp_server.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tcp_server.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/thread.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/thread.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/utils.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/utils.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/log.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/log.h
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
