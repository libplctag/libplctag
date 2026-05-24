/*
 * bench_fiber_switch.c — null-fiber micro-benchmark
 *
 * Measures the raw cost of yafl_fiber_resume() / yafl_fiber_suspend()
 * round-trips with no payload work.  This isolates the context-switch
 * overhead from any I/O, poll(), or application logic.
 *
 * Usage: bench_fiber_switch [iterations]
 *   Default: 10,000,000 iterations
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>

#include "yafl.h"
#include "utils.h" /* util_time_us() */

#ifdef _WIN32
#    include <windows.h>
#    include <psapi.h>
#else
#    include <sys/resource.h>
#endif


static void get_cpu_time(double *user_ms, double *system_ms) {
#ifdef _WIN32
    FILETIME create, exit, kernel, user_ft;
    GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user_ft);
    uint64_t k = ((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
    uint64_t u = ((uint64_t)user_ft.dwHighDateTime << 32) | user_ft.dwLowDateTime;
    *system_ms = (double)k / 10000.0;
    *user_ms = (double)u / 10000.0;
#else
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    *user_ms = (double)ru.ru_utime.tv_sec * 1000.0 + (double)ru.ru_utime.tv_usec / 1000.0;
    *system_ms = (double)ru.ru_stime.tv_sec * 1000.0 + (double)ru.ru_stime.tv_usec / 1000.0;
#endif
}


/* The null fiber: suspends in a tight loop forever. */
static void *null_fiber_fn(void *arg) {
    (void)arg;
    for(;;) { yafl_fiber_suspend(NULL); }
    return NULL; /* unreachable */
}


int main(int argc, char *argv[]) {
    int64_t iterations = 10000000; /* default: 10M */

    if(argc > 1) {
        iterations = atoll(argv[1]);
        if(iterations <= 0) {
            fprintf(stderr, "Usage: %s [iterations]\n", argv[0]);
            return 1;
        }
    }

    fprintf(stderr, "bench_fiber_switch: %" PRId64 " resume/suspend round-trips\n", iterations);

    /* Create the null fiber with a small stack */
    yafl_fiber_t *fiber = yafl_fiber_create(null_fiber_fn, 4096, YAFL_STACK_FLAGS_VMEM);
    if(!fiber) {
        fprintf(stderr, "Failed to create fiber\n");
        return 1;
    }

    /* Initial resume to get the fiber to its first suspend point */
    yafl_fiber_resume(fiber, NULL);

    /* Warm up */
    for(int i = 0; i < 1000; i++) { yafl_fiber_resume(fiber, NULL); }

    /* Measure */
    double cpu_user_start, cpu_sys_start;
    get_cpu_time(&cpu_user_start, &cpu_sys_start);
    int64_t wall_start = util_time_us();

    for(int64_t i = 0; i < iterations; i++) { yafl_fiber_resume(fiber, NULL); }

    int64_t wall_end = util_time_us();
    double cpu_user_end, cpu_sys_end;
    get_cpu_time(&cpu_user_end, &cpu_sys_end);

    /* Results */
    double wall_us = (double)(wall_end - wall_start);
    double cpu_user_ms = cpu_user_end - cpu_user_start;
    double cpu_sys_ms = cpu_sys_end - cpu_sys_start;
    double ns_per_trip = wall_us * 1000.0 / (double)iterations;

    fprintf(stderr, "\n");
    fprintf(stderr, "Wall time:      %10.1f ms\n", wall_us / 1000.0);
    fprintf(stderr, "User CPU:       %10.1f ms\n", cpu_user_ms);
    fprintf(stderr, "System CPU:     %10.1f ms\n", cpu_sys_ms);
    fprintf(stderr, "Iterations:     %10" PRId64 "\n", iterations);
    fprintf(stderr, "Per round-trip: %10.1f ns  (resume + fiber-suspend)\n", ns_per_trip);
    fprintf(stderr, "Throughput:     %10.1f M round-trips/sec\n", (double)iterations / wall_us);

    yafl_fiber_destroy(fiber);
    return 0;
}
