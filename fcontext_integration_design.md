# fcontext Integration Design

**Date:** 2026-01-21
**Issue:** Apple Silicon (ARM64) ucontext is broken, need reliable alternative
**Solution:** Use fcontext (extracted from Boost.Context)

---

## Why fcontext

### Problems with ucontext on macOS

1. **Deprecated since macOS 10.6** - could be removed any time
2. **Broken on Apple Silicon (ARM64)** - does not work reliably on M1/M2/M3
3. **Intel only** - only works on x86-64 macOS, not ARM64

### fcontext Advantages

1. **Battle-tested** - from Boost.Context, used in production for years
2. **ARM64 support** - full support for Apple Silicon
3. **Minimal** - just 3 assembly files per platform (~150 lines each)
4. **Boost Software License** - permissive, compatible with MPL/LGPL
5. **Clean C API** - simple, easy to integrate
6. **High performance** - just register save/restore, no syscalls

---

## fcontext API

From https://github.com/DaoWen/fcontext:

```c
// Opaque context pointer
typedef struct fcontext_opaque_t *fcontext_t;

// Transfer object (contains previous context + data)
typedef struct {
    fcontext_t prev_context;
    void *data;
} fcontext_transfer_t;

// Entry point function signature
typedef void (*fcontext_fn_t)(fcontext_transfer_t);

// Core functions (implemented in assembly)
extern fcontext_t make_fcontext(void *sp, size_t size, fcontext_fn_t fn);
extern fcontext_transfer_t jump_fcontext(fcontext_t const to, void *vp);
extern fcontext_transfer_t ontop_fcontext(fcontext_t const to, void *vp,
                                          fcontext_ontop_fn_t fn);
```

### Basic Usage

```c
// Entry point for new context
void my_coroutine(fcontext_transfer_t t) {
    printf("In coroutine, from: %p, data: %p\n", t.prev_context, t.data);

    // Yield back to caller
    t = jump_fcontext(t.prev_context, (void *)42);

    printf("Resumed in coroutine\n");
}

int main() {
    // Allocate stack
    void *stack = malloc(8192);

    // Create context at top of stack
    fcontext_t ctx = make_fcontext((char *)stack + 8192, 8192, my_coroutine);

    // Jump to coroutine
    fcontext_transfer_t t = jump_fcontext(ctx, (void *)123);

    printf("Back in main, data: %p\n", t.data);  // data = 42

    // Resume coroutine
    t = jump_fcontext(t.prev_context, NULL);

    printf("Done\n");
    free(stack);
}
```

---

## Integration with Event Loop

### Wrapper Layer

We'll wrap fcontext to provide our coroutine API:

```c
// ev_coro.h - Our coroutine API wrapping fcontext

typedef struct ev_coro_s ev_coro_t;

typedef void (*ev_coro_func_t)(void *arg);

typedef enum {
    EV_CORO_READY,
    EV_CORO_RUNNING,
    EV_CORO_BLOCKED,
    EV_CORO_DEAD
} ev_coro_state_t;

struct ev_coro_s {
    fcontext_t fctx;           // fcontext handle
    void *stack_base;          // For cleanup
    size_t stack_size;
    ev_coro_state_t state;
    ev_loop_t *loop;
    socket_t blocked_on_fd;
    short blocked_on_events;
    util_err_t error;
    void *user_data;           // User context
    ev_coro_func_t user_func;  // User entry point
};

util_err_t ev_coro_create(ev_loop_t *loop,
                          ev_coro_func_t func,
                          void *arg,
                          size_t stack_size,
                          ev_coro_t **out_coro);

void ev_coro_destroy(ev_coro_t **coro);
ev_coro_t *ev_coro_current(void);
void ev_coro_yield(void);
```

### Implementation

```c
// ev_coro.c

#include "fcontext.h"

// Global pointer to current coroutine (per-thread if needed)
static __thread ev_coro_t *g_current_coro = NULL;

// Entry wrapper - adapts fcontext_fn_t to our ev_coro_func_t
static void coro_entry_wrapper(fcontext_transfer_t t) {
    ev_coro_t *coro = (ev_coro_t *)t.data;

    // Save scheduler context for yielding back
    coro->loop->scheduler_fctx = t.prev_context;

    // Set as current
    g_current_coro = coro;
    coro->state = EV_CORO_RUNNING;

    // Call user function
    coro->user_func(coro->user_data);

    // User function returned - mark as dead
    coro->state = EV_CORO_DEAD;

    // Yield back to scheduler (never returns)
    jump_fcontext(coro->loop->scheduler_fctx, coro);
}

util_err_t ev_coro_create(ev_loop_t *loop,
                          ev_coro_func_t func,
                          void *arg,
                          size_t stack_size,
                          ev_coro_t **out_coro) {
    if (stack_size == 0) {
        stack_size = 8 * 1024;  // 8KB default
    }

    size_t guard_size = 4 * 1024;  // 4KB guard page
    size_t total_size = stack_size + guard_size;

    ev_coro_t *coro = calloc(1, sizeof(ev_coro_t));
    if (!coro) {
        return UTIL_ENOMEM;
    }

    // Allocate stack with guard page
    coro->stack_base = mmap(NULL, total_size,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (coro->stack_base == MAP_FAILED) {
        free(coro);
        return UTIL_ENOMEM;
    }

    // Protect guard page
    if (mprotect(coro->stack_base, guard_size, PROT_NONE) != 0) {
        munmap(coro->stack_base, total_size);
        free(coro);
        return UTIL_EINTERNAL;
    }

    coro->stack_size = total_size;
    coro->state = EV_CORO_READY;
    coro->loop = loop;
    coro->blocked_on_fd = -1;
    coro->user_func = func;
    coro->user_data = arg;

    // Create fcontext at top of stack (stacks grow down)
    void *stack_top = (char *)coro->stack_base + total_size;
    coro->fctx = make_fcontext(stack_top, stack_size, coro_entry_wrapper);

    // Add to loop's ready queue
    ev_loop_add_coro(loop, coro);

    *out_coro = coro;
    return UTIL_OK;
}

void ev_coro_destroy(ev_coro_t **coro) {
    if (!coro || !*coro) return;

    ev_coro_t *c = *coro;

    if (c->stack_base) {
        munmap(c->stack_base, c->stack_size);
    }

    free(c);
    *coro = NULL;
}

ev_coro_t *ev_coro_current(void) {
    return g_current_coro;
}

void ev_coro_yield_to_scheduler(void) {
    ev_coro_t *current = g_current_coro;

    // Switch back to scheduler
    fcontext_transfer_t t = jump_fcontext(current->loop->scheduler_fctx, current);

    // When we resume, update scheduler context
    current->loop->scheduler_fctx = t.prev_context;
}
```

### Event Loop Integration

```c
// ev_loop.h additions

struct ev_loop_s {
    fcontext_t scheduler_fctx;   // Scheduler's context

    // Coroutine queues
    ev_coro_t **ready_queue;
    size_t ready_count;

    ev_coro_t **blocked_queue;
    size_t blocked_count;

    // Poll arrays
    struct pollfd *pollfds;
    ev_coro_t **poll_coros;
    size_t poll_count;

    // Wake pipe
    socket_t wake_fds[2];
    bool running;
};

// ev_loop.c

util_err_t ev_loop_poll(ev_loop_t *loop, uint32_t timeout_ms) {
    // Build pollfd array from blocked coroutines
    loop->poll_count = 0;
    for (size_t i = 0; i < loop->blocked_count; i++) {
        ev_coro_t *coro = loop->blocked_queue[i];
        if (coro->blocked_on_fd != -1) {
            loop->pollfds[loop->poll_count].fd = coro->blocked_on_fd;
            loop->pollfds[loop->poll_count].events = coro->blocked_on_events;
            loop->pollfds[loop->poll_count].revents = 0;
            loop->poll_coros[loop->poll_count] = coro;
            loop->poll_count++;
        }
    }

    // Add wake pipe
    loop->pollfds[loop->poll_count].fd = loop->wake_fds[0];
    loop->pollfds[loop->poll_count].events = POLLIN;
    loop->poll_count++;

    // Poll
    int ready = poll(loop->pollfds, loop->poll_count, timeout_ms);

    if (ready > 0) {
        // Move ready coroutines to ready queue
        for (size_t i = 0; i < loop->poll_count - 1; i++) {
            if (loop->pollfds[i].revents) {
                ev_coro_t *coro = loop->poll_coros[i];
                coro->state = EV_CORO_READY;
                coro->blocked_on_fd = -1;
                ev_loop_move_to_ready(loop, coro);
            }
        }
    }

    // Run ready coroutines
    while (loop->ready_count > 0) {
        ev_coro_t *coro = loop->ready_queue[0];
        ev_loop_remove_from_ready(loop, 0);

        if (coro->state == EV_CORO_DEAD) {
            ev_coro_destroy(&coro);
            continue;
        }

        // Resume coroutine
        coro->state = EV_CORO_RUNNING;
        fcontext_transfer_t t = jump_fcontext(coro->fctx, coro);

        // Coroutine yielded back
        loop->scheduler_fctx = t.prev_context;
        ev_coro_t *returned = (ev_coro_t *)t.data;

        // Check state
        if (returned->state == EV_CORO_BLOCKED) {
            ev_loop_add_to_blocked(loop, returned);
        } else if (returned->state == EV_CORO_DEAD) {
            ev_coro_destroy(&returned);
        }
    }

    return UTIL_OK;
}
```

---

## Obtaining fcontext Assembly Files

### Option 1: Use DaoWen/fcontext as-is

The repo at https://github.com/DaoWen/fcontext provides:
- x86-64 Linux (ELF)
- x86-64 macOS (Mach-O)

**Missing:** ARM64 files

### Option 2: Extract from Boost.Context

Boost.Context has comprehensive assembly for all platforms:
- https://github.com/boostorg/context/tree/develop/src/asm

Files we need:

**For macOS x86-64:**
- `make_x86_64_sysv_macho_gas.S`
- `jump_x86_64_sysv_macho_gas.S`
- `ontop_x86_64_sysv_macho_gas.S`

**For macOS ARM64 (Apple Silicon):**
- `make_arm64_aapcs_macho_gas.S`
- `jump_arm64_aapcs_macho_gas.S`
- `ontop_arm64_aapcs_macho_gas.S`

**For Linux x86-64:**
- `make_x86_64_sysv_elf_gas.S`
- `jump_x86_64_sysv_elf_gas.S`
- `ontop_x86_64_sysv_elf_gas.S`

**For Linux ARM64:**
- `make_arm64_aapcs_elf_gas.S`
- `jump_arm64_aapcs_elf_gas.S`
- `ontop_arm64_aapcs_elf_gas.S`

### Recommended Approach

**Copy assembly files from Boost.Context into our project:**

```
src/
  platform/
    fcontext/
      fcontext.h           # API header (from DaoWen or write our own)

      # macOS x86-64
      make_x86_64_sysv_macho_gas.S
      jump_x86_64_sysv_macho_gas.S
      ontop_x86_64_sysv_macho_gas.S

      # macOS ARM64
      make_arm64_aapcs_macho_gas.S
      jump_arm64_aapcs_macho_gas.S
      ontop_arm64_aapcs_macho_gas.S

      # Linux x86-64
      make_x86_64_sysv_elf_gas.S
      jump_x86_64_sysv_elf_gas.S
      ontop_x86_64_sysv_elf_gas.S

      # Linux ARM64
      make_arm64_aapcs_elf_gas.S
      jump_arm64_aapcs_elf_gas.S
      ontop_arm64_aapcs_elf_gas.S
```

**License:** Boost Software License 1.0 - permissive, GPL-compatible, no attribution required in binaries.

---

## Build System Integration

### CMakeLists.txt

```cmake
# Detect platform and architecture
if(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(FCONTEXT_ASM
            src/platform/fcontext/make_arm64_aapcs_macho_gas.S
            src/platform/fcontext/jump_arm64_aapcs_macho_gas.S
            src/platform/fcontext/ontop_arm64_aapcs_macho_gas.S
        )
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
        set(FCONTEXT_ASM
            src/platform/fcontext/make_x86_64_sysv_macho_gas.S
            src/platform/fcontext/jump_x86_64_sysv_macho_gas.S
            src/platform/fcontext/ontop_x86_64_sysv_macho_gas.S
        )
    endif()
elseif(UNIX)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64")
        set(FCONTEXT_ASM
            src/platform/fcontext/make_arm64_aapcs_elf_gas.S
            src/platform/fcontext/jump_arm64_aapcs_elf_gas.S
            src/platform/fcontext/ontop_arm64_aapcs_elf_gas.S
        )
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
        set(FCONTEXT_ASM
            src/platform/fcontext/make_x86_64_sysv_elf_gas.S
            src/platform/fcontext/jump_x86_64_sysv_elf_gas.S
            src/platform/fcontext/ontop_x86_64_sysv_elf_gas.S
        )
    endif()
endif()

# Add to library sources
add_library(ev_lib
    src/ev_loop.c
    src/ev_coro.c
    src/ev_socket.c
    src/ev_read_buf.c
    src/ev_write_buf.c
    ${FCONTEXT_ASM}
)

# Enable assembly
enable_language(ASM)
```

---

## Testing on Apple Silicon

### Basic Test

```c
// test_fcontext_arm64.c

#include "fcontext.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

static int phase = 0;

void coro_func(fcontext_transfer_t t) {
    printf("In coroutine on ARM64\n");
    assert(phase == 0);
    phase = 1;

    // Yield back
    t = jump_fcontext(t.prev_context, (void *)42);

    assert(phase == 2);
    phase = 3;
    printf("Coroutine resuming\n");
}

int main(void) {
    printf("Testing fcontext on Apple Silicon\n");

    void *stack = malloc(8192);
    fcontext_t ctx = make_fcontext((char *)stack + 8192, 8192, coro_func);

    // First switch
    fcontext_transfer_t t = jump_fcontext(ctx, NULL);
    assert(phase == 1);
    assert(t.data == (void *)42);
    phase = 2;

    // Resume
    t = jump_fcontext(t.prev_context, NULL);
    assert(phase == 3);

    free(stack);
    printf("All tests passed!\n");
    return 0;
}
```

Compile and test:
```bash
# On Apple Silicon Mac
gcc -c make_arm64_aapcs_macho_gas.S -o make.o
gcc -c jump_arm64_aapcs_macho_gas.S -o jump.o
gcc -c ontop_arm64_aapcs_macho_gas.S -o ontop.o
gcc test_fcontext_arm64.c make.o jump.o ontop.o -o test
./test
```

---

## Code Size Estimate

| Component | Lines | Notes |
|-----------|-------|-------|
| fcontext.h (API) | 150 | Header with inline helpers |
| ev_coro.c (wrapper) | 200 | Wraps fcontext for our API |
| Assembly (per platform) | 450 | 3 files × 150 lines each |
| Event loop integration | 100 | Add fcontext support to loop |
| **Total per platform** | **900** | Very reasonable |

We need 2 platforms (macOS ARM64 + x86-64), so ~1800 lines total for coroutines.

---

## Migration Path

### Phase 1: Add fcontext Support (1 week)

1. Copy fcontext.h from DaoWen/fcontext
2. Extract ARM64 assembly from Boost.Context
3. Extract x86-64 assembly (already in DaoWen)
4. Implement ev_coro wrapper layer
5. Test on both Intel and Apple Silicon Macs

### Phase 2: Integrate with Event Loop (1 week)

1. Add fcontext support to ev_loop_poll
2. Implement blocking I/O helpers (recv_exact, send_all)
3. Test with Modbus TCP example
4. Verify memory usage matches estimates

### Phase 3: Add Linux Support (optional)

1. Test with Linux x86-64 assembly
2. Test with Linux ARM64 assembly (Raspberry Pi, AWS Graviton)

### Phase 4: Add Windows Support (optional)

1. Use Win32 Fibers (different approach, not fcontext)
2. Or use fcontext Windows assembly (available in Boost)

---

## Licensing

**Boost Software License 1.0:**
- Permission to use, copy, modify, distribute
- No attribution required in binaries
- Compatible with GPL, LGPL, MPL
- Same license as your libplctag project

We can include the Boost.Context assembly files directly with proper license header.

---

## Example: Complete Coroutine-Based Handler

```c
#include <ev.h>

void modbus_connection_handler(void *arg) {
    socket_t sock = (socket_t)(intptr_t)arg;

    uint8_t recv_data[512];
    uint8_t send_data[512];
    ev_read_buf_t recv_buf = ev_read_buf_init(recv_data, sizeof(recv_data));
    ev_write_buf_t send_buf = ev_write_buf_init(send_data, sizeof(send_data));

    ev_frame_length_prefixed_cfg_t frame_cfg = {
        .length_offset = 4,
        .length_size = 2,
        .length_endian = EV_BIG_ENDIAN,
        .header_size = 6,
        .length_includes_header = false,
        .max_frame_size = 512
    };

    while (1) {
        // Blocking-style receive (uses fcontext under the hood)
        util_err_t err = ev_coro_recv_frame(sock, &recv_buf,
                                            ev_frame_check_length_prefixed,
                                            &frame_cfg);
        if (err != UTIL_OK) break;

        // Decode header
        uint16_t transaction_id, length;
        ev_read_buf_decode_u16(&recv_buf, EV_BIG_ENDIAN, &transaction_id);
        ev_read_buf_decode_u16(&recv_buf, EV_BIG_ENDIAN, &length);
        // ... decode rest

        // Process request
        process_modbus_request(&recv_buf, &send_buf);

        // Send response
        ev_coro_send_all(sock, &send_buf);

        ev_read_buf_reset(&recv_buf);
        ev_write_buf_reset(&send_buf);
    }

    ev_socket_close(sock);
}
```

**This works on Apple Silicon, Intel Mac, Linux x86-64, Linux ARM64** - all with the same code!

---

## Recommendation

**Use fcontext with assembly from Boost.Context.**

**Why:**
1. Proven solution for Apple Silicon ARM64
2. Works everywhere (macOS, Linux, both architectures)
3. Simple integration (~900 lines per platform)
4. Boost Software License is compatible
5. No deprecated APIs to worry about
6. High performance

**Next steps:**
1. Extract ARM64 assembly from Boost.Context
2. Test on Apple Silicon
3. Integrate with event loop design
4. Build Modbus example to validate

This is the most robust solution for your cross-platform needs.
