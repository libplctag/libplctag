# CMake toolchain configuration for sparc64-unknown-linux-gnu
# Copyright Kyle Hayes (2026)
# Distributed under the Boost Software License, Version 1.0.
# See LICENSE file for details

set(ASM_FILES
    src/asm/sparc64/make_context_sparc64_sysv_elf_gas.S
    src/asm/sparc64/switch_sparc64_sysv_elf_gas.S
)
enable_language(ASM)
add_compile_options(-Wall -Wextra -Werror -g -O2)