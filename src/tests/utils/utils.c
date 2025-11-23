#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#include <sys/time.h>
#endif

#include "utils.h"

/**
 * @brief Get current time in milliseconds.
 *
 * Uses platform-specific functions.
 */
uint64_t util_get_time_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

/**
 * @brief Get current time in microseconds.
 *
 * Used for performance analysis and latency measurements.
 */
int64_t util_time_us(void) {
#ifdef _WIN32
    /* Windows: use QueryPerformanceCounter for high-resolution timing */
    static LARGE_INTEGER frequency = {0};
    LARGE_INTEGER counter;

    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }

    QueryPerformanceCounter(&counter);
    return (int64_t)((counter.QuadPart * 1000000) / frequency.QuadPart);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((int64_t)tv.tv_sec * 1000000) + (int64_t)tv.tv_usec;
#endif
}
