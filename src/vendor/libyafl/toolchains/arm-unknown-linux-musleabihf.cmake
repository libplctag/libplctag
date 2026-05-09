# CMake toolchain configuration for arm-unknown-linux-musleabihf
# Uses musl-cross pre-built toolchain from /opt/x-tools
# Compiler path must be specified: -DCMAKE_C_COMPILER=/opt/x-tools/arm-unknown-linux-musleabihf/bin/arm-unknown-linux-musleabihf-gcc
# Copyright Kyle Hayes (2026)
# Distributed under the Boost Software License, Version 1.0.
# See LICENSE file for details

set(ASM_FILES
    src/asm/arm/make_context_arm_aapcs_elf_gas.S
    src/asm/arm/switch_arm_aapcs_elf_gas.S
)
enable_language(ASM)
add_compile_options(-Wall -Wextra -Werror -g -O2 -static)
