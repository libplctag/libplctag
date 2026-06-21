message("Building replay tool.")

set(REPLAY_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/replay.c
)

add_executable(replay ${REPLAY_FILES})
target_link_libraries(replay PUBLIC ${EXTRA_LINKER_LIBS})
target_link_options(replay PRIVATE ${STATIC_C_LINKER_OPTIONS} ${EXTRA_LINK_FLAGS})
