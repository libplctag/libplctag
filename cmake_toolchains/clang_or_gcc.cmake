

# using Clang or GCC

set(EXTRA_COMPILE_FLAGS_MINSIZEREL "${EXTRA_COMPILE_FLAGS_MINSIZEREL} -D__USE_POSIX=1 -D_XOPEN_SOURCE=700 -D_POSIX_C_SOURCE=200809L")
set(EXTRA_COMPILE_FLAGS_DEBUG "${EXTRA_COMPILE_FLAGS_DEBUG} -D__USE_POSIX=1 -D_XOPEN_SOURCE=700 -D_POSIX_C_SOURCE=200809L")

if(APPLE)
    set(EXTRA_COMPILE_FLAGS_MINSIZEREL "${EXTRA_COMPILE_FLAGS_MINSIZEREL} -D_DARWIN_C_SOURCE")
    set(EXTRA_COMPILE_FLAGS_DEBUG "${EXTRA_COMPILE_FLAGS_DEBUG} -D_DARWIN_C_SOURCE")
else()
    # Don't set static linker options if sanitizers are enabled in Debug mode
    if(NOT (USE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug"))
        set(STATIC_C_LINKER_OPTIONS "-static")
        set(STATIC_CXX_LINKER_OPTIONS "-static")
    endif()
endif()

if(USE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT MINGW)
    message("Building Debug with ASan and UBSan etc.")
    if(APPLE)
        SET(EXTRA_COMPILE_FLAGS_DEBUG "${EXTRA_COMPILE_FLAGS_DEBUG} -fsanitize=address -fsanitize=undefined")
    else()
        SET(EXTRA_COMPILE_FLAGS_DEBUG "${EXTRA_COMPILE_FLAGS_DEBUG} -fsanitize=address -fsanitize=leak -fsanitize=undefined")
    endif()
endif()

# check to see if we are building 32-bit or 64-bit
if(BUILD_32_BIT)
    set(EXTRA_COMPILE_FLAGS_MINSIZEREL "${EXTRA_COMPILE_FLAGS_MINSIZEREL} -m32")
    set(EXTRA_COMPILE_FLAGS_DEBUG "${EXTRA_COMPILE_FLAGS_DEBUG} -m32")
    set(EXTRA_LINK_FLAGS "-m32")
endif()

set(CMAKE_C_FLAGS_MINSIZEREL " -Os -DNDEBUG -Wall -pedantic -Wextra -Wconversion -fno-strict-aliasing -fvisibility=hidden -std=c11 ${EXTRA_COMPILE_FLAGS_MINSIZEREL}")
set(CMAKE_C_FLAGS_DEBUG " -O0 -g -Wall -pedantic -Wextra -Wconversion -fno-strict-aliasing -fvisibility=hidden -fno-omit-frame-pointer -std=c11 ${EXTRA_COMPILE_FLAGS_DEBUG}")

set(CMAKE_CXX_FLAGS_MINSIZEREL " -Os -DNDEBUG -Wall -pedantic -Wextra -Wconversion -fno-strict-aliasing -fvisibility=hidden ${EXTRA_COMPILE_FLAGS_MINSIZEREL}")
set(CMAKE_CXX_FLAGS_DEBUG " -O0 -g -Wall -pedantic -Wextra -Wconversion -fno-strict-aliasing -fvisibility=hidden -fno-omit-frame-pointer ${EXTRA_COMPILE_FLAGS_DEBUG}")

set(CMAKE_SHARED_LINKER_FLAGS_MINSIZEREL "")
set(CMAKE_STATIC_LINKER_FLAGS_MINSIZEREL "")

# message("EXTRA_LINKER_LIBS = ${EXTRA_LINKER_LIBS}")
# message("CMAKE_C_FLAGS_MINSIZEREL = ${CMAKE_C_FLAGS_MINSIZEREL}")
# message("CMAKE_C_FLAGS_DEBUG = ${CMAKE_C_FLAGS_DEBUG}")
