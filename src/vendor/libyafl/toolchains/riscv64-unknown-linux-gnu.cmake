# CMake toolchain configuration for riscv64-unknown-linux-gnu
# Copyright Kyle Hayes (2026)
# Distributed under the Boost Software License, Version 1.0.
# See LICENSE file for details

set(ASM_FILES
    src/asm/riscv64/make_context_riscv64_sysv_elf_gas.S
    src/asm/riscv64/switch_riscv64_sysv_elf_gas.S
)
enable_language(ASM)
add_compile_options(-Wall -Wextra -Werror -g -O2)