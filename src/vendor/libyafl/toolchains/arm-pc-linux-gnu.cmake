# CMake toolchain configuration for arm-pc-linux-gnu
# Copyright Kyle Hayes (2026)
# Distributed under the Boost Software License, Version 1.0.
# See LICENSE file for details

set(ASM_FILES
    src/asm/arm/make_context_arm_aapcs_elf_gas.S
    src/asm/arm/switch_arm_aapcs_elf_gas.S
)
enable_language(ASM)
add_compile_options(-Wall -Wextra -Werror -g -O2)
