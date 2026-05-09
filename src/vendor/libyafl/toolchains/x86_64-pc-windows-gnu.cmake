# CMake toolchain configuration for x86_64-pc-windows-gnu
# Copyright Kyle Hayes (2026)
# Distributed under the Boost Software License, Version 1.0.
# See LICENSE file for details

set(ASM_FILES
    src/asm/x86_64/make_context_x86_64_ms_pe_gas.S
    src/asm/x86_64/switch_x86_64_ms_pe_gas.S
)
enable_language(ASM)
add_compile_options(-Wall -Wextra -Werror -g -O2)