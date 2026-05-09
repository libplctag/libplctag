# CMake toolchain configuration for powerpc64le-unknown-linux-musl
# Uses musl-cross pre-built toolchain from /opt/x-tools
# Compiler path must be specified: -DCMAKE_C_COMPILER=/opt/x-tools/powerpc64le-unknown-linux-musl/bin/powerpc64le-unknown-linux-musl-gcc
# Copyright Kyle Hayes (2026)
# Distributed under the Boost Software License, Version 1.0.
# See LICENSE file for details

set(ASM_FILES
    src/asm/ppc64/make_context_ppc64_sysv_elf_gas.S
    src/asm/ppc64/switch_ppc64_sysv_elf_gas.S
)
enable_language(ASM)
add_compile_options(-Wall -Wextra -Werror -g -O2 -static)
