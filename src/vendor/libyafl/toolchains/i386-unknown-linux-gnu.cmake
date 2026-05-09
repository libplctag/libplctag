# CMake toolchain configuration for i386-unknown-linux-gnu
# Copyright Kyle Hayes (2026)
# Distributed under the Boost Software License, Version 1.0.
# See LICENSE file for details

set(ASM_FILES
    src/asm/i386/make_context_i386_sysv_elf_gas.S
    src/asm/i386/switch_i386_sysv_elf_gas.S
)
enable_language(ASM)
add_compile_options(-Wall -Wextra -Werror -g -O2 -m32)