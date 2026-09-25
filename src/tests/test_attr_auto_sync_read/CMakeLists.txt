message("Building test_attr_auto_sync_read.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_ATTR_AUTO_SYNC_READ_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_attr_auto_sync_read.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_attr_auto_sync_read ${TEST_ATTR_AUTO_SYNC_READ_FILES})

target_include_directories(test_attr_auto_sync_read PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_attr_auto_sync_read PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_attr_auto_sync_read PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
