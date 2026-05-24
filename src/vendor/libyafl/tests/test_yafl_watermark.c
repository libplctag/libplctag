/*
 * test_yafl_watermark.c - Dedicated stack watermark tests
 *
 * Verifies watermark tracking for both allocation modes and confirms that
 * watermark queries work while a fiber is suspended and after it completes.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../include/yafl.h"

#define TEST_STACK_TOUCH_COMPLETE 0x34
#define TEST_STACK_TOUCH_SUSPEND_BEFORE 0xC9
#define TEST_STACK_TOUCH_SUSPEND_AFTER 0x9C

static const char *alloc_name(yafl_stack_flags_t alloc_flag) { return alloc_flag == YAFL_STACK_FLAGS_VMEM ? "mmap" : "malloc"; }

/* Force observable stack writes so YAFL's own watermark logic has real usage to measure. */
static void touch_volatile_stack_buffer(volatile unsigned char *buffer, size_t size, unsigned char value) {
    for(size_t index = 0; index < size; index++) { buffer[index] = value; }
}

static void *complete_after_stack_use(void *arg) {
    volatile unsigned char stack_buffer[1536];

    touch_volatile_stack_buffer(stack_buffer, sizeof(stack_buffer), TEST_STACK_TOUCH_COMPLETE);
    return arg;
}

static void *suspend_after_stack_use(void *arg) {
    volatile unsigned char stack_buffer[768];

    touch_volatile_stack_buffer(stack_buffer, sizeof(stack_buffer), TEST_STACK_TOUCH_SUSPEND_BEFORE);
    arg = yafl_fiber_suspend(arg);

    touch_volatile_stack_buffer(stack_buffer, sizeof(stack_buffer), TEST_STACK_TOUCH_SUSPEND_AFTER);
    return arg;
}

static void test_watermark_complete(yafl_stack_flags_t alloc_flag, uintptr_t value) {
    printf("test_watermark_complete_%s: ", alloc_name(alloc_flag));
    fflush(stdout);

    yafl_fiber_t *fiber = yafl_fiber_create(complete_after_stack_use, 16 * 1024, alloc_flag | YAFL_STACK_FLAGS_WATERMARK);
    assert(fiber != NULL);

    void *result = yafl_fiber_resume(fiber, (void *)value);
    assert(result == (void *)value);
    assert(yafl_fiber_status(fiber) == YAFL_FIBER_STATUS_COMPLETE);

    size_t usage = yafl_fiber_stack_high_watermark(fiber);
    assert(usage > 0);
    assert(usage < 16 * 1024);

    yafl_fiber_destroy(fiber);

    printf("PASS\n");
}

static void test_watermark_while_suspended(yafl_stack_flags_t alloc_flag, uintptr_t first_value, uintptr_t second_value) {
    printf("test_watermark_while_suspended_%s: ", alloc_name(alloc_flag));
    fflush(stdout);

    yafl_fiber_t *fiber = yafl_fiber_create(suspend_after_stack_use, 16 * 1024, alloc_flag | YAFL_STACK_FLAGS_WATERMARK);
    assert(fiber != NULL);

    void *result = yafl_fiber_resume(fiber, (void *)first_value);
    assert(result == (void *)first_value);
    assert(yafl_fiber_status(fiber) == YAFL_FIBER_STATUS_SUSPENDED);

    size_t suspended_usage = yafl_fiber_stack_high_watermark(fiber);
    assert(suspended_usage > 0);
    assert(suspended_usage < 16 * 1024);

    result = yafl_fiber_resume(fiber, (void *)second_value);
    assert(result == (void *)second_value);
    assert(yafl_fiber_status(fiber) == YAFL_FIBER_STATUS_COMPLETE);

    size_t complete_usage = yafl_fiber_stack_high_watermark(fiber);
    assert(complete_usage >= suspended_usage);
    assert(complete_usage < 16 * 1024);

    yafl_fiber_destroy(fiber);

    printf("PASS\n");
}

static void test_watermark_without_flag(yafl_stack_flags_t alloc_flag, uintptr_t value) {
    printf("test_watermark_without_flag_%s: ", alloc_name(alloc_flag));
    fflush(stdout);

    yafl_fiber_t *fiber = yafl_fiber_create(complete_after_stack_use, 16 * 1024, alloc_flag);
    assert(fiber != NULL);
    assert(yafl_fiber_stack_high_watermark(fiber) == 0);

    void *result = yafl_fiber_resume(fiber, (void *)value);
    assert(result == (void *)value);
    assert(yafl_fiber_stack_high_watermark(fiber) == 0);

    yafl_fiber_destroy(fiber);

    printf("PASS\n");
}

static void test_watermark_null_fiber(void) {
    printf("test_watermark_null_fiber: ");
    fflush(stdout);

    assert(yafl_fiber_stack_high_watermark(NULL) == 0);

    printf("PASS\n");
}

int main(void) {
    printf("Running YAFL watermark tests...\n");

    test_watermark_complete(YAFL_STACK_FLAGS_MALLOC, 0x11);
    test_watermark_complete(YAFL_STACK_FLAGS_VMEM, 0x22);
    test_watermark_while_suspended(YAFL_STACK_FLAGS_MALLOC, 0x33, 0x44);
    test_watermark_while_suspended(YAFL_STACK_FLAGS_VMEM, 0x55, 0x66);
    test_watermark_without_flag(YAFL_STACK_FLAGS_MALLOC, 0x77);
    test_watermark_without_flag(YAFL_STACK_FLAGS_VMEM, 0x88);
    test_watermark_null_fiber();

    printf("\nAll watermark tests passed!\n");
    return 0;
}
