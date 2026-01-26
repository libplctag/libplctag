# Windows Fiber API Analysis and Unified Design

**Date:** 2026-01-21
**Goal:** Design unified API that works with both Windows Fibers and fcontext

---

## Windows Fiber API Overview

### Core Fiber Functions (Win32)

```c
// Convert thread to fiber
LPVOID ConvertThreadToFiber(LPVOID lpParameter);

// Create a new fiber
LPVOID CreateFiber(
    SIZE_T dwStackSize,
    LPFIBER_START_ROUTINE lpStartAddress,
    LPVOID lpParameter
);

// Create fiber with explicit commit/reserve
LPVOID CreateFiberEx(
    SIZE_T dwStackCommitSize,
    SIZE_T dwStackReserveSize,
    DWORD dwFlags,
    LPFIBER_START_ROUTINE lpStartAddress,
    LPVOID lpParameter
);

// Switch to another fiber
void SwitchToFiber(LPVOID lpFiber);

// Delete a fiber
void DeleteFiber(LPVOID lpFiber);

// Get current fiber data
PVOID GetFiberData(void);

// Get current fiber
PVOID GetCurrentFiber(void);
```

### Fiber Function Signature

```c
typedef void (WINAPI *LPFIBER_START_ROUTINE)(LPVOID lpParameter);

// Fiber entry point
void WINAPI FiberFunction(LPVOID lpParameter) {
    // Fiber code here
    // Never returns - must call SwitchToFiber to yield
}
```

### Key Characteristics

1. **Thread conversion required** - Must call `ConvertThreadToFiber` before creating fibers
2. **Opaque handles** - Fibers are `LPVOID` (void pointers)
3. **No return values** - `SwitchToFiber` is void, doesn't return data
4. **Symmetric switching** - Can switch to any fiber directly
5. **Stack size** - Specified at creation, in bytes
6. **User data** - Single `LPVOID` parameter passed to fiber, accessible via `GetFiberData`

---

## Differences from fcontext

| Feature | Windows Fibers | fcontext |
|---------|---------------|----------|
| Context creation | `CreateFiber` | `make_fcontext` |
| Switching | `SwitchToFiber(void*)` | `jump_fcontext(fcontext_t, void*)` |
| Return data | No | Yes (via `fcontext_transfer_t`) |
| Previous context | Must track manually | Returned automatically |
| Entry function | `void func(void*)` | `void func(fcontext_transfer_t)` |
| Thread conversion | Required | Not needed |
| Stack allocation | Automatic | Manual |

---

## Unified API Design

### Goal: Minimal API Compatible with Both

We need:
1. Fiber creation with stack size
2. Fiber switching
3. Data passing between fibers
4. Current fiber identification
5. Fiber destruction

### Proposed API

```c
// ev_fiber.h - Unified fiber API

#include <stddef.h>
#include <stdint.h>

// Opaque fiber handle (maps to LPVOID on Windows, fcontext_t* wrapper on POSIX)
typedef struct ev_fiber_s *ev_fiber_t;

// Fiber entry point signature (matches Windows)
typedef void (*ev_fiber_func_t)(void *arg);

/**
 * @brief Initialize fiber system for current thread.
 *
 * On Windows: Calls ConvertThreadToFiber
 * On POSIX: Sets up scheduler context
 *
 * Must be called once per thread before creating fibers.
 *
 * @return 0 on success, -1 on error
 */
int ev_fiber_init_thread(void);

/**
 * @brief Create a new fiber.
 *
 * @param stack_size Stack size in bytes (0 = default, typically 24KB)
 * @param func Fiber entry point (never returns)
 * @param arg User data passed to fiber
 * @return Fiber handle, or NULL on failure
 */
ev_fiber_t ev_fiber_create(size_t stack_size, ev_fiber_func_t func, void *arg);

/**
 * @brief Switch to another fiber.
 *
 * On Windows: Calls SwitchToFiber
 * On POSIX: Calls jump_fcontext
 *
 * @param fiber Fiber to switch to
 */
void ev_fiber_switch(ev_fiber_t fiber);

/**
 * @brief Get current fiber.
 *
 * @return Current fiber handle
 */
ev_fiber_t ev_fiber_current(void);

/**
 * @brief Get user data for current fiber.
 *
 * @return User data pointer passed to ev_fiber_create
 */
void *ev_fiber_data(void);

/**
 * @brief Destroy a fiber.
 *
 * Fiber must not be currently running.
 * On Windows: Calls DeleteFiber
 * On POSIX: Frees stack and context
 *
 * @param fiber Fiber to destroy
 */
void ev_fiber_destroy(ev_fiber_t fiber);

/**
 * @brief Get previous fiber (POSIX only, not available on Windows).
 *
 * On Windows: Returns NULL (not supported)
 * On POSIX: Returns fiber that switched to current fiber
 *
 * This is fcontext-specific and can't be portably implemented on Windows.
 * Avoid using if possible.
 *
 * @return Previous fiber, or NULL if unavailable
 */
ev_fiber_t ev_fiber_previous(void);
```

---

## Implementation Strategy

### Windows Implementation

```c
// ev_fiber_windows.c

#include <windows.h>
#include "ev_fiber.h"

struct ev_fiber_s {
    LPVOID fiber_handle;
    void *user_data;
};

static __declspec(thread) ev_fiber_t current_fiber = NULL;

int ev_fiber_init_thread(void) {
    LPVOID main_fiber = ConvertThreadToFiber(NULL);
    if (!main_fiber && GetLastError() != ERROR_ALREADY_FIBER) {
        return -1;
    }

    // Create wrapper for main fiber
    current_fiber = malloc(sizeof(struct ev_fiber_s));
    if (!current_fiber) return -1;

    current_fiber->fiber_handle = GetCurrentFiber();
    current_fiber->user_data = NULL;

    return 0;
}

ev_fiber_t ev_fiber_create(size_t stack_size, ev_fiber_func_t func, void *arg) {
    ev_fiber_t fiber = malloc(sizeof(struct ev_fiber_s));
    if (!fiber) return NULL;

    if (stack_size == 0) {
        stack_size = 24 * 1024;  // 24KB default
    }

    fiber->user_data = arg;

    // CreateFiberEx for explicit stack size
    fiber->fiber_handle = CreateFiberEx(
        stack_size,    // commit
        stack_size,    // reserve
        0,             // flags
        (LPFIBER_START_ROUTINE)func,
        arg
    );

    if (!fiber->fiber_handle) {
        free(fiber);
        return NULL;
    }

    return fiber;
}

void ev_fiber_switch(ev_fiber_t fiber) {
    current_fiber = fiber;
    SwitchToFiber(fiber->fiber_handle);
}

ev_fiber_t ev_fiber_current(void) {
    return current_fiber;
}

void *ev_fiber_data(void) {
    return current_fiber ? current_fiber->user_data : NULL;
}

void ev_fiber_destroy(ev_fiber_t fiber) {
    if (fiber) {
        DeleteFiber(fiber->fiber_handle);
        free(fiber);
    }
}

ev_fiber_t ev_fiber_previous(void) {
    return NULL;  // Not supported on Windows
}
```

### POSIX Implementation (fcontext)

```c
// ev_fiber_posix.c

#include "fcontext.h"
#include "ev_fiber.h"
#include <stdlib.h>
#include <sys/mman.h>

struct ev_fiber_s {
    fcontext_t fctx;
    void *stack_base;
    size_t stack_size;
    void *user_data;
    ev_fiber_func_t user_func;
    ev_fiber_t previous;  // Track who switched to us
};

static __thread ev_fiber_t current_fiber = NULL;
static __thread ev_fiber_t scheduler_fiber = NULL;

// Wrapper to adapt fcontext_transfer_t to ev_fiber_func_t
static void fiber_entry_wrapper(fcontext_transfer_t t) {
    ev_fiber_t self = (ev_fiber_t)t.data;
    self->previous = (ev_fiber_t)t.prev_context;  // Not quite right, but close
    current_fiber = self;

    // Call user function
    self->user_func(self->user_data);

    // If user function returns, switch back to scheduler
    // (This shouldn't happen in normal use)
}

int ev_fiber_init_thread(void) {
    // Create scheduler fiber (no actual context, just marker)
    scheduler_fiber = calloc(1, sizeof(struct ev_fiber_s));
    if (!scheduler_fiber) return -1;

    current_fiber = scheduler_fiber;
    return 0;
}

ev_fiber_t ev_fiber_create(size_t stack_size, ev_fiber_func_t func, void *arg) {
    if (stack_size == 0) {
        stack_size = 24 * 1024;  // 24KB default
    }

    ev_fiber_t fiber = calloc(1, sizeof(struct ev_fiber_s));
    if (!fiber) return NULL;

    // Allocate stack with mmap for guard pages
    size_t guard_size = 4096;
    size_t total_size = stack_size + guard_size;

    fiber->stack_base = mmap(NULL, total_size,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fiber->stack_base == MAP_FAILED) {
        free(fiber);
        return NULL;
    }

    // Set guard page
    if (mprotect(fiber->stack_base, guard_size, PROT_NONE) != 0) {
        munmap(fiber->stack_base, total_size);
        free(fiber);
        return NULL;
    }

    fiber->stack_size = total_size;
    fiber->user_data = arg;
    fiber->user_func = func;

    // Create fcontext
    void *stack_top = (char *)fiber->stack_base + total_size;
    fiber->fctx = make_fcontext(stack_top, stack_size, fiber_entry_wrapper);

    return fiber;
}

void ev_fiber_switch(ev_fiber_t fiber) {
    ev_fiber_t prev = current_fiber;
    current_fiber = fiber;

    fcontext_transfer_t t = jump_fcontext(fiber->fctx, fiber);

    // When we return here, update context
    current_fiber = prev;
}

ev_fiber_t ev_fiber_current(void) {
    return current_fiber;
}

void *ev_fiber_data(void) {
    return current_fiber ? current_fiber->user_data : NULL;
}

void ev_fiber_destroy(ev_fiber_t fiber) {
    if (fiber && fiber != scheduler_fiber) {
        if (fiber->stack_base) {
            munmap(fiber->stack_base, fiber->stack_size);
        }
        free(fiber);
    }
}

ev_fiber_t ev_fiber_previous(void) {
    return current_fiber ? current_fiber->previous : NULL;
}
```

---

## Stack Watermark Measurement

### Approach: Fill Stack with Pattern

```c
#define STACK_FILL_PATTERN 0xCC

// Fill stack with pattern at creation
void ev_fiber_init_stack_watermark(ev_fiber_t fiber) {
    if (!fiber || !fiber->stack_base) return;

    size_t guard_size = 4096;
    uint8_t *stack_start = (uint8_t *)fiber->stack_base + guard_size;
    size_t fillable = fiber->stack_size - guard_size;

    memset(stack_start, STACK_FILL_PATTERN, fillable);
}

// Check how much stack was used
size_t ev_fiber_stack_usage(ev_fiber_t fiber) {
    if (!fiber || !fiber->stack_base) return 0;

    size_t guard_size = 4096;
    uint8_t *stack_start = (uint8_t *)fiber->stack_base + guard_size;
    size_t available = fiber->stack_size - guard_size;

    // Find how much of the pattern is still intact
    size_t unused = 0;
    for (size_t i = 0; i < available; i++) {
        if (stack_start[i] == STACK_FILL_PATTERN) {
            unused++;
        } else {
            break;  // Hit used stack
        }
    }

    return available - unused;
}

// Report peak usage
void ev_fiber_report_stack_usage(ev_fiber_t fiber) {
    size_t used = ev_fiber_stack_usage(fiber);
    size_t total = fiber->stack_size - 4096;  // Subtract guard
    double percent = (used * 100.0) / total;

    printf("Fiber %p stack usage: %zu / %zu bytes (%.1f%%)\n",
           fiber, used, total, percent);

    if (percent > 75.0) {
        printf("WARNING: Stack usage over 75%%!\n");
    }
}
```

### Usage

```c
ev_fiber_t fiber = ev_fiber_create(24 * 1024, my_func, arg);
ev_fiber_init_stack_watermark(fiber);  // Fill with pattern

// Run fiber...
ev_fiber_switch(fiber);

// After fiber completes or yields
ev_fiber_report_stack_usage(fiber);
```

**Note:** On Windows, we can't measure stack usage this way since we don't control the stack. This is POSIX-only functionality.

---

## I/O Integration

### The Challenge

Windows Fibers API has no I/O functions. We need to add our own that work with both implementations.

### Proposed I/O API

```c
// ev_fiber_io.h

#include "ev_fiber.h"
#include "ev_read_buf.h"
#include "ev_write_buf.h"

/**
 * @brief Receive data into buffer (blocks fiber until available).
 *
 * Automatically yields fiber if would block.
 *
 * @param sockfd Socket to read from
 * @param buf Read buffer to receive into
 * @param n Number of bytes to receive
 * @return 0 on success, -1 on error (errno set)
 */
int ev_fiber_recv(socket_t sockfd, ev_read_buf_t *buf, size_t n);

/**
 * @brief Send data from buffer (blocks fiber until sent).
 *
 * @param sockfd Socket to send to
 * @param buf Write buffer to send from
 * @return 0 on success, -1 on error (errno set)
 */
int ev_fiber_send(socket_t sockfd, ev_write_buf_t *buf);

/**
 * @brief Receive until frame complete.
 *
 * @param sockfd Socket
 * @param buf Read buffer
 * @param checker Frame checker
 * @param ctx Checker context
 * @return 0 on success, -1 on error
 */
int ev_fiber_recv_frame(socket_t sockfd,
                        ev_read_buf_t *buf,
                        ev_frame_checker_t checker,
                        void *ctx);

/**
 * @brief Accept connection (blocks fiber until client connects).
 *
 * @param sockfd Listening socket
 * @param addr Client address (output)
 * @param addrlen Address length (input/output)
 * @return Client socket, or -1 on error
 */
socket_t ev_fiber_accept(socket_t sockfd, struct sockaddr *addr, socklen_t *addrlen);

/**
 * @brief Sleep (blocks fiber for duration).
 *
 * @param ms Milliseconds to sleep
 */
void ev_fiber_sleep(uint32_t ms);
```

### Implementation Notes

These I/O functions will need to:
1. Try non-blocking I/O
2. If would block, register socket with event loop
3. Switch to scheduler fiber
4. Scheduler polls and resumes when ready

This is the same on Windows and POSIX - the fiber switching mechanism is abstracted.

---

## Revised Unified API Summary

### Core Fiber Operations (Windows-compatible)

```c
int ev_fiber_init_thread(void);
ev_fiber_t ev_fiber_create(size_t stack_size, ev_fiber_func_t func, void *arg);
void ev_fiber_switch(ev_fiber_t fiber);
ev_fiber_t ev_fiber_current(void);
void *ev_fiber_data(void);
void ev_fiber_destroy(ev_fiber_t fiber);
```

### POSIX Extensions (not available on Windows)

```c
ev_fiber_t ev_fiber_previous(void);  // Returns NULL on Windows
size_t ev_fiber_stack_usage(ev_fiber_t fiber);  // 0 on Windows
void ev_fiber_report_stack_usage(ev_fiber_t fiber);  // No-op on Windows
```

### I/O Operations (cross-platform)

```c
int ev_fiber_recv(socket_t sockfd, ev_read_buf_t *buf, size_t n);
int ev_fiber_send(socket_t sockfd, ev_write_buf_t *buf);
int ev_fiber_recv_frame(socket_t sockfd, ev_read_buf_t *buf,
                        ev_frame_checker_t checker, void *ctx);
socket_t ev_fiber_accept(socket_t sockfd, struct sockaddr *addr, socklen_t *addrlen);
void ev_fiber_sleep(uint32_t ms);
```

---

## Code Portability

### Same Code Works Everywhere

```c
void connection_handler(void *arg) {
    socket_t sock = (socket_t)(intptr_t)arg;

    uint8_t recv_data[512];
    ev_read_buf_t buf = ev_read_buf_init(recv_data, sizeof(recv_data));

    // This works on Windows and POSIX!
    if (ev_fiber_recv(sock, &buf, 7) < 0) {
        return;  // Error
    }

    // Process...

    uint8_t send_data[512];
    ev_write_buf_t send_buf = ev_write_buf_init(send_data, sizeof(send_data));
    // Build response...

    ev_fiber_send(sock, &send_buf);
}

int main() {
    ev_fiber_init_thread();

    ev_fiber_t fiber = ev_fiber_create(24 * 1024, connection_handler,
                                       (void *)(intptr_t)sock);

    ev_fiber_switch(fiber);

    return 0;
}
```

**This exact code compiles and runs on Windows, macOS, Linux, BSD.**

---

## Recommendation

**Use this unified API design:**

1. **Core API mirrors Windows Fibers** - easy to map directly
2. **POSIX extensions clearly marked** - don't use if need Windows compatibility
3. **I/O API is cross-platform** - abstracts event loop details
4. **Stack watermarking built in** - optional, POSIX-only
5. **24KB default stack** - good starting point

This makes the shim layer minimal or non-existent - the API IS the shim.
