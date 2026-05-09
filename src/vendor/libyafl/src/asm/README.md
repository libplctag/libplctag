# Assembly Files Organization

This directory contains architecture-specific assembly code for context switching, extracted from Boost.Context.

## File Organization

```text
asm/
├── arm/             # 32-bit ARM
├── arm64/           # 64-bit ARM (AArch64)
├── i386/            # 32-bit Intel
├── loongarch64/     # 64-bit LoongArch
├── mips/            # 32-bit MIPS
├── mips64/          # 64-bit MIPS N64 ABI
├── ppc32/           # 32-bit PowerPC
├── ppc64/           # 64-bit PowerPC
├── riscv32/         # 32-bit RISC-V notes
├── riscv64/         # 64-bit RISC-V
├── s390x/           # IBM Z / s390x
├── sparc64/         # 64-bit SPARC
└── x86_64/          # 64-bit Intel/AMD
```

## File Naming Convention

Each architecture has two low-level entry points:

1. **make_context_*_*.S** - Create/initialize a context
   - Sets up stack frame and instruction pointer
   - Called once per new context
   - Maps to: `yafl_t yafl_make_context(void *sp, size_t size, yafl_fn_t fn)`

2. **switch_*_*.S** - Switch to a context
   - Saves current state, restores new context
   - Called frequently (on every context switch)
   - Maps to: `void *yafl_switch(yafl_t *save, yafl_t target, void *data)`

## ABI and OS-Specific Suffixes

Filenames follow the pattern: `[function]_[arch]_[abi]_[os]_[asm].S`

### Architectures

- `x86_64` - Intel/AMD 64-bit
- `arm64` - ARM 64-bit (AArch64)
- `i386` - Intel/AMD 32-bit
- `arm` - ARM 32-bit
- `loongarch64` - LoongArch 64-bit
- `mips` - MIPS 32-bit O32 ABI
- `mips64` - MIPS 64-bit N64 ABI
- `ppc32` - PowerPC 32-bit SysV ABI
- `ppc64` - PowerPC 64-bit SysV ABI
- `riscv64` - RISC-V 64-bit SysV ABI
- `s390x` - IBM Z / s390x SysV ABI
- `sparc64` - SPARC V9 64-bit SysV ABI

### Application Binary Interface (ABI)

- `sysv` - System V ABI (Linux, BSD, older UNIX)
- `aapcs` - ARM EABI / ARM Architecture Procedure Call Standard (ARM systems)
- `ms` - Microsoft x64 ABI (Windows - not used in this project)

### Operating System / Format

- `elf` - ELF executable format (Linux)
- `macho` - Mach-O executable format (macOS)
- `xcoff` - XCOFF executable format (IBM PowerPC AIX - not in tier 1)

### Assembler

- `gas` - GNU Assembler syntax
- `masm` - Microsoft Assembler (MASM) syntax - not used in this project
- `armasm` - ARM Assembler syntax - not used in this project

## Platform-Specific Selection

The build system automatically selects the correct files based on `uname -s` and `uname -m`:

### macOS (Darwin)

- **arm64**: `make_context_arm64_aapcs_macho_gas.S`, `switch_arm64_aapcs_macho_gas.S`
- **x86_64**: `make_context_x86_64_sysv_macho_gas.S`, `switch_x86_64_sysv_macho_gas.S`
- **i386**: `make_context_i386_sysv_macho_gas.S`, `switch_i386_sysv_macho_gas.S`
- **arm**: `make_context_arm_aapcs_macho_gas.S`, `switch_arm_aapcs_macho_gas.S`

### Linux

- **x86_64**: `make_context_x86_64_sysv_elf_gas.S`, `switch_x86_64_sysv_elf_gas.S`
- **aarch64**: `make_context_arm64_aapcs_elf_gas.S`, `switch_arm64_aapcs_elf_gas.S`
- **i386**: `make_context_i386_sysv_elf_gas.S`, `switch_i386_sysv_elf_gas.S`
- **armv7l**: `make_context_arm_aapcs_elf_gas.S`, `switch_arm_aapcs_elf_gas.S`

Additional Linux-targeted toolchain files in this repository select backend pairs for `mipsel`, `mips64el`, `powerpc`, `powerpc64le`, `riscv64`, `s390x`, and `sparc64`.

## Source and License

All files are extracted directly from Boost.Context repository:

<https://github.com/boostorg/context/tree/develop/src/asm>

Each file retains its original Boost Software License 1.0 header:

```c
/*
 * Boost.Context Library
 * Copyright Oliver Kowalke 2009.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 *  http://www.boost.org/LICENSE_1_0.txt)
 */
```

## How They Work (High Level)

### make_context_* Functions

Create a new execution context by:

1. Storing the stack pointer and context metadata
2. Setting up the instruction pointer to the entry function
3. Initializing CPU registers to safe values
4. Returning an opaque handle to the new context

### switch_* Functions

Context switch by:

1. Saving all CPU registers to current stack
2. Restoring all CPU registers from target context's stack
3. Returning to execution at target context's saved instruction pointer
4. Returning the caller-supplied `data` pointer directly while saving the previous context through the `save` pointer

### Calling Convention Details

The entry function for a new context receives the first resume argument directly in:

- **x86_64**: `rdi` register (System V ABI first argument)
- **arm64**: `x0` register (AAPCS first argument)
- **i386**: Stack parameter (32-bit calling convention)
- **arm**: `r0` register (ARM EABI first argument)

## Testing and Validation

All assembly files are tested by:

1. `test_yafl_basic` - Basic context creation and switching
2. `test_yafl_suspend_resume` - Suspend/resume handoff behavior
3. `test_yafl_guard` - Guard-page stack protection
4. `test_yafl_many` - Repeated fiber creation and switching
5. `test_yafl_watermark` - Watermark-based stack usage tracking

Tests are compiled with the selected architecture's assembly files and run on the target platform.

## Adding New Architectures

To add a new architecture (e.g., RISC-V):

1. Create new subdirectory: `riscv64/`
2. Copy assembly files from Boost.Context:
   - `make_context_riscv64_sysv_elf_gas.S`
   - `switch_riscv64_sysv_elf_gas.S`
3. Update `Makefile` to detect the new architecture
4. Update `CMakeLists.txt` similarly
5. Test on actual hardware or in emulation

## Current Repository Notes

- `riscv32/` currently contains notes only and is not selected by any checked-in toolchain file.
- Several backend directories beyond the host architecture are maintained primarily through assembly syntax checks and cross-target builds when the corresponding toolchains are available.

## References

- **Boost.Context Source**: <https://github.com/boostorg/context>
- **System V ABI**: <https://software.intel.com/en-us/sites/default/files/article/2016-08/x86-64-abi-0.99.pdf>
- **ARM EABI**: <https://github.com/ARM-software/abi-aa>
- **ELF Format**: <https://refspecs.linuxbase.org/elf/elf.pdf>
- **Mach-O Format**: <https://developer.apple.com/library/archive/documentation/DeveloperTools/Conceptual/MachORuntime/>
