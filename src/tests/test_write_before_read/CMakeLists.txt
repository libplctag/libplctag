message("Building test_write_before_read.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_WRITE_BEFORE_READ_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_write_before_read.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_write_before_read ${TEST_WRITE_BEFORE_READ_FILES})

target_include_directories(test_write_before_read PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_write_before_read PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_write_before_read PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
