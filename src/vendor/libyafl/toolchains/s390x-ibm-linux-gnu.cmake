# CMake toolchain configuration for s390x-ibm-linux-gnu
# Copyright Kyle Hayes (2026)
# Distributed under the Boost Software License, Version 1.0.
# See LICENSE file for details

set(ASM_FILES
    src/asm/s390x/make_context_s390x_sysv_elf_gas.S
    src/asm/s390x/switch_s390x_sysv_elf_gas.S
)
enable_language(ASM)
add_compile_options(-Wall -Wextra -Werror -g -O2)