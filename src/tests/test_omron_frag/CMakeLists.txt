message("Building test_omron_frag.")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_MINSIZEREL}")
endif()

set(TEST_OMRON_FRAG_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_omron_frag.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/compat_utils.c
)

add_executable(test_omron_frag ${TEST_OMRON_FRAG_FILES})

target_include_directories(test_omron_frag PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utils)

target_link_libraries(test_omron_frag PUBLIC plctag_static ${EXTRA_LINKER_LIBS})
target_link_options(test_omron_frag PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
