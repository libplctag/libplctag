message("Building device_sim tool.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(DEVICE_SIM_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../utils/arena.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../utils/arena.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../utils/bytes.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../utils/bytes.h
    ${CMAKE_CURRENT_SOURCE_DIR}/args.c
    ${CMAKE_CURRENT_SOURCE_DIR}/args.h
    ${CMAKE_CURRENT_SOURCE_DIR}/cip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cip.h
    ${CMAKE_CURRENT_SOURCE_DIR}/cpf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cpf.h
    ${CMAKE_CURRENT_SOURCE_DIR}/device.h
    ${CMAKE_CURRENT_SOURCE_DIR}/eip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/eip.h
    ${CMAKE_CURRENT_SOURCE_DIR}/pccc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/pccc.h
    ${CMAKE_CURRENT_SOURCE_DIR}/server.c
    ${CMAKE_CURRENT_SOURCE_DIR}/server.h
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
)

add_executable(device_sim ${DEVICE_SIM_FILES})

target_include_directories(device_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils
)

target_link_libraries(device_sim PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(device_sim PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
