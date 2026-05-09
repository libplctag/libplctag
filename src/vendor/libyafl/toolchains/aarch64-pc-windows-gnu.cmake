# CMake toolchain configuration for aarch64-pc-windows-gnu
# Copyright Kyle Hayes (2026)
# Distributed under the Boost Software License, Version 1.0.
# See LICENSE file for details

set(ASM_FILES
    src/asm/arm64/make_context_arm64_aapcs_pe_armclang.S
    src/asm/arm64/switch_arm64_aapcs_pe_armclang.S
)
enable_language(ASM)
add_compile_options(-Wall -Wextra -Werror -g -O2)