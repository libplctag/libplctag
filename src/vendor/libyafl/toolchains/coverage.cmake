option(ENABLE_COVERAGE "Enable code coverage" OFF)

if(ENABLE_COVERAGE)
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        message(STATUS "Enabling GCC coverage")
        add_compile_options(-O0 -g --coverage)
        add_link_options(--coverage)

    elseif(CMAKE_C_COMPILER_ID STREQUAL "Clang" OR CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
        message(STATUS "Enabling Clang coverage with llvm-cov")
        # Use GCC-style coverage for lcov compatibility
        add_compile_options(-O0 -g --coverage)
        add_link_options(--coverage)

    elseif(MSVC)
        message(WARNING "Coverage enabled, but MSVC coverage is not supported in CI")
    else()
        message(WARNING "Code coverage not configured for this compiler: ${CMAKE_C_COMPILER_ID}")
    endif()
endif()
