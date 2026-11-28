message("Building alloc_count.")

# Interposes the allocator, so it only makes sense where the allocator is a shared library and
# is not ASan's (no sanitizer build).  See alloc_count.c.
if(UNIX AND NOT CLIENT_SANITIZE_FLAGS)
    add_library(alloc_count SHARED ${CMAKE_CURRENT_SOURCE_DIR}/alloc_count.c)
    target_link_libraries(alloc_count PRIVATE ${CMAKE_DL_LIBS})
    # the toolchain hides symbols by default; the replacements and alloc_count_mark must be found
    target_compile_options(alloc_count PRIVATE -fvisibility=default)
endif()
