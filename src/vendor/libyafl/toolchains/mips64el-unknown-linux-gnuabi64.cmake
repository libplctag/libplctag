# CMake toolchain configuration for mips64el-unknown-linux-gnuabi64
# Copyright Kyle Hayes (2026)
# Distributed under the Boost Software License, Version 1.0.
# See LICENSE file for details

set(ASM_FILES
    src/asm/mips64/make_context_mips64_n64_elf_gas.S
    src/asm/mips64/switch_mips64_n64_elf_gas.S
)
enable_language(ASM)
add_compile_options(-Wall -Wextra -Werror -g -O2)