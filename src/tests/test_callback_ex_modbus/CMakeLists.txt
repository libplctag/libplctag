message("Building test_callback_ex_modbus.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_CALLBACK_EX_MODBUS_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_callback_ex_modbus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_callback_ex_modbus ${TEST_CALLBACK_EX_MODBUS_FILES})

target_include_directories(test_callback_ex_modbus PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_callback_ex_modbus PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_callback_ex_modbus PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
