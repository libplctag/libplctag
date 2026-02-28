# macOS Coroutine Implementation Alternatives

**Date:** 2026-01-21
**Issue:** macOS deprecated ucontext, need reliable alternatives

---

## Status of ucontext on macOS

### Current State (Tested on macOS)
- **Still works** despite deprecation warnings
- Requires `_XOPEN_SOURCE` define
- Functions correctly on both Intel and Apple Silicon
- Test passes with gcc and clang

### Concerns
- Deprecated since 10.6 (2009)
- Could be removed in future macOS versions
- Apple recommends against use
- May have subtle bugs with signal handlers

**Conclusion:** Works now, but risky to depend on long-term.

---

## Alternative 1: setjmp/longjmp with Manual Stack Switching

This is the most portable approach, but requires some assembly for stack switching.

### Concept

```c
#include <setjmp.h>

typedef struct {
    jmp_buf jmp;
    void *stack_base;
    void *stack_ptr;
    size_t stack_size;
    /* ... */
} coro_t;
```

**Problem:** `setjmp/longjmp` save/restore registers but NOT the stack pointer.

**Solution:** Manually switch stack pointer in assembly before `longjmp`.

### Implementation Sketch

```c
/* Platform-specific assembly for stack switching */

#if defined(__x86_64__)
/* x86-64 (Intel Mac, Linux x86-64) */

static inline void coro_switch(coro_t *from, coro_t *to) {
    if (setjmp(from->jmp) == 0) {
        /* Switch stack pointer */
        __asm__ volatile (
            "movq %0, %%rsp\n\t"
            : /* no outputs */
            : "r" (to->stack_ptr)
            : "memory"
        );
        longjmp(to->jmp, 1);
    }
}

static inline void coro_init_stack(coro_t *coro, void (*func)(void *), void *arg) {
    /* Point to top of stack (grows down) */
    uint64_t *stack = (uint64_t *)((char *)coro->stack_base + coro->stack_size);

    /* Push argument */
    *(--stack) = (uint64_t)arg;

    /* Push return address (should never be reached) */
    *(--stack) = 0;

    /* Push function address */
    *(--stack) = (uint64_t)func;

    /* Leave space for saved registers (setjmp will fill these) */
    stack -= 8;  /* 8 registers typically saved */

    coro->stack_ptr = stack;

    /* Set up jmp_buf to point to function */
    setjmp(coro->jmp);

    /* Manually set instruction pointer in jmp_buf */
    /* This is platform-specific and fragile */
#if defined(__APPLE__)
    /* macOS jmp_buf layout (may vary by version!) */
    coro->jmp[21] = (long)func;  /* RIP */
#endif
}

#elif defined(__aarch64__)
/* ARM64 (Apple Silicon) */

static inline void coro_switch(coro_t *from, coro_t *to) {
    if (setjmp(from->jmp) == 0) {
        /* Switch stack pointer */
        __asm__ volatile (
            "mov sp, %0\n\t"
            : /* no outputs */
            : "r" (to->stack_ptr)
            : "memory"
        );
        longjmp(to->jmp, 1);
    }
}

static inline void coro_init_stack(coro_t *coro, void (*func)(void *), void *arg) {
    /* ARM64 stack setup */
    uint64_t *stack = (uint64_t *)((char *)coro->stack_base + coro->stack_size);

    /* ARM64 calling convention: x0 = first arg */
    *(--stack) = (uint64_t)arg;
    *(--stack) = 0;  /* Link register (return address) */
    *(--stack) = (uint64_t)func;

    stack -= 16;  /* Space for saved registers */

    coro->stack_ptr = stack;

    setjmp(coro->jmp);

#if defined(__APPLE__)
    /* Apple Silicon jmp_buf layout */
    coro->jmp[13] = (long)func;  /* PC */
#endif
}

#else
#error "Unsupported architecture"
#endif
```

**Problems with this approach:**
1. **jmp_buf is opaque** - layout varies by platform/version
2. **Fragile** - breaks if OS changes jmp_buf layout
3. **Not officially supported** - relying on implementation details
4. **Still needs assembly** - not really avoiding the problem

---

## Alternative 2: Boost.Context-style Assembly

Write minimal assembly for context switching. This is what Boost.Context does.

### x86-64 Implementation

```asm
# save_context(void **sp)
.globl _save_context
_save_context:
    # Save callee-saved registers
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    # Save stack pointer
    movq %rsp, (%rdi)

    # Return 0 (first time)
    xorq %rax, %rax
    ret

# restore_context(void *sp)
.globl _restore_context
_restore_context:
    # Switch to new stack
    movq %rdi, %rsp

    # Restore callee-saved registers
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp

    # Return 1 (resumed)
    movq $1, %rax
    ret

# make_context(void *stack_top, void (*func)(void*), void *arg)
.globl _make_context
_make_context:
    # rdi = stack_top
    # rsi = func
    # rdx = arg

    # Set up stack frame
    movq %rdi, %rsp

    # Push argument for func
    pushq %rdx

    # Push function to call
    pushq %rsi

    # Reserve space for saved registers
    subq $48, %rsp

    # Return stack pointer
    movq %rsp, %rax
    ret
```

### ARM64 Implementation

```asm
# save_context(void **sp)
.globl _save_context
_save_context:
    # Save callee-saved registers
    stp x19, x20, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    stp x23, x24, [sp, #-16]!
    stp x25, x26, [sp, #-16]!
    stp x27, x28, [sp, #-16]!
    stp x29, x30, [sp, #-16]!  # fp, lr

    # Save stack pointer
    mov x9, sp
    str x9, [x0]

    # Return 0
    mov w0, #0
    ret

# restore_context(void *sp)
.globl _restore_context
_restore_context:
    # Switch to new stack
    mov sp, x0

    # Restore callee-saved registers
    ldp x29, x30, [sp], #16
    ldp x27, x28, [sp], #16
    ldp x25, x26, [sp], #16
    ldp x23, x24, [sp], #16
    ldp x21, x22, [sp], #16
    ldp x19, x20, [sp], #16

    # Return 1
    mov w0, #1
    ret

# make_context(void *stack_top, void (*func)(void*), void *arg)
.globl _make_context
_make_context:
    # x0 = stack_top
    # x1 = func
    # x2 = arg

    mov sp, x0

    # Push arg and func
    stp x2, x1, [sp, #-16]!

    # Reserve space for saved registers (6 pairs)
    sub sp, sp, #96

    # Return stack pointer
    mov x0, sp
    ret
```

### C Wrapper

```c
/* Implemented in assembly */
extern int save_context(void **sp);
extern void restore_context(void *sp);
extern void *make_context(void *stack_top, void (*func)(void *), void *arg);

typedef struct {
    void *sp;
    void *stack_base;
    size_t stack_size;
} coro_context_t;

void coro_switch(coro_context_t *from, coro_context_t *to) {
    if (save_context(&from->sp) == 0) {
        restore_context(to->sp);
    }
}

void coro_create(coro_context_t *ctx, void *stack, size_t size,
                 void (*func)(void *), void *arg) {
    ctx->stack_base = stack;
    ctx->stack_size = size;
    ctx->sp = make_context((char *)stack + size, func, arg);
}
```

**Advantages:**
- **Clean and minimal** - ~40 lines of assembly per architecture
- **No deprecated APIs** - pure assembly
- **Full control** - no opaque structures
- **Fast** - just register save/restore
- **Proven** - Boost.Context uses this approach

**Disadvantages:**
- **Assembly required** - but it's simple and well-understood
- **Per-architecture** - need x86-64, ARM64, maybe ARM32
- **Testing burden** - must test on each arch

---

## Alternative 3: libuv-style Thread Pool

Instead of coroutines, use a small thread pool with work queue.

```c
typedef struct {
    pthread_t threads[4];  /* 4 worker threads */
    work_queue_t queue;
    /* ... */
} thread_pool_t;

typedef struct {
    void (*func)(void *);
    void *arg;
} work_item_t;

void thread_pool_submit(thread_pool_t *pool, void (*func)(void *), void *arg) {
    work_item_t *item = malloc(sizeof(work_item_t));
    item->func = func;
    item->arg = arg;
    queue_push(&pool->queue, item);
}
```

**For blocking I/O in handlers:**
```c
void handle_connection(void *arg) {
    socket_t sock = (socket_t)(intptr_t)arg;

    /* Can just use blocking I/O - we're on a thread */
    uint8_t buf[512];
    ssize_t n = recv(sock, buf, sizeof(buf), 0);  /* Blocks thread, not whole process */

    /* Process... */
}
```

**Advantages:**
- No coroutines needed
- Simple implementation
- Portable (pthreads everywhere)
- Can actually use blocking I/O

**Disadvantages:**
- Limited concurrency (number of threads)
- More memory per connection (thread stack)
- Context switch overhead
- Not suitable for 1000s of connections

---

## Alternative 4: Hybrid Approach

Combine multiple strategies:

```c
/* On macOS: use assembly-based coroutines */
#if defined(__APPLE__)
    #include "coro_asm.h"
#elif defined(_WIN32)
    /* Windows: use fibers */
    #include "coro_fibers.h"
#elif defined(__linux__)
    /* Linux: try ucontext, fall back to assembly if not available */
    #if HAVE_UCONTEXT
        #include "coro_ucontext.h"
    #else
        #include "coro_asm.h"
    #endif
#else
    /* Other platforms: assembly */
    #include "coro_asm.h"
#endif
```

---

## Recommendation

### For Production Use: Assembly-Based (Alternative 2)

Implement minimal assembly context switching like Boost.Context:

**Reasons:**
1. **Not deprecated** - pure assembly, no deprecated APIs
2. **Minimal** - ~40 lines per architecture
3. **Fast** - just register save/restore, no syscalls
4. **Portable** - can support many architectures
5. **Proven** - widely used pattern (Boost, libuv, etc.)
6. **Future-proof** - won't break when Apple removes ucontext

**Implementation effort:**
- x86-64: ~40 lines assembly + ~50 lines C wrapper
- ARM64: ~40 lines assembly + same C wrapper
- Testing: Run on Intel Mac, Apple Silicon Mac, Linux x86-64, Linux ARM64

Total: ~200 lines including both architectures.

### Quick Prototype: Use ucontext with fallback

For rapid development and testing:

```c
#if defined(__APPLE__)
    #define _XOPEN_SOURCE
    #include <ucontext.h>
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wdeprecated-declarations"

    /* Use ucontext for now, mark for future replacement */
    #warning "Using deprecated ucontext - replace with assembly implementation"

    #pragma GCC diagnostic pop
#endif
```

Then migrate to assembly when ready for production.

---

## Example: Assembly Context Switching in Practice

Here's how simple the assembly actually is:

```c
// coro_asm.h
typedef struct {
    void *sp;  /* That's it - just a stack pointer */
} coro_context_t;

int coro_save(coro_context_t *ctx);      /* Returns 0 first time, 1 when resumed */
void coro_restore(coro_context_t *ctx);  /* Never returns */
void coro_make(coro_context_t *ctx, void *stack, size_t size,
               void (*func)(void *), void *arg);
```

**Usage is identical to ucontext:**

```c
void my_coroutine(void *arg) {
    printf("In coroutine\n");
    coro_yield();
    printf("Resumed\n");
}

coro_context_t main_ctx, coro_ctx;
uint8_t stack[8192];

coro_make(&coro_ctx, stack, sizeof(stack), my_coroutine, NULL);

if (coro_save(&main_ctx) == 0) {
    coro_restore(&coro_ctx);  /* Switch to coroutine */
}
/* When coroutine yields, we return here */

printf("Back in main\n");
```

The assembly is straightforward register save/restore - nothing exotic.

---

## Code from Other Projects

### libdill

The libdill project (by Martin Sustrik of ZeroMQ fame) uses pure assembly:
- https://github.com/sustrik/libdill
- Supports x86-64, ARM64, ARM32, MIPS, etc.
- ~50 lines per architecture
- Battle-tested in production

### Boost.Context

Industry standard:
- https://github.com/boostorg/context
- Comprehensive architecture support
- Highly optimized
- Can borrow/adapt their assembly

### minicoro

Single-header coroutine library:
- https://github.com/edubart/minicoro
- Supports assembly fallback from ucontext
- Good reference implementation

---

## Testing Strategy

```c
/* Test context switching */
void test_basic_switch(void) {
    static int phase = 0;
    static coro_context_t main_ctx, coro_ctx;
    static uint8_t stack[8192];

    void coro_func(void *arg) {
        assert(phase == 0);
        phase = 1;

        if (coro_save(&coro_ctx) == 0) {
            coro_restore(&main_ctx);
        }

        assert(phase == 2);
        phase = 3;
    }

    coro_make(&coro_ctx, stack, sizeof(stack), coro_func, NULL);

    if (coro_save(&main_ctx) == 0) {
        coro_restore(&coro_ctx);
    }

    assert(phase == 1);
    phase = 2;

    if (coro_save(&main_ctx) == 0) {
        coro_restore(&coro_ctx);
    }

    assert(phase == 3);
}

/* Test stack isolation */
void test_stack_isolation(void) {
    /* Ensure stacks don't interfere */
}

/* Test many switches */
void test_many_switches(void) {
    /* Switch 10000 times, ensure no corruption */
}
```

---

## Conclusion

**Short term:** Use ucontext with deprecation warnings suppressed - it works fine.

**Long term:** Implement assembly-based context switching (~200 lines total).

**Best of both worlds:**
1. Prototype with ucontext
2. Test the API and memory usage
3. Replace with assembly before production
4. Keep both implementations for comparison

The assembly is not scary - it's simpler than dealing with ucontext portability issues.
