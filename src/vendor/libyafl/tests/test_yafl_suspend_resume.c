/*
 * test_yafl_suspend_resume.c - Suspend/resume cycle tests
 *
 * Tests multiple suspend/resume patterns and data passing.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../include/yafl.h"

/* ========================================================================
 * Test: Bidirectional Data Passing
 * ======================================================================== */

static void *echo_fiber_func(void *arg) {
    /* Fiber receives initial arg, suspends with it */
    arg = yafl_fiber_suspend(arg);

    /* Receives new arg, suspends with it */
    arg = yafl_fiber_suspend(arg);

    /* Receives final arg, returns it */
    return arg;
}

static void test_bidirectional_data_passing(void) {
    printf("test_bidirectional_data_passing: ");
    fflush(stdout);

    yafl_fiber_t *fiber = yafl_fiber_create(echo_fiber_func, 16 * 1024,
                                            YAFL_STACK_FLAGS_MALLOC);
    assert(fiber != NULL);

    /* First resume: send 0x1111, receive 0x1111 back */
    void *result = yafl_fiber_resume(fiber, (void *)0x1111);
    assert(result == (void *)0x1111);
    assert(yafl_fiber_status(fiber) == YAFL_FIBER_STATUS_SUSPENDED);

    /* Second resume: send 0x2222, receive 0x2222 back */
    result = yafl_fiber_resume(fiber, (void *)0x2222);
    assert(result == (void *)0x2222);
    assert(yafl_fiber_status(fiber) == YAFL_FIBER_STATUS_SUSPENDED);

    /* Third resume: send 0x3333, receive 0x3333 back (fiber completes) */
    result = yafl_fiber_resume(fiber, (void *)0x3333);
    assert(result == (void *)0x3333);
    assert(yafl_fiber_status(fiber) == YAFL_FIBER_STATUS_COMPLETE);

    yafl_fiber_destroy(fiber);

    printf("PASS\n");
}

/* ========================================================================
 * Test: Multiple Suspend Points
 * ======================================================================== */

static void *multi_suspend_fiber_func(void *arg) {
    for (int i = 0; i < 10; i++) {
        arg = yafl_fiber_suspend((void *)(uintptr_t)(0x1000 + i));
    }
    return arg;
}

static void test_multiple_suspend_points(void) {
    printf("test_multiple_suspend_points: ");
    fflush(stdout);

    yafl_fiber_t *fiber = yafl_fiber_create(multi_suspend_fiber_func, 16 * 1024,
                                            YAFL_STACK_FLAGS_MALLOC);
    assert(fiber != NULL);

    for (int i = 0; i < 10; i++) {
        void *result = yafl_fiber_resume(fiber, (void *)(uintptr_t)(0x2000 + i));
        assert(result == (void *)(uintptr_t)(0x1000 + i));
        assert(yafl_fiber_status(fiber) == YAFL_FIBER_STATUS_SUSPENDED);
    }

    /* Final resume to complete - should return the last arg passed to resume */
    void *final = yafl_fiber_resume(fiber, (void *)0x3000);
    assert(final == (void *)0x3000);
    assert(yafl_fiber_status(fiber) == YAFL_FIBER_STATUS_COMPLETE);

    yafl_fiber_destroy(fiber);

    printf("PASS\n");
}

/* ========================================================================
 * Test: Conditional Suspension
 * ======================================================================== */

static void *conditional_suspend_fiber_func(void *arg) {
    for (int i = 0; i < 5; i++) {
        if (i % 2 == 0) {
            arg = yafl_fiber_suspend((void *)(uintptr_t)(0x100 + i));
        }
    }
    return arg;
}

static void test_conditional_suspension(void) {
    printf("test_conditional_suspension: ");
    fflush(stdout);

    yafl_fiber_t *fiber = yafl_fiber_create(conditional_suspend_fiber_func, 16 * 1024,
                                            YAFL_STACK_FLAGS_MALLOC);
    assert(fiber != NULL);

    /* Suspend at i=0 */
    void *result = yafl_fiber_resume(fiber, (void *)0x1);
    assert(result == (void *)0x100);

    /* Suspend at i=2 */
    result = yafl_fiber_resume(fiber, (void *)0x2);
    assert(result == (void *)0x102);

    /* Suspend at i=4 */
    result = yafl_fiber_resume(fiber, (void *)0x3);
    assert(result == (void *)0x104);

    /* Complete */
    result = yafl_fiber_resume(fiber, (void *)0x4);
    assert(result == (void *)0x4);
    assert(yafl_fiber_status(fiber) == YAFL_FIBER_STATUS_COMPLETE);

    yafl_fiber_destroy(fiber);

    printf("PASS\n");
}

/* ========================================================================
 * Test: NULL Data Passing
 * ======================================================================== */

static void *null_data_fiber_func(void *arg) {
    assert(arg == NULL);  /* First resume passes NULL */
    arg = yafl_fiber_suspend(NULL);
    assert(arg == NULL);  /* Second resume passes NULL */
    return arg;
}

static void test_null_data_passing(void) {
    printf("test_null_data_passing: ");
    fflush(stdout);

    yafl_fiber_t *fiber = yafl_fiber_create(null_data_fiber_func, 16 * 1024,
                                            YAFL_STACK_FLAGS_MALLOC);
    assert(fiber != NULL);

    void *result = yafl_fiber_resume(fiber, NULL);
    assert(result == NULL);

    result = yafl_fiber_resume(fiber, NULL);
    assert(result == NULL);

    yafl_fiber_destroy(fiber);

    printf("PASS\n");
}

/* ========================================================================
 * Test: VMEM Allocation with Suspend/Resume
 * ======================================================================== */

static void *vmem_test_func(void *arg) {
    arg = yafl_fiber_suspend((void *)0xAAAA);
    arg = yafl_fiber_suspend((void *)0xBBBB);
    return arg;
}

static void test_vmem_suspend_resume(void) {
    printf("test_vmem_suspend_resume: ");
    fflush(stdout);

    yafl_fiber_t *fiber = yafl_fiber_create(vmem_test_func, 16 * 1024,
                                            YAFL_STACK_FLAGS_VMEM);
    assert(fiber != NULL);

    void *result = yafl_fiber_resume(fiber, (void *)0x1111);
    assert(result == (void *)0xAAAA);

    result = yafl_fiber_resume(fiber, (void *)0x2222);
    assert(result == (void *)0xBBBB);

    result = yafl_fiber_resume(fiber, (void *)0x3333);
    assert(result == (void *)0x3333);
    assert(yafl_fiber_status(fiber) == YAFL_FIBER_STATUS_COMPLETE);

    yafl_fiber_destroy(fiber);

    printf("PASS\n");
}

/* ========================================================================
 * Main
 * ======================================================================== */

int main(void) {
    printf("Running YAFL suspend/resume tests...\n");

    test_bidirectional_data_passing();
    test_multiple_suspend_points();
    test_conditional_suspension();
    test_null_data_passing();
    test_vmem_suspend_resume();

    printf("\nAll tests passed!\n");
    return 0;
}
