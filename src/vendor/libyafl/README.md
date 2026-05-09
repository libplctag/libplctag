# yafl - Portable Fiber/Coroutine Library

## Build Status

### Linux (glibc)

| Architecture | Compiler | OS | Libc | Status | Coverage |
| ------------ | -------- | -- | ---- | ------ | -------- |
| x86_64 | GCC | Linux | glibc | [![x86_64-pc-linux-gnu](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/x86_64-pc-linux-gnu.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | [![Coverage](docs/coverage_linux_x86_64.svg)](docs/coverage_linux_x86_64.svg) |
| aarch64 | GCC | Linux | glibc | [![aarch64-pc-linux-gnu](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/aarch64-pc-linux-gnu.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | [![Coverage](docs/coverage_linux_aarch64.svg)](docs/coverage_linux_aarch64.svg) |
| ARM (32-bit) | GCC | Linux | glibc | [![arm-unknown-linux-gnueabihf](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/arm-unknown-linux-gnueabihf.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | - |
| RISC-V 64 | GCC | Linux | glibc | [![riscv64-unknown-linux-gnu](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/riscv64-unknown-linux-gnu.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | - |
| MIPS (32-bit LE) | GCC | Linux | glibc | [![mipsel-unknown-linux-gnu](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/mipsel-unknown-linux-gnu.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | - |
| MIPS (64-bit LE) | GCC | Linux | glibc | [![mips64el-unknown-linux-gnuabi64](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/mips64el-unknown-linux-gnuabi64.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | - |
| PowerPC 64 LE | GCC | Linux | glibc | [![powerpc64le-unknown-linux-gnu](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/powerpc64le-unknown-linux-gnu.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | - |
| PowerPC (32-bit) | GCC | Linux | glibc | [![powerpc-unknown-linux-gnu](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/powerpc-unknown-linux-gnu.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | - |
| s390x | GCC | Linux | glibc | [![s390x-ibm-linux-gnu](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/s390x-ibm-linux-gnu.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | - |
| SPARC 64 | GCC | Linux | glibc | [![sparc64-unknown-linux-gnu](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/sparc64-unknown-linux-gnu.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | - |
| i386 | GCC | Linux | glibc | [![i386-unknown-linux-gnu](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/i386-unknown-linux-gnu.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | - |

### Linux (Alpine Container, native musl)

| Architecture | Compiler | OS | Libc | Status | Coverage |
| ------------ | -------- | -- | ---- | ------ | -------- |
| x86_64 | GCC | Alpine Linux | musl | [![x86_64-alpine-linux](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/x86_64-alpine-linux.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | - |
| i386 | GCC | Alpine Linux | musl | [![i386-alpine-linux](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/i386-alpine-linux.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | - |
| aarch64 | GCC | Alpine Linux | musl | [![aarch64-alpine-linux](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/aarch64-alpine-linux.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | - |

### Linux (musl.cc Cross-Compiler, QEMU Emulation)

| Architecture | Compiler | OS | Libc | Status | Coverage |
| ------------ | -------- | -- | ---- | ------ | -------- |
| ARM (32-bit) | GCC | Linux | musl | [![armv7-musl-linux](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/armv7-musl-linux.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | - |
| PowerPC 64 LE | GCC | Linux | musl | [![ppc64le-musl-linux](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/ppc64le-musl-linux.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | - |
| s390x | GCC | Linux | musl | [![s390x-musl-linux](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/s390x-musl-linux.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | - |
| RISC-V 64 | GCC | Linux | musl | [![riscv64-musl-linux](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/riscv64-musl-linux.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | - |

### macOS/iOS

| Architecture | Compiler | OS | Libc | Status | Coverage |
| ------------ | -------- | -- | ---- | ------ | -------- |
| x86_64 | Clang | macOS | libc | [![x86_64-apple-darwin](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/x86_64-apple-darwin.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | [![Coverage](docs/coverage_macos_x86_64.svg)](docs/coverage_macos_x86_64.svg) |
| aarch64 | Clang | macOS | libc | [![aarch64-apple-darwin](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/aarch64-apple-darwin.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | [![Coverage](docs/coverage_macos_aarch64.svg)](docs/coverage_macos_aarch64.svg) |
| aarch64 | Clang | iOS | libc | [![aarch64-apple-ios](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/aarch64-apple-ios.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | - |

### Windows

| Architecture | Compiler | OS | Libc | Status | Coverage |
| ------------ | -------- | -- | ---- | ------ | -------- |
| x86_64 | MSVC | Windows | MSVCRT | [![x86_64-pc-windows-msvc](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/x86_64-pc-windows-msvc.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | [![Coverage](docs/coverage_windows_x86_64.svg)](docs/coverage_windows_x86_64.svg) |
| aarch64 | MSVC | Windows | MSVCRT | [![aarch64-pc-windows-msvc](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/aarch64-pc-windows-msvc.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | [![Coverage](docs/coverage_windows_aarch64.svg)](docs/coverage_windows_aarch64.svg) |
| x86_64 | GCC (MinGW) | Windows | MSVCRT | [![x86_64-pc-windows-gnu](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/x86_64-pc-windows-gnu.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | - |
| aarch64 | GCC (MinGW) | Windows | MSVCRT | [![aarch64-pc-windows-gnu](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/aarch64-pc-windows-gnu.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | - |

### Android

| Architecture | Compiler | OS | Libc | Status | Coverage |
| ------------ | -------- | -- | ---- | ------ | -------- |
| x86_64 | Clang | Android | bionic | [![x86_64-unknown-linux-android](https://raw.githubusercontent.com/kyle-github/libyafl/badges/status/x86_64-unknown-linux-android.svg)](https://github.com/kyle-github/libyafl/actions/workflows/ci.yml) | - |

**Latest Release:** v0.1.0

A portable, low-level C11 fiber/coroutine library derived from Boost.Context.

## Why Another Fiber Library?

I wanted to use coroutines/fibers for another project and searched on GitHub and elsewhere and did not find anything that was:

- Portable across many OSes, architectures and compilers.  Aiming for RTOS environments as well.
- Every release tested across a matrix of architectures and operating systems and compilers.
- Only C11 and assembly.
- CMake build system or easy creation of CMake build configuration.
- Minimal functionality.  Just fibers.
- Support for guard pages etc. on operating systems that support them and malloc on platforms that do not.
- The ability to measure stack usage.  This is critical for embedded projects.
- No dependency on POSIX ucontext.
- No other dependencies.
- Appears to be maintained.

Boost.Context is very close but aimed at C++ and in one case seems to require C++ code.  However, the project has large set of assembly for a wide variety of processor architectures.  I decided it would make a good test of using an LLM to wrap/refactor some code.

The C code and documentation was mostly generated by Claude Code.  The assembly is from the Boost.Context project and was written by humans.  I have touched/edited almost every file at some point.

## Features

- **Pure C11 implementation** - No C++ dependencies
- **Asymmetric coroutines (fibers)** - Simplified suspend/resume model
- **Guard pages** - Memory-efficient mmap-based stack with automatic overflow detection (crashing your program!)
- **Stack watermark checking** - Measure maximum stack usage
- **16-byte stack alignment** - ABI-compliant on x86_64 and ARM64
- **Page-aware allocation** - Handles 4KB (Linux/Windows) and 16KB (macOS ARM) pages
- **Cross-platform** - Linux, macOS, iOS, Android, and Windows
- **Zero external dependencies** - Only standard APIs (POSIX/Windows)

## Quick Start

```c
#include "yafl.h"

static void *my_fiber_func(void *arg) {
    printf("Fiber running with arg: %p\n", arg);

    /* Suspend and wait for resumption */
    void *data = yafl_fiber_suspend((void *)0x1111);
    printf("Resumed with: %p\n", data);

    return (void *)0x2222;  /* Final result */
}

int main(void) {
    /* Create fiber with virtual memory stack and guard pages */
    yafl_fiber_t *fiber = yafl_fiber_create(
        my_fiber_func,
        16 * 1024,
        YAFL_STACK_FLAGS_VMEM | YAFL_STACK_FLAGS_WATERMARK
    );

    /* Start fiber */
    void *result = yafl_fiber_resume(fiber, (void *)42);
    assert(result == (void *)0x1111);  /* Got result from suspend */

    /* Resume fiber */
    result = yafl_fiber_resume(fiber, (void *)0x5555);
    assert(result == (void *)0x2222);  /* Got final result */
    assert(yafl_fiber_status(fiber) == YAFL_FIBER_STATUS_COMPLETE);

    /* Cleanup */
    yafl_fiber_destroy(fiber);
    return 0;
}
```

## API Overview

### Core Functions

```c
/* Creation with flags for stack type and watermark */
yafl_fiber_t *yafl_fiber_create(yafl_fiber_fn fiber_fn, size_t stack_size,
                                yafl_stack_flags_t flags);

/* Resume a fiber (start or continue) */
void *yafl_fiber_resume(yafl_fiber_t *fiber, void *arg);

/* Suspend current fiber */
void *yafl_fiber_suspend(void *result);

/* Query fiber status */
yafl_fiber_status_t yafl_fiber_status(yafl_fiber_t *fiber);

/* Get maximum stack usage (if watermarked) */
size_t yafl_fiber_stack_high_watermark(yafl_fiber_t *fiber);

/* Cleanup */
void yafl_fiber_destroy(yafl_fiber_t *fiber);

/* Utilities */
size_t yafl_get_page_size(void);
```

### Flags

Choose stack allocation type and optional watermark:

```c
/* Virtual memory (guard pages for overflow detection) */
YAFL_STACK_FLAGS_VMEM

/* Malloc (simple allocation, no guard pages) */
YAFL_STACK_FLAGS_MALLOC

/* Optional: track stack usage with watermark pattern */
YAFL_STACK_FLAGS_WATERMARK

/* Examples */
YAFL_STACK_FLAGS_VMEM                              /* vmem, no watermark */
YAFL_STACK_FLAGS_VMEM | YAFL_STACK_FLAGS_WATERMARK /* vmem + watermark */
YAFL_STACK_FLAGS_MALLOC                            /* malloc, no watermark */
YAFL_STACK_FLAGS_MALLOC | YAFL_STACK_FLAGS_WATERMARK
```

### Status Values

```c
YAFL_FIBER_STATUS_ERR        /* Invalid fiber or error */
YAFL_FIBER_STATUS_SUSPENDED  /* Waiting to be resumed */
YAFL_FIBER_STATUS_RUNNING    /* Currently executing */
YAFL_FIBER_STATUS_COMPLETE   /* Finished execution */
```

## Stack Options

### Virtual Memory Stacks (Recommended)

```c
yafl_fiber_t *fiber = yafl_fiber_create(
    my_func,
    16 * 1024,
    YAFL_STACK_FLAGS_VMEM
);
```

**Advantages:**

- Guard pages detect overflow/underflow
- Memory efficient (address space reserved, minimal physical memory used)
- Automatic bounds checking (SIGSEGV/access violation on overflow)

**Implementation:**

- Linux/macOS: Uses `mmap()` + `mprotect()` with PROT_NONE guard pages
- Windows: Uses `VirtualAlloc()` with PAGE_NOACCESS guard pages

### Malloc Stacks

```c
yafl_fiber_t *fiber = yafl_fiber_create(
    my_func,
    16 * 1024,
    YAFL_STACK_FLAGS_MALLOC
);
```

**Advantages:**

- Simple allocation without guard page overhead
- Useful for constrained environments

**Limitations:**

- No overflow detection
- Stack overflows cause undefined behavior

## Stack Watermarking

Enable with `YAFL_STACK_FLAGS_WATERMARK` flag:

```c
yafl_fiber_t *fiber = yafl_fiber_create(
    my_func,
    16 * 1024,
    YAFL_STACK_FLAGS_VMEM | YAFL_STACK_FLAGS_WATERMARK
);

/* After fiber execution */
size_t used = yafl_fiber_stack_high_watermark(fiber);
printf("Stack used: %zu bytes\n", used);
```

**How it works:**

1. Stack is filled with pattern `0xA5` at creation
2. As fiber executes, pattern is overwritten
3. On completion, scan detects how many bytes were used
4. Result: accurate measurement of maximum stack depth

**Overhead:**

- Negligible runtime cost (only at creation/destruction)
- Additional physical memory allocation (fills entire stack initially)

## Asymmetric Coroutines

This library implements asymmetric fibers - a fiber can only suspend back to its resumer.

```text
      Main Thread
         |
      resume(fiber)
         |
         v
    [Fiber Running]
         |
      suspend()
         |
         v
      Main Thread
         |
      resume(fiber) again
         |
         v
    [Fiber Running Again]
         |
      return (complete)
         |
         v
      Main Thread
```

Not supported: Fiber A switching directly to Fiber B. Fibers always return to their resumer.

## Memory Layout

### Virtual Memory Stack

```text
┌──────────────────────────┐
│ Guard Page (PROT_NONE)   │
├──────────────────────────┤
│ Stack Space (N pages)    │  Writable
│                          │  Grows downward
├──────────────────────────┤
│ Guard Page (PROT_NONE)   │
└──────────────────────────┘
```

**Address Space:** (N+2) × page_size
**Physical Memory:** Minimal (typically ~1 page initially)

### Malloc Stack

```text
┌──────────────────────────┐
│ User-allocated block     │  No guard pages
│ (N + 256 bytes)          │  Simple heap allocation
└──────────────────────────┘
```

**Address Space:** N + 256 bytes
**Physical Memory:** Entire allocation

## Building

```bash
# Configure
cmake -B build

# Build
cmake --build build

# Test
cd build && ctest --output-on-failure
```

## Testing

Tests included:

- `test_yafl_basic` - Basic API functionality and flag combinations
- `test_yafl_suspend_resume` - Multiple suspend/resume cycles
- `test_yafl_guard` - Guard page overflow detection
- `test_yafl_many` - Scalability with 100 fibers

Run all tests:

```bash
cd build && ctest --output-on-failure
```

## Architecture Support

Tested on:

- x86_64 (Linux, macOS, Windows)
- ARM64 (Linux, macOS, iOS, Windows)
- ARM (Linux)
- RISC-V, MIPS, PowerPC (cross-compiled)

## Limitations

1. **Stack grows downward** - Required by implementation
2. **Entry function must use suspend/return** - Cannot return normally from fiber entry
3. **Not thread-safe** - Each thread needs its own fibers
4. **Asymmetric only** - No direct fiber-to-fiber switching

## License

Derived from Boost.Context, distributed under the Boost Software License 1.0.

See `LICENSE` file for details.

## References

- **Boost.Context**: [https://github.com/boostorg/context](https://github.com/boostorg/context)
- **POSIX**: [https://pubs.opengroup.org/onlinepubs/9699919799/](https://pubs.opengroup.org/onlinepubs/9699919799/)
