/*
 * yafl.c - Safe Fiber API Implementation
 *
 * Implements high-level fiber operations using low-level context switching.
 * Stack allocation with optional guard pages and watermark support.
 *
 * Derived from Boost.Context (https://github.com/boostorg/context)
 * Copyright Kyle Hayes (2026)
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE or copy at
 *  http://www.boost.org/LICENSE_1_0.txt)
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <sys/mman.h>
    #include <unistd.h>
#endif

#include "yafl.h"


/* Raw context handle - opaque pointer to saved machine state */
typedef struct yafl_opaque_t *yafl_t;

/* Raw entry function type for low-level API */
typedef void (*yafl_entry_t)(void *);

/* Low-level assembly-implemented functions */

/**
 * @brief Create the initial saved context for a fiber stack.
 *
 * @param sp Top of the stack region.
 * @param size Size of the usable stack region in bytes.
 * @param fn Entry function that will run on the new context.
 * @return Saved low-level context handle, or NULL on failure.
 */
extern yafl_t yafl_make_context(void *sp, size_t size, yafl_entry_t fn);

/**
 * @brief Switch execution from one context to another.
 *
 * @param save Receives the current context before switching away.
 * @param target Context to resume.
 * @param data User data passed across the switch.
 * @return User data returned when control switches back.
 */
extern void *yafl_switch(yafl_t *save, yafl_t target, void *data);

/* Constants and types */
#define YAFL_FIBER_MAGIC 0xF1BE7001
#define YAFL_STACK_WATERMARK 0xA5
#define YAFL_STACK_ALIGNMENT 16

typedef enum { YAFL_ALLOC_MALLOC, YAFL_ALLOC_VMEM } yafl_alloc_type_t;

/* Internal fiber structure */
struct yafl_fiber {
    uint32_t magic;
    yafl_alloc_type_t alloc_type;
    yafl_fiber_status_t status;

    /* Stack management */
    void *stack_region;
    size_t stack_total_size;
    void *stack_top;
    size_t stack_size;
    bool watermark_filled;

    /* Context tracking */
    yafl_t context;         /* Fiber's saved context */
    yafl_t resumer_context; /* Context to return to */

    /* User entry and result */
    yafl_fiber_fn user_entry;
    void *cached_result;
};

/* Typedef for internal use */
typedef struct yafl_fiber yafl_fiber_t;

/* Thread-local storage */
static _Thread_local yafl_fiber_t *tls_current_fiber = NULL;

static void fiber_entry_trampoline(void *arg);

/**
 * @brief Release any stack memory owned by a fiber.
 *
 * @param fiber Fiber whose stack allocation should be freed.
 */
static void free_fiber_stack(yafl_fiber_t *fiber) {
    if(fiber == NULL || fiber->stack_region == NULL) { return; }

    if(fiber->alloc_type == YAFL_ALLOC_MALLOC) {
        free(fiber->stack_region);
    } else if(fiber->alloc_type == YAFL_ALLOC_VMEM) {
#ifdef _WIN32
        VirtualFree(fiber->stack_region, 0, MEM_RELEASE);
#else
        munmap(fiber->stack_region, fiber->stack_total_size);
#endif
    }

    fiber->stack_region = NULL;
    fiber->stack_total_size = 0;
    fiber->stack_top = NULL;
    fiber->stack_size = 0;
}

/**
 * @brief Create the initial low-level context for a fiber.
 *
 * @param fiber Fiber whose stack should be prepared for first entry.
 * @return true if the context was created successfully, otherwise false.
 */
static bool initialize_fiber_context(yafl_fiber_t *fiber) {
    fiber->context = yafl_make_context(fiber->stack_top, fiber->stack_size, fiber_entry_trampoline);
    return fiber->context != NULL;
}


/**
 * @brief Fill a fiber stack with the watermark byte and rebuild its context.
 *
 * @param fiber Fiber whose stack should be watermarked.
 * @return true if the context was rebuilt successfully, otherwise false.
 */
static bool reinitialize_fiber_context_with_watermark(yafl_fiber_t *fiber) {
    memset((char *)fiber->stack_top - fiber->stack_size, YAFL_STACK_WATERMARK, fiber->stack_size);
    return initialize_fiber_context(fiber);
}


/**
 * @brief Align a stack pointer down to the required stack alignment.
 *
 * @param ptr Unaligned stack pointer candidate.
 * @return Aligned stack pointer.
 */
static inline void *align_stack_pointer(void *ptr) {
    uintptr_t addr = (uintptr_t)ptr;
    return (void *)(addr & ~((uintptr_t)YAFL_STACK_ALIGNMENT - 1));
}


/**
 * @brief Enter a fiber through the low-level context trampoline.
 *
 * Calls the user entry function, updates fiber state, caches the final
 * result, and switches back to the resumer.
 *
 * @param arg Argument supplied by the first resume into the fiber.
 */
static void fiber_entry_trampoline(void *arg) {
    yafl_fiber_t *fiber = tls_current_fiber;
    if(fiber == NULL) { abort(); }

    /* Update status and TLS */
    fiber->status = YAFL_FIBER_STATUS_RUNNING;
    tls_current_fiber = fiber;

    /* Call user entry with argument from first resume */
    void *result = fiber->user_entry(arg);

    /* Mark complete and cache result */
    fiber->status = YAFL_FIBER_STATUS_COMPLETE;
    fiber->cached_result = result;
    tls_current_fiber = NULL;

    /* Return to resumer with final result */
    yafl_switch(&fiber->context, fiber->resumer_context, result);

    /* Should never reach here */
    abort();
}


/**
 * @brief Create a fiber and allocate its backing stack.
 *
 * @param fiber_fn User entry function for the new fiber.
 * @param stack_size Requested stack size in bytes, or 0 for the default.
 * @param flags Stack allocation and feature flags.
 * @return Newly created fiber, or NULL on failure.
 */
extern yafl_fiber_t *yafl_fiber_create(yafl_fiber_fn fiber_fn, size_t stack_size, yafl_stack_flags_t flags) {
    /* Validate fiber function is not NULL */
    if(fiber_fn == NULL) { return NULL; }

    /* Validate exactly one allocation type is set */
    int alloc_flags = flags & (YAFL_STACK_FLAGS_MALLOC | YAFL_STACK_FLAGS_VMEM);
    if(alloc_flags != YAFL_STACK_FLAGS_MALLOC && alloc_flags != YAFL_STACK_FLAGS_VMEM) { return NULL; }

    bool use_vmem = (flags & YAFL_STACK_FLAGS_VMEM) != 0;
    bool use_watermark = (flags & YAFL_STACK_FLAGS_WATERMARK) != 0;

    /* Allocate fiber structure */
    yafl_fiber_t *fiber = malloc(sizeof(yafl_fiber_t));
    if(fiber == NULL) { return NULL; }

    /* Initialize fiber structure */
    fiber->magic = YAFL_FIBER_MAGIC;
    fiber->alloc_type = use_vmem ? YAFL_ALLOC_VMEM : YAFL_ALLOC_MALLOC;
    fiber->status = YAFL_FIBER_STATUS_SUSPENDED;
    fiber->user_entry = fiber_fn;
    fiber->cached_result = NULL;
    fiber->watermark_filled = use_watermark;
    fiber->context = NULL;
    fiber->resumer_context = NULL;
    fiber->stack_region = NULL;
    fiber->stack_total_size = 0;
    fiber->stack_top = NULL;
    fiber->stack_size = 0;

    /* Use default stack size if not specified */
    if(stack_size == 0) { stack_size = YAFL_DEFAULT_STACK_SIZE; }

    /* Allocate stack based on allocation type */
    if(use_vmem) {
        size_t page_size = yafl_get_page_size();
        size_t stack_with_overhead = stack_size + 256;
        size_t aligned_stack_size = ((stack_with_overhead + page_size - 1) / page_size) * page_size;
        size_t guard_size = page_size;
        size_t total_size = guard_size + aligned_stack_size + guard_size;

        void *region = NULL;
#ifdef _WIN32
        region = VirtualAlloc(NULL, total_size, MEM_RESERVE, PAGE_NOACCESS);
        if(region == NULL) { goto create_fail; }
        fiber->stack_region = region;
        fiber->stack_total_size = total_size;
        void *stack_base = (char *)region + guard_size;
        if(!VirtualAlloc(stack_base, aligned_stack_size, MEM_COMMIT, PAGE_READWRITE)) { goto create_fail; }
#else
        region = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if(region == MAP_FAILED) { goto create_fail; }
        fiber->stack_region = region;
        fiber->stack_total_size = total_size;
        if(mprotect(region, guard_size, PROT_NONE) == -1) { goto create_fail; }
        if(mprotect((char *)region + guard_size + aligned_stack_size, guard_size, PROT_NONE) == -1) { goto create_fail; }
        void *stack_base = (char *)region + guard_size;
#endif

        void *stack_region_end = (char *)stack_base + aligned_stack_size;
        void *stack_top = (char *)stack_region_end - 256;
        stack_top = align_stack_pointer(stack_top);
        size_t actual_stack_size = (uintptr_t)(intptr_t)((char *)stack_top - (char *)stack_base);

        fiber->stack_top = stack_top;
        fiber->stack_size = actual_stack_size;
    } else {
        /* malloc allocation */
        size_t allocated_size = stack_size + 256;
        void *block = malloc(allocated_size);
        if(block == NULL) { goto create_fail; }
        fiber->stack_region = block;
        fiber->stack_total_size = allocated_size;

        void *block_end = (char *)block + allocated_size;
        void *stack_top = (char *)block_end - 256;
        stack_top = align_stack_pointer(stack_top);

        size_t actual_stack_size = (uintptr_t)(intptr_t)((char *)stack_top - (char *)block);

        fiber->stack_top = stack_top;
        fiber->stack_size = actual_stack_size;
    }

    /* Initialize the low-level context with trampoline as entry */
    if(!initialize_fiber_context(fiber)) { goto create_fail; }

    /* Apply watermark if requested */
    if(use_watermark) {
        if(!reinitialize_fiber_context_with_watermark(fiber)) { goto create_fail; }
    }

    return fiber;

create_fail:
    free_fiber_stack(fiber);
    free(fiber);
    return NULL;
}

/**
 * @brief Resume a suspended fiber.
 *
 * @param fiber Fiber to resume.
 * @param arg Argument delivered to the fiber.
 * @return Value yielded or returned by the fiber, or NULL on error.
 */
extern void *yafl_fiber_resume(yafl_fiber_t *fiber, void *arg) {
    /* Validation */
    if(fiber == NULL || fiber->magic != YAFL_FIBER_MAGIC) { return NULL; }

    /* If complete, return cached result (idempotent) */
    if(fiber->status == YAFL_FIBER_STATUS_COMPLETE) { return fiber->cached_result; }

    /* Cannot resume running fiber */
    if(fiber->status == YAFL_FIBER_STATUS_RUNNING) { return NULL; }

    /* Update status and TLS */
    fiber->status = YAFL_FIBER_STATUS_RUNNING;
    tls_current_fiber = fiber;

    /* Perform context switch */
    void *result = yafl_switch(&fiber->resumer_context, fiber->context, arg);

    /* Back in resumer */
    tls_current_fiber = NULL;

    return result;
}

/**
 * @brief Suspend the current fiber and return control to its resumer.
 *
 * @param result Value yielded back to the resumer.
 * @return Argument supplied by the next resume call, or NULL on error.
 */
extern void *yafl_fiber_suspend(void *result) {
    yafl_fiber_t *current = tls_current_fiber;

    /* Must be in a fiber */
    if(current == NULL) { return NULL; }

    /* Update status */
    current->status = YAFL_FIBER_STATUS_SUSPENDED;

    /* Switch back to resumer */
    void *arg = yafl_switch(&current->context, current->resumer_context, result);

    /* When resumed - restore state */
    current->status = YAFL_FIBER_STATUS_RUNNING;
    tls_current_fiber = current;

    /* Return argument passed to resume */
    return arg;
}

/**
 * @brief Query the status of a fiber.
 *
 * @param fiber Fiber to inspect.
 * @return Current fiber status, or YAFL_FIBER_STATUS_ERR on invalid input.
 */
extern yafl_fiber_status_t yafl_fiber_status(yafl_fiber_t *fiber) {
    if(fiber == NULL || fiber->magic != YAFL_FIBER_MAGIC) { return YAFL_FIBER_STATUS_ERR; }
    return fiber->status;
}

/**
 * @brief Measure the maximum observed stack usage for a watermarked fiber.
 *
 * @param fiber Fiber to inspect.
 * @return Number of bytes used, or 0 if watermarking is unavailable.
 */
extern size_t yafl_fiber_stack_high_watermark(yafl_fiber_t *fiber) {
    if(fiber == NULL || fiber->magic != YAFL_FIBER_MAGIC || !fiber->watermark_filled) { return 0; }

    /* Scan from stack base for watermark bytes */
    unsigned char *stack_base = (unsigned char *)fiber->stack_top - fiber->stack_size;
    size_t unused = 0;

    while(unused < fiber->stack_size && stack_base[unused] == YAFL_STACK_WATERMARK) { unused++; }

    return fiber->stack_size - unused;
}

/**
 * @brief Destroy a fiber and release any owned resources.
 *
 * @param fiber Fiber to destroy.
 */
extern void yafl_fiber_destroy(yafl_fiber_t *fiber) {
    if(fiber == NULL || fiber->magic != YAFL_FIBER_MAGIC) { return; }

    /* Cannot destroy running fiber */
    if(fiber->status == YAFL_FIBER_STATUS_RUNNING) { return; }

    free_fiber_stack(fiber);

    /* Invalidate and free */
    fiber->magic = 0;
    free(fiber);
}

/**
 * @brief Return the host page size.
 *
 * @return System page size in bytes.
 */
extern size_t yafl_get_page_size(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (size_t)si.dwPageSize;
#else
    long page_size = sysconf(_SC_PAGE_SIZE);
    if(page_size <= 0) { return 4096; }
    return (size_t)page_size;
#endif
}
