# CMake toolchain configuration for x86_64-pc-windows-msvc
# Copyright Kyle Hayes (2026)
# Distributed under the Boost Software License, Version 1.0.
# See LICENSE file for details

set(ASM_FILES
    src/asm/x86_64/make_context_x86_64_ms_pe_masm.asm
    src/asm/x86_64/switch_x86_64_ms_pe_masm.asm
)

set(CMAKE_ASM_MASM_COMPILER ml64.exe CACHE FILEPATH "x64 MASM assembler" FORCE)
enable_language(ASM_MASM)

foreach(asm_file ${ASM_FILES})
    set_source_files_properties(${asm_file} PROPERTIES LANGUAGE ASM_MASM)
endforeach()

add_compile_options($<$<COMPILE_LANGUAGE:C>:/W4>)
add_compile_options($<$<COMPILE_LANGUAGE:C>:/WX>)