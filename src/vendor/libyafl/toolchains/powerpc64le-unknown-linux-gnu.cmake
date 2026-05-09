# CMake toolchain configuration for powerpc64le-unknown-linux-gnu
# Copyright Kyle Hayes (2026)
# Distributed under the Boost Software License, Version 1.0.
# See LICENSE file for details

set(ASM_FILES
    src/asm/ppc64/make_context_ppc64_sysv_elf_gas.S
    src/asm/ppc64/switch_ppc64_sysv_elf_gas.S
)
enable_language(ASM)
add_compile_options(-Wall -Wextra -Werror -g -O2)