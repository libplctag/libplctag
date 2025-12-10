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
uint64_t util_time_ms(void) {
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
    /* Windows: use GetSystemTimeAsFileTime for wall-clock time since Unix epoch */
    FILETIME ft;
    ULARGE_INTEGER uli;

    GetSystemTimeAsFileTime(&ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;

    /* Convert from Windows FILETIME (100ns intervals since 1601-01-01)
     * to Unix epoch (microseconds since 1970-01-01).
     * The difference between 1601 and 1970 is 116444736000000000 in 100ns units.
     */
    int64_t intervals_since_epoch = (int64_t)uli.QuadPart - 116444736000000000LL;
    return intervals_since_epoch / 10;  /* Convert 100ns units to microseconds */
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((int64_t)tv.tv_sec * 1000000) + (int64_t)tv.tv_usec;
#endif
}









/******************************************************************/

#ifdef _WIN32


static void (*interrupt_handler)(void) = NULL;

static void interrupt_handler_wrapper(void) {
    if(interrupt_handler) { interrupt_handler(); }
}


/* straight from MS' web site */
BOOL WINAPI CtrlHandler(DWORD fdwCtrlType) {
    switch(fdwCtrlType) {
            /* ^C. */
        case CTRL_C_EVENT:
            interrupt_handler_wrapper();
            return TRUE;

            /*  */
        case CTRL_CLOSE_EVENT:
            interrupt_handler_wrapper();
            return TRUE;

            /* Pass other signals to the next handler. */
        case CTRL_BREAK_EVENT: interrupt_handler_wrapper(); return FALSE;

        case CTRL_LOGOFF_EVENT: interrupt_handler_wrapper(); return FALSE;

        case CTRL_SHUTDOWN_EVENT: interrupt_handler_wrapper(); return FALSE;

        default: return FALSE;
    }
}

int util_set_interrupt_handler(void (*handler)(void)) {
    interrupt_handler = handler;

    /* FIXME - this can fail! */
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    return 0;
}

#else

#include <signal.h>

static void (*interrupt_handler)(void) = NULL;

static void interrupt_handler_wrapper(int sig) {
    (void)sig;

    if(interrupt_handler) { interrupt_handler(); }
}

int util_set_interrupt_handler(void (*handler)(void)) {
    interrupt_handler = handler;

    signal(SIGINT, interrupt_handler_wrapper);
    signal(SIGTERM, interrupt_handler_wrapper);
    signal(SIGHUP, interrupt_handler_wrapper);

    return 0;
}

#endif


#ifdef _WIN32

void util_sleep_ms(int ms) {
    if(ms <= 0) { return; }

    Sleep((DWORD)(unsigned int)ms);
    return;
}

#else

void util_sleep_ms(int ms) {
    struct timeval tv;

    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;

    select(0, NULL, NULL, NULL, &tv);
}

#endif

