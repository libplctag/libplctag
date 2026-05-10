# CMake toolchain configuration for i386-pc-windows-msvc (MSVC 32-bit)
# Copyright Kyle Hayes (2026)
# Distributed under the Boost Software License, Version 1.0.
# See LICENSE file for details

set(ASM_FILES
    src/asm/i386/make_context_i386_ms_pe_masm.asm
    src/asm/i386/switch_i386_ms_pe_masm.asm
)

set(CMAKE_ASM_MASM_COMPILER ml.exe CACHE FILEPATH "x86 MASM assembler" FORCE)
enable_language(ASM_MASM)

foreach(asm_file ${ASM_FILES})
    set_source_files_properties(${asm_file} PROPERTIES LANGUAGE ASM_MASM)
endforeach()

add_compile_options($<$<COMPILE_LANGUAGE:C>:/W4>)
add_compile_options($<$<COMPILE_LANGUAGE:C>:/WX>)
