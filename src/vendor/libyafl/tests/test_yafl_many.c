/*
 * test_yafl_many.c - Scalability test with many fibers
 *
 * Verifies that 100 fibers can coexist and complete using round-robin scheduling.
 * Tests status query functions and confirms each fiber gets exactly 10 iterations.
 *
 * Copyright Kyle Hayes (2026)
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE or copy at
 *  http://www.boost.org/LICENSE_1_0.txt)
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../include/yafl.h"

#define NUM_FIBERS 100
#define ITERATIONS 10

static int counters[NUM_FIBERS];
static yafl_fiber_t *fibers[NUM_FIBERS];

static void *fiber_entry(void *data) {
    int id = (int)(uintptr_t)data;

    fprintf(stderr, "[fiber %d] entered\n", id);
    fflush(stderr);

    for (int i = 0; i < ITERATIONS; i++) {
        fprintf(stderr, "[fiber %d] iteration %d/%d\n", id, i + 1, ITERATIONS);
        fflush(stderr);

        counters[id]++;

        fprintf(stderr, "[fiber %d] suspending\n", id);
        fflush(stderr);
        yafl_fiber_suspend(NULL);

        fprintf(stderr, "[fiber %d] resumed\n", id);
        fflush(stderr);
    }

    fprintf(stderr, "[fiber %d] finishing with result %d\n", id, id + 1000);
    fflush(stderr);
    return (void *)(uintptr_t)(id + 1000);
}

int main(void) {
    fprintf(stderr, "=== yafl Many Fibers Test ===\n");
    fprintf(stderr, "[main] testing with %d fibers, %d iterations each\n", NUM_FIBERS, ITERATIONS);
    fflush(stderr);

    /* Create 100 fibers */
    fprintf(stderr, "[main] creating %d fibers\n", NUM_FIBERS);
    fflush(stderr);
    for (int i = 0; i < NUM_FIBERS; i++) {
        counters[i] = 0;
        fibers[i] = yafl_fiber_create(fiber_entry, 24 * 1024, YAFL_STACK_FLAGS_VMEM);
        assert(fibers[i] != NULL);

        /* Test status on created fiber */
        yafl_fiber_status_t status = yafl_fiber_status(fibers[i]);
        assert(status == YAFL_FIBER_STATUS_SUSPENDED);

        if (i == 0 || i == NUM_FIBERS - 1) {
            fprintf(stderr, "[main] fiber %d: created, status=%d\n", i, status);
            fflush(stderr);
        }
    }
    fprintf(stderr, "[main] all %d fibers created successfully\n", NUM_FIBERS);
    fflush(stderr);

    /* Round-robin scheduling until all done */
    fprintf(stderr, "[main] starting round-robin scheduling\n");
    fflush(stderr);

    int round = 0;
    bool all_done = false;
    while (!all_done) {
        all_done = true;
        int active_count = 0;

        for (int i = 0; i < NUM_FIBERS; i++) {
            yafl_fiber_status_t status = yafl_fiber_status(fibers[i]);
            if (status != YAFL_FIBER_STATUS_COMPLETE) {
                yafl_fiber_resume(fibers[i], (void *)(uintptr_t)i);
                all_done = false;
                active_count++;
            }
        }

        round++;
        if (round <= 3 || all_done) {
            fprintf(stderr, "[main] round %d: %d fibers still active\n", round, active_count);
            fflush(stderr);
        }
    }

    fprintf(stderr, "[main] all fibers finished after %d rounds\n", round);
    fflush(stderr);

    /* Verify all fibers ran exactly ITERATIONS times */
    fprintf(stderr, "[main] verifying all fibers completed %d iterations\n", ITERATIONS);
    fflush(stderr);

    for (int i = 0; i < NUM_FIBERS; i++) {
        assert(counters[i] == ITERATIONS);

        yafl_fiber_status_t status = yafl_fiber_status(fibers[i]);
        assert(status == YAFL_FIBER_STATUS_COMPLETE);

        if (i == 0 || i == NUM_FIBERS - 1) {
            fprintf(stderr, "[main] fiber %d: counter=%d, status=%d\n", i, counters[i], status);
            fflush(stderr);
        }

        yafl_fiber_destroy(fibers[i]);
    }

    fprintf(stderr, "[main] all fibers verified and destroyed\n");
    fflush(stderr);

    fprintf(stderr, "\nPASS: %d fibers x %d iterations = %d total context switches\n", NUM_FIBERS,
            ITERATIONS, NUM_FIBERS * ITERATIONS);
    fflush(stderr);
    return 0;
}
