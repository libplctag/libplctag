message("Building test_modbus_multiple.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_MODBUS_MULTIPLE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_modbus_multiple.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_modbus_multiple ${TEST_MODBUS_MULTIPLE_FILES})

target_include_directories(test_modbus_multiple PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_modbus_multiple PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_modbus_multiple PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
