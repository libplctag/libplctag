/*
 * test_yafl_basic.c - Basic API tests for simplified fiber API
 *
 * Tests all combinations of creation flags and basic lifecycle operations.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/yafl.h"

/* ========================================================================
 * Test: Simple Fiber Execution
 * ======================================================================== */

static void *simple_fiber_func(void *arg) {
    return arg;
}

static void test_simple_execution(void) {
    printf("test_simple_execution: ");
    fflush(stdout);

    /* Create fiber with malloc allocation */
    yafl_fiber_t *fiber = yafl_fiber_create(simple_fiber_func, 16 * 1024,
                                            YAFL_STACK_FLAGS_MALLOC);
    assert(fiber != NULL);
    assert(yafl_fiber_status(fiber) == YAFL_FIBER_STATUS_SUSPENDED);

    /* Resume fiber */
    void *result = yafl_fiber_resume(fiber, (void *)0x1234);
    assert(result == (void *)0x1234);
    assert(yafl_fiber_status(fiber) == YAFL_FIBER_STATUS_COMPLETE);

    /* Resuming completed fiber returns cached result */
    void *result2 = yafl_fiber_resume(fiber, (void *)0x5678);
    assert(result2 == (void *)0x1234);

    /* Cleanup */
    yafl_fiber_destroy(fiber);
    yafl_fiber_destroy(NULL);  /* Safe to call on NULL */

    printf("PASS\n");
}

/* ========================================================================
 * Test: Flag Combinations
 * ======================================================================== */

static void test_flag_combinations(void) {
    printf("test_flag_combinations: ");
    fflush(stdout);

    /* Valid: MALLOC alone */
    yafl_fiber_t *f1 = yafl_fiber_create(simple_fiber_func, 16 * 1024,
                                         YAFL_STACK_FLAGS_MALLOC);
    assert(f1 != NULL);
    yafl_fiber_destroy(f1);

    /* Valid: VMEM alone */
    yafl_fiber_t *f2 = yafl_fiber_create(simple_fiber_func, 16 * 1024,
                                         YAFL_STACK_FLAGS_VMEM);
    assert(f2 != NULL);
    yafl_fiber_destroy(f2);

    /* Valid: MALLOC with WATERMARK */
    yafl_fiber_t *f3 = yafl_fiber_create(simple_fiber_func, 16 * 1024,
                                         YAFL_STACK_FLAGS_MALLOC | YAFL_STACK_FLAGS_WATERMARK);
    assert(f3 != NULL);
    yafl_fiber_destroy(f3);

    /* Valid: VMEM with WATERMARK */
    yafl_fiber_t *f4 = yafl_fiber_create(simple_fiber_func, 16 * 1024,
                                         YAFL_STACK_FLAGS_VMEM | YAFL_STACK_FLAGS_WATERMARK);
    assert(f4 != NULL);
    yafl_fiber_destroy(f4);

    /* Invalid: NONE */
    yafl_fiber_t *f5 = yafl_fiber_create(simple_fiber_func, 16 * 1024,
                                         YAFL_STACK_FLAGS_NONE);
    assert(f5 == NULL);

    /* Invalid: MALLOC | VMEM both set */
    yafl_fiber_t *f6 = yafl_fiber_create(simple_fiber_func, 16 * 1024,
                                         YAFL_STACK_FLAGS_MALLOC | YAFL_STACK_FLAGS_VMEM);
    assert(f6 == NULL);

    /* Invalid: Neither allocation type */
    yafl_fiber_t *f7 = yafl_fiber_create(simple_fiber_func, 16 * 1024,
                                         YAFL_STACK_FLAGS_WATERMARK);
    assert(f7 == NULL);

    printf("PASS\n");
}

/* ========================================================================
 * Test: Suspend/Resume Cycles
 * ======================================================================== */

static void *cycling_fiber_func(void *arg) {
    void *result = arg;

    for (int i = 0; i < 5; i++) {
        result = yafl_fiber_suspend(result);
    }

    return result;
}

static void test_suspend_resume_cycles(void) {
    printf("test_suspend_resume_cycles: ");
    fflush(stdout);

    yafl_fiber_t *fiber = yafl_fiber_create(cycling_fiber_func, 16 * 1024,
                                            YAFL_STACK_FLAGS_MALLOC);
    assert(fiber != NULL);

    /* Cycle 1 */
    void *result = yafl_fiber_resume(fiber, (void *)0x1);
    assert(result == (void *)0x1);
    assert(yafl_fiber_status(fiber) == YAFL_FIBER_STATUS_SUSPENDED);

    /* Cycle 2 */
    result = yafl_fiber_resume(fiber, (void *)0x2);
    assert(result == (void *)0x2);
    assert(yafl_fiber_status(fiber) == YAFL_FIBER_STATUS_SUSPENDED);

    /* Cycle 3 */
    result = yafl_fiber_resume(fiber, (void *)0x3);
    assert(result == (void *)0x3);

    /* Cycle 4 */
    result = yafl_fiber_resume(fiber, (void *)0x4);
    assert(result == (void *)0x4);

    /* Cycle 5 */
    result = yafl_fiber_resume(fiber, (void *)0x5);
    assert(result == (void *)0x5);

    /* Final cycle - fiber completes */
    result = yafl_fiber_resume(fiber, (void *)0x6);
    assert(result == (void *)0x6);
    assert(yafl_fiber_status(fiber) == YAFL_FIBER_STATUS_COMPLETE);

    yafl_fiber_destroy(fiber);

    printf("PASS\n");
}

/* ========================================================================
 * Test: Status Query
 * ======================================================================== */

static void test_status_query(void) {
    printf("test_status_query: ");
    fflush(stdout);

    yafl_fiber_t *fiber = yafl_fiber_create(simple_fiber_func, 16 * 1024,
                                            YAFL_STACK_FLAGS_MALLOC);

    /* Check initial status */
    assert(yafl_fiber_status(fiber) == YAFL_FIBER_STATUS_SUSPENDED);

    /* Check status of NULL */
    assert(yafl_fiber_status(NULL) == YAFL_FIBER_STATUS_ERR);

    /* Resume and check completion */
    yafl_fiber_resume(fiber, (void *)0x99);
    assert(yafl_fiber_status(fiber) == YAFL_FIBER_STATUS_COMPLETE);

    yafl_fiber_destroy(fiber);

    printf("PASS\n");
}

/* ========================================================================
 * Test: Watermark Measurement
 * ======================================================================== */

static void *watermarked_fiber_func(void *arg) {
    /* Allocate some stack space to trigger watermark usage */
    volatile char stack_buffer[1024];
    /* Use memset to ensure compiler can't optimize the buffer away */
    memset((char *)stack_buffer, 0xAA, sizeof(stack_buffer));
    return arg;
}

static void test_watermark(void) {
    printf("test_watermark_malloc: ");
    fflush(stdout);

    /* Test with malloc + watermark */
    yafl_fiber_t *fiber = yafl_fiber_create(watermarked_fiber_func, 16 * 1024,
                                            YAFL_STACK_FLAGS_MALLOC | YAFL_STACK_FLAGS_WATERMARK);
    assert(fiber != NULL);

    /* Resume and check watermark */
    yafl_fiber_resume(fiber, NULL);
    assert(yafl_fiber_status(fiber) == YAFL_FIBER_STATUS_COMPLETE);

    size_t usage = yafl_fiber_stack_high_watermark(fiber);
    assert(usage > 0);  /* Should have used some stack */

    yafl_fiber_destroy(fiber);

    printf("PASS\n");

    printf("test_watermark_vmem: ");
    fflush(stdout);

    /* Test with vmem + watermark */
    yafl_fiber_t *fiber2 = yafl_fiber_create(watermarked_fiber_func, 16 * 1024,
                                             YAFL_STACK_FLAGS_VMEM | YAFL_STACK_FLAGS_WATERMARK);
    assert(fiber2 != NULL);

    yafl_fiber_resume(fiber2, NULL);
    assert(yafl_fiber_status(fiber2) == YAFL_FIBER_STATUS_COMPLETE);

    size_t usage2 = yafl_fiber_stack_high_watermark(fiber2);
    assert(usage2 > 0);

    yafl_fiber_destroy(fiber2);

    printf("PASS\n");
}

/* ========================================================================
 * Test: Watermark Without Flag
 * ======================================================================== */

static void test_watermark_without_flag(void) {
    printf("test_watermark_without_flag: ");
    fflush(stdout);

    yafl_fiber_t *fiber = yafl_fiber_create(simple_fiber_func, 16 * 1024,
                                            YAFL_STACK_FLAGS_MALLOC);
    assert(fiber != NULL);

    /* Without watermark flag, should return 0 */
    assert(yafl_fiber_stack_high_watermark(fiber) == 0);

    yafl_fiber_resume(fiber, NULL);

    /* Still 0 because no watermark was applied */
    assert(yafl_fiber_stack_high_watermark(fiber) == 0);

    yafl_fiber_destroy(fiber);

    printf("PASS\n");
}

/* ========================================================================
 * Test: NULL Fiber Pointer
 * ======================================================================== */

static void test_null_fiber_pointer(void) {
    printf("test_null_fiber_pointer: ");
    fflush(stdout);

    /* Resume NULL fiber */
    void *result = yafl_fiber_resume(NULL, (void *)0x1);
    assert(result == NULL);

    /* Destroy NULL fiber (safe no-op) */
    yafl_fiber_destroy(NULL);

    printf("PASS\n");
}

/* ========================================================================
 * Test: NULL Entry Function
 * ======================================================================== */

static void test_null_entry_function(void) {
    printf("test_null_entry_function: ");
    fflush(stdout);

    yafl_fiber_t *fiber = yafl_fiber_create(NULL, 16 * 1024,
                                            YAFL_STACK_FLAGS_MALLOC);
    assert(fiber == NULL);

    printf("PASS\n");
}

/* ========================================================================
 * Test: Default Stack Size
 * ======================================================================== */

static void test_default_stack_size(void) {
    printf("test_default_stack_size: ");
    fflush(stdout);

    /* Create with 0 stack size (uses default) */
    yafl_fiber_t *fiber = yafl_fiber_create(simple_fiber_func, 0,
                                            YAFL_STACK_FLAGS_MALLOC);
    assert(fiber != NULL);

    yafl_fiber_resume(fiber, (void *)0x42);
    yafl_fiber_destroy(fiber);

    printf("PASS\n");
}

/* ========================================================================
 * Main
 * ======================================================================== */

int main(void) {
    printf("Running YAFL basic tests...\n");

    test_simple_execution();
    test_flag_combinations();
    test_suspend_resume_cycles();
    test_status_query();
    test_watermark();
    test_watermark_without_flag();
    test_null_fiber_pointer();
    test_null_entry_function();
    test_default_stack_size();

    printf("\nAll tests passed!\n");
    return 0;
}
