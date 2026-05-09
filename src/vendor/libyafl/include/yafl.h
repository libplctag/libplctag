/*
 * yafl.h - Safe Fiber API
 *
 * High-level fiber/coroutine support built on portable assembly primitives.
 * The low-level context switching API is internal and not exposed.
 *
 * Copyright Kyle Hayes (2026)
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE or copy at
 *  http://www.boost.org/LICENSE_1_0.txt)
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Configuration
 * ======================================================================== */

#ifndef YAFL_DEFAULT_STACK_SIZE
    #define YAFL_DEFAULT_STACK_SIZE (24 * 1024)
#endif

/* ========================================================================
 * Types
 * ======================================================================== */

/* Opaque fiber handle */
typedef struct yafl_fiber yafl_fiber_t;

/* Stack allocation and watermark flags */
typedef enum {
    YAFL_STACK_FLAGS_NONE = 0,
    YAFL_STACK_FLAGS_MALLOC = (1 << 0),    /* Use malloc for stack allocation */
    YAFL_STACK_FLAGS_VMEM = (1 << 1),      /* Use virtual memory (with guard pages) */
    YAFL_STACK_FLAGS_WATERMARK = (1 << 8), /* Fill stack with watermark pattern */
} yafl_stack_flags_t;

/* Fiber execution status */
typedef enum {
    YAFL_FIBER_STATUS_ERR,       /* Invalid fiber or error condition */
    YAFL_FIBER_STATUS_SUSPENDED, /* Fiber is suspended, waiting to resume */
    YAFL_FIBER_STATUS_RUNNING,   /* Fiber is currently executing */
    YAFL_FIBER_STATUS_COMPLETE,  /* Fiber has finished execution */
} yafl_fiber_status_t;

/* Fiber entry function - receives argument, returns final result */
typedef void *(*yafl_fiber_fn)(void *arg);

/* ========================================================================
 * Fiber Creation
 * ======================================================================== */

/*
 * Create a new fiber.
 *
 * The fiber is created in a SUSPENDED state. Stack allocation type and
 * watermark settings are controlled via flags.
 *
 * Parameters:
 *   fiber_fn   - Function to execute in the fiber
 *   stack_size - Requested stack size (0 = use default)
 *   flags      - Stack allocation and watermark flags
 *
 * Returns:
 *   New fiber in SUSPENDED state, or NULL on allocation failure or invalid flags.
 *
 * Flag validation:
 *   - Exactly one of YAFL_STACK_FLAGS_MALLOC or YAFL_STACK_FLAGS_VMEM must be set
 *   - YAFL_STACK_FLAGS_WATERMARK is optional
 *   - YAFL_STACK_FLAGS_NONE (no flags) is invalid
 */
extern yafl_fiber_t *yafl_fiber_create(yafl_fiber_fn fiber_fn, size_t stack_size, yafl_stack_flags_t flags);


/* ========================================================================
 * Fiber Control Flow
 * ======================================================================== */

/*
 * Start or resume a fiber.
 *
 * If the fiber is SUSPENDED, resumes execution from the last suspend point.
 * If the fiber is in its initial SUSPENDED state, starts execution.
 * If the fiber is COMPLETE, returns cached result without re-entering.
 *
 * Parameters:
 *   fiber - Fiber to resume (must be SUSPENDED or initially created)
 *   arg   - Argument passed to fiber (becomes return value from suspend)
 *
 * Returns:
 *   Data returned from suspend or fiber's entry function.
 *   Returns NULL if fiber is NULL, invalid, or RUNNING.
 *
 * Note: NULL is a valid return value. Use yafl_fiber_status() to distinguish
 *       NULL results from errors.
 */
extern void *yafl_fiber_resume(yafl_fiber_t *fiber, void *arg);

/*
 * Suspend the current fiber.
 *
 * Yields control back to the fiber that called resume().
 * Must be called from within a fiber, not from the main thread/non-fiber context.
 *
 * Parameters:
 *   result - Value to return from the current resume() call
 *
 * Returns:
 *   Argument passed to the next resume() call.
 *   Returns NULL if not currently in a fiber.
 */
extern void *yafl_fiber_suspend(void *result);

/* ========================================================================
 * Fiber Status and Monitoring
 * ======================================================================== */

/*
 * Get the current status of a fiber.
 *
 * Parameters:
 *   fiber - Fiber to query (may be NULL)
 *
 * Returns:
 *   Current status, or YAFL_FIBER_STATUS_ERR if fiber is NULL or invalid.
 */
extern yafl_fiber_status_t yafl_fiber_status(yafl_fiber_t *fiber);

/* ========================================================================
 * Stack Debugging (Watermark)
 * ======================================================================== */

/*
 * Get the high water mark of stack usage.
 *
 * Only valid if the fiber was created with YAFL_STACK_FLAGS_WATERMARK.
 * Can be called when fiber is SUSPENDED or COMPLETE.
 *
 * Parameters:
 *   fiber - Fiber to check
 *
 * Returns:
 *   Maximum bytes of stack used, or 0 if no watermark or error.
 */
extern size_t yafl_fiber_stack_high_watermark(yafl_fiber_t *fiber);

/* ========================================================================
 * Cleanup
 * ======================================================================== */

/*
 * Destroy a fiber and free its resources.
 *
 * Do not call on a RUNNING fiber.
 * Safe to call on NULL or invalid fiber.
 */
extern void yafl_fiber_destroy(yafl_fiber_t *fiber);

/* ========================================================================
 * Utilities
 * ======================================================================== */

/* Get system page size in bytes */
extern size_t yafl_get_page_size(void);

#ifdef __cplusplus
}
#endif
