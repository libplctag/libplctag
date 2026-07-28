

# using Clang or GCC

set(EXTRA_COMPILE_FLAGS_MINSIZEREL "${EXTRA_COMPILE_FLAGS_MINSIZEREL} -D__USE_POSIX=1 -D_XOPEN_SOURCE=700 -D_POSIX_C_SOURCE=200809L")
set(EXTRA_COMPILE_FLAGS_DEBUG "${EXTRA_COMPILE_FLAGS_DEBUG} -D__USE_POSIX=1 -D_XOPEN_SOURCE=700 -D_POSIX_C_SOURCE=200809L")

if(APPLE)
    set(EXTRA_COMPILE_FLAGS_MINSIZEREL "${EXTRA_COMPILE_FLAGS_MINSIZEREL} -D_DARWIN_C_SOURCE")
    set(EXTRA_COMPILE_FLAGS_DEBUG "${EXTRA_COMPILE_FLAGS_DEBUG} -D_DARWIN_C_SOURCE")
else()
    # Don't set static linker options if sanitizers are enabled in Debug mode
    if(NOT (CMAKE_BUILD_TYPE STREQUAL "Debug" AND (USE_MEM_SANITIZERS OR USE_THREAD_SANITIZERS)))
        set(STATIC_C_LINKER_OPTIONS "-static")
        set(STATIC_CXX_LINKER_OPTIONS "-static-libgcc;-static-libstdc++")
    endif()
endif()

# ASan/UBSan/LeakSan and TSan cannot be linked together, so USE_THREAD_SANITIZERS takes a
# separate, mutually exclusive branch from USE_MEM_SANITIZERS.
#
# -fno-sanitize=function disables just the function-pointer-type-mismatch check within
# -fsanitize=undefined. This codebase's protocol/tag vtable dispatch and rc_alloc()
# destructors deliberately declare handlers taking a concrete tag/PLC pointer type (e.g.
# ab_tag_p) and assign them into fields typed for the generic base pointer (plc_tag_p);
# this is a standard, ABI-safe C idiom relying on the "common initial sequence" struct
# layout guarantee, but it technically mismatches the exact function pointer type, which
# -fsanitize=function flags as undefined behavior. Suppressed here rather than rewriting
# every protocol's function signatures to take the generic pointer type and cast internally.
# GCC's C front end doesn't recognize "function" as a valid -fno-sanitize= value (it's a
# Clang/C++-only check there), so only pass the flag when actually compiling with Clang.
if(CMAKE_C_COMPILER_ID STREQUAL "Clang" OR CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
    set(SANITIZE_NO_FUNCTION_FLAG "-fno-sanitize=function")
else()
    set(SANITIZE_NO_FUNCTION_FLAG "")
endif()

if(USE_THREAD_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT MINGW)
    message("Building Debug with ThreadSanitizer.")
    SET(EXTRA_COMPILE_FLAGS_DEBUG "${EXTRA_COMPILE_FLAGS_DEBUG} -fsanitize=thread -fsanitize=undefined ${SANITIZE_NO_FUNCTION_FLAG}")
elseif(USE_MEM_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT MINGW)
    message("Building Debug with ASan and UBSan etc.")
    if(APPLE)
        SET(EXTRA_COMPILE_FLAGS_DEBUG "${EXTRA_COMPILE_FLAGS_DEBUG} -fsanitize=address -fsanitize=undefined ${SANITIZE_NO_FUNCTION_FLAG}")
    else()
        SET(EXTRA_COMPILE_FLAGS_DEBUG "${EXTRA_COMPILE_FLAGS_DEBUG} -fsanitize=address -fsanitize=leak -fsanitize=undefined ${SANITIZE_NO_FUNCTION_FLAG}")
    endif()
endif()

# check to see if we are building 32-bit or 64-bit
if(BUILD_32_BIT)
    set(EXTRA_COMPILE_FLAGS_MINSIZEREL "${EXTRA_COMPILE_FLAGS_MINSIZEREL} -m32")
    set(EXTRA_COMPILE_FLAGS_DEBUG "${EXTRA_COMPILE_FLAGS_DEBUG} -m32")
    set(EXTRA_LINK_FLAGS "-m32")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -m32")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -m32")
endif()

set(CMAKE_C_FLAGS_MINSIZEREL " -Os -DNDEBUG -DPLCTAG_COMPILE_DEBUG_LEVEL=3 -Wall -pedantic -Wextra -Wconversion -fno-strict-aliasing -fvisibility=hidden -std=c11 ${EXTRA_COMPILE_FLAGS_MINSIZEREL}")
set(CMAKE_C_FLAGS_DEBUG " -O0 -g -Wall -DPLCTAG_COMPILE_DEBUG_LEVEL=${MAX_DEBUG_LEVEL} -pedantic -Wextra -Wconversion -fno-strict-aliasing -fvisibility=hidden -fno-omit-frame-pointer -std=c11 ${EXTRA_COMPILE_FLAGS_DEBUG}")

set(CMAKE_CXX_FLAGS_MINSIZEREL " -Os -DNDEBUG -DPLCTAG_COMPILE_DEBUG_LEVEL=3 -Wall -pedantic -Wextra -Wconversion -fno-strict-aliasing -fvisibility=hidden ${EXTRA_COMPILE_FLAGS_MINSIZEREL}")
set(CMAKE_CXX_FLAGS_DEBUG " -O0 -g -Wall -DPLCTAG_COMPILE_DEBUG_LEVEL=${MAX_DEBUG_LEVEL} -pedantic -Wextra -Wconversion -fno-strict-aliasing -fvisibility=hidden -fno-omit-frame-pointer ${EXTRA_COMPILE_FLAGS_DEBUG}")

set(CMAKE_SHARED_LINKER_FLAGS_MINSIZEREL "")
set(CMAKE_STATIC_LINKER_FLAGS_MINSIZEREL "")

# message("EXTRA_LINKER_LIBS = ${EXTRA_LINKER_LIBS}")
# message("CMAKE_C_FLAGS_MINSIZEREL = ${CMAKE_C_FLAGS_MINSIZEREL}")
# message("CMAKE_C_FLAGS_DEBUG = ${CMAKE_C_FLAGS_DEBUG}")
