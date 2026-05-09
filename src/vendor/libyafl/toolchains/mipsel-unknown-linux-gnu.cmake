# CMake toolchain configuration for mipsel-unknown-linux-gnu
# Copyright Kyle Hayes (2026)
# Distributed under the Boost Software License, Version 1.0.
# See LICENSE file for details

set(ASM_FILES
    src/asm/mips/make_context_mips32_o32_elf_gas.S
    src/asm/mips/switch_mips32_o32_elf_gas.S
)
enable_language(ASM)
add_compile_options(-Wall -Wextra -Werror -g -O2)