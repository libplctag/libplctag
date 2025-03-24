/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever     *
 * you choose.                                                             *
 *                                                                         *
 * MPL 2.0:                                                                *
 *                                                                         *
 *   This Source Code Form is subject to the terms of the Mozilla Public   *
 *   License, v. 2.0. If a copy of the MPL was not distributed with this   *
 *   file, You can obtain one at http://mozilla.org/MPL/2.0/.              *
 *                                                                         *
 *                                                                         *
 * LGPL 2:                                                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * This file contains useful utilities for the sample programs.
 */


/*
 * util_time_ms
 *
 * Return current system time in millisecond units.  This is NOT an
 * Unix epoch time.  Windows uses a different epoch starting 1/1/1601.
 */

#if defined(__unix__) || defined(APPLE) || defined(__APPLE__) || defined(__MACH__) || defined(__linux__)

#    include <time.h>

int64_t util_time_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}


#elif defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__) || defined(WIN64) || defined(_WIN64)
int64_t util_time_ms(void) {
    FILETIME ft;
    int64_t res;

    GetSystemTimeAsFileTime(&ft);

    /* calculate time as 100ns increments since Jan 1, 1601. */
    res = (int64_t)(ft.dwLowDateTime) + ((int64_t)(ft.dwHighDateTime) << 32);

    /* get time in ms */
    res = res / 10000;

    return res;
}
#else
#    error "Not a supported platform!"
#endif

/* implement C11 compatibility functions */
#ifdef __STDC_NO_THREADS__

#    if defined(__unix__) || defined(APPLE) || defined(__APPLE__) || defined(__MACH__) || defined(__linux__)

/* pthread-base implemenation of C11 threads */
#        include <errno.h>
#        include <pthread.h>

/* threads */

int thrd_create(thrd_t *thrd, int (*func)(void *), void *arg_ptr) {
    pthread_attr_t attr;
    int rc;

    /* FIXME - do we need to set reentrant here too? */
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    rc = pthread_create(thrd, &attr, (void *(*)(void *))func, arg_ptr);

    pthread_attr_destroy(&attr);

    return rc == 0 ? thrd_success : thrd_error;
}

thrd_t thrd_current(void) { return pthread_self(); }

int thrd_detach(thrd_t thrd) { return pthread_detach(thrd) == 0 ? thrd_success : thrd_error; }

int thrd_equal(thrd_t first, thrd_t second) { return pthread_equal(first, second); }

int thrd_exit(int res) {
    pthread_exit((void *)(intptr_t)res);
    return thrd_success;
}

int thrd_join(thrd_t thrd, int *result) {
    void *res;
    int rc = pthread_join(thrd, &res);
    if(rc == 0 && result) { *result = (int)(intptr_t)res; }
    return rc == 0 ? thrd_success : thrd_error;
}

int thrd_sleep(const struct timespec *sleep_duration, struct timespec *remaining_duration) {
    int rc;
    do { rc = nanosleep(sleep_duration, remaining_duration); } while(rc && errno == EINTR);
    return rc == 0 ? thrd_success : thrd_error;
}

void thrd_yield(void) { sched_yield(); }

/* mutexes */

int mtx_init(mtx_t *mtx, int type) {
    pthread_mutexattr_t attr;
    int rc;

    pthread_mutexattr_init(&attr);

    if(type & mtx_recursive) { pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE); }

    rc = pthread_mutex_init(mtx, &attr);

    pthread_mutexattr_destroy(&attr);

    return rc == 0 ? thrd_success : thrd_error;
}

int mtx_lock(mtx_t *mtx) { return pthread_mutex_lock(mtx) == 0 ? thrd_success : thrd_error; }

#        if !defined(APPLE) && !defined(__APPLE__) && !defined(__MACH__)

/* ah, we actually have a full implementation of pthreads. */

int mtx_timedlock(mtx_t *mtx, const struct timespec *abs_timeout_time) {
    if(pthread_mutex_timedlock(mtx, abs_timeout_time) == 0) {
        return thrd_success;
    } else {
        return thrd_error;
    }
}

#        else

/* Why, Apple?  Why? */

int mtx_timedlock(mtx_t *mtx, const struct timespec *abs_timeout_time) {
    int64_t current_time_ms;
    int64_t timeout_time_ms;

    if(mtx_trylock(mtx) == thrd_success) { return thrd_success; }

    timeout_time_ms = (int64_t)abs_timeout_time->tv_sec * 1000 + (int64_t)abs_timeout_time->tv_nsec / 1000000;

    while(1) {
        current_time_ms = util_time_ms();
        if(current_time_ms >= timeout_time_ms) {
            return thrd_error;  // Timeout
        }

        if(mtx_trylock(mtx) == thrd_success) { return thrd_success; }
        thrd_sleep_ms(10, NULL);
    }
}

#        endif

int mtx_trylock(mtx_t *mtx) { return pthread_mutex_trylock(mtx) == 0 ? thrd_success : thrd_error; }

int mtx_unlock(mtx_t *mtx) { return pthread_mutex_unlock(mtx) == 0 ? thrd_success : thrd_error; }

int mtx_destroy(mtx_t *mtx) { return pthread_mutex_destroy(mtx) == 0 ? thrd_success : thrd_error; }

/* condition variables */

int cnd_broadcast(cnd_t *cond) { return pthread_cond_broadcast(cond) == 0 ? thrd_success : thrd_error; }

int cnd_destroy(cnd_t *cond) { return pthread_cond_destroy(cond) == 0 ? thrd_success : thrd_error; }

int cnd_init(cnd_t *cond) { return pthread_cond_init(cond, NULL) == 0 ? thrd_success : thrd_error; }

int cnd_signal(cnd_t *cond) { return pthread_cond_signal(cond) == 0 ? thrd_success : thrd_error; }

int cnd_timedwait(cnd_t *cond, mtx_t *mtx, const struct timespec *abs_timeout_time) {
    return pthread_cond_timedwait(cond, mtx, abs_timeout_time) == 0 ? thrd_success : thrd_error;
}

int cnd_wait(cnd_t *cond, mtx_t *mtx) { return pthread_cond_wait(cond, mtx) == 0 ? thrd_success : thrd_error; }

#    elif defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__) || defined(WIN64) || defined(_WIN64)
#        define WIN32_LEAN_AND_MEAN
#        include <process.h>
#        include <windows.h>

/* Windows thread-based implementation fo C11 threads */

/* threads */

int thrd_create(thrd_t *thrd, int (*func)(void *), void *arg_ptr) {
    *thrd = (HANDLE)_beginthreadex(NULL, 0, (unsigned int(__stdcall *)(void *))func, arg_ptr, 0, NULL);
    return *thrd ? thrd_success : thrd_error;
}

thrd_t thrd_current(void) { return GetCurrentThread(); }

int thrd_detach(thrd_t thrd) {
    CloseHandle(thrd);
    return thrd_success;
}

int thrd_equal(thrd_t first, thrd_t second) { return first == second; }

int thrd_exit(int res) {
    _endthreadex(res);
    return thrd_success;
}

int thrd_join(thrd_t thrd, int *result) {
    DWORD res;
    WaitForSingleObject(thrd, INFINITE);
    GetExitCodeThread(thrd, &res);
    if(result) { *result = (int)res; }
    CloseHandle(thrd);
    return thrd_success;
}

int thrd_sleep(const struct timespec *sleep_duration, struct timespec *remaining_duration) {
    DWORD ms = (DWORD)(sleep_duration->tv_sec * 1000 + sleep_duration->tv_nsec / 1000000);
    Sleep(ms);
    if(remaining_duration) {
        remaining_duration->tv_sec = 0;
        remaining_duration->tv_nsec = 0;
    }
    return thrd_success;
}

void thrd_yield(void) { SwitchToThread(); }

/* mutexes */

int mtx_init(mtx_t *mtx, int type) {
    (void)type; /* FIXME - double check that Windows does not have something here? */
    InitializeCriticalSection(mtx);
    return thrd_success;
}

int mtx_lock(mtx_t *mtx) {
    EnterCriticalSection(mtx);
    return thrd_success;
}

int mtx_timedlock(mtx_t *mtx, const struct timespec *abs_timeout_time) {
    DWORD ms = (DWORD)(abs_timeout_time->tv_sec * 1000 + abs_timeout_time->tv_nsec / 1000000);
    DWORD start_time = GetTickCount();

    while(1) {
        if(TryEnterCriticalSection(mtx)) { return thrd_success; }

        if(GetTickCount() - start_time >= ms) {
            return thrd_error;  // Timeout
        }
        Sleep(5); /* FIXME - should this just be yield? */
    }
}

int mtx_trylock(mtx_t *mtx) {
    if(TryEnterCriticalSection(mtx)) {
        return thrd_success;
    } else {
        return thrd_error;
    }
}

int mtx_unlock(mtx_t *mtx) {
    LeaveCriticalSection(mtx); /* FIXME- what happens if the mutex was not locked? */
    return thrd_success;
}

int mtx_destroy(mtx_t *mtx) {
    DeleteCriticalSection(mtx);
    return thrd_success;
}

/* condition variables */

int cnd_broadcast(cnd_t *cond) {
    WakeAllConditionVariable(cond);
    return thrd_success;
}

int cnd_destroy(cnd_t *cond) {
    /* nothing to do on Windows */
    return thrd_success;
}

int cnd_init(cnd_t *cond) {
    InitializeConditionVariable(cond);
    return thrd_success;
}

int cnd_signal(cnd_t *cond) {
    WakeConditionVariable(cond);
    return thrd_success;
}

int cnd_timedwait(cnd_t *cond, mtx_t *mtx, const struct timespec *abs_timeout_time) {
    DWORD ms = (DWORD)(abs_timeout_time->tv_sec * 1000 + abs_timeout_time->tv_nsec / 1000000);
    if(SleepConditionVariableCS(cond, mtx, ms)) {
        return thrd_success;
    } else {
        return thrd_error;
    }
}

int cnd_wait(cnd_t *cond, mtx_t *mtx) {
    SleepConditionVariableCS(cond, mtx, INFINITE); /* Can this be interrupted? */
    return thrd_success;
}


#    else
#        error "Not a supported platform!"
#    endif


#endif /* __STDC_NO_THREADS__ */


/* our helper functions for C11 waits */
int thrd_sleep_ms(uint32_t sleep_duration_ms, uint32_t *remaining_duration_ms) {
    int rc = thrd_success;
    int64_t start_time = util_time_ms();
    int64_t end_time = 0;
    struct timespec ts;

    ts.tv_sec = sleep_duration_ms / 1000;
    ts.tv_nsec = (sleep_duration_ms % 1000) * 1000000;

    rc = thrd_sleep(&ts, NULL);

    end_time = util_time_ms();

    if(remaining_duration_ms) {
        if(end_time <= start_time + sleep_duration_ms) {
            *remaining_duration_ms = (uint32_t)0;
        } else {
            *remaining_duration_ms = (uint32_t)(end_time - start_time);
        }
    }

    return rc;
}


int mtx_timedlock_ms(mtx_t *mtx, const uint32_t timeout_duration_ms, uint32_t *remaining_duration_ms) {
    int rc = thrd_success;
    int64_t start_time = util_time_ms();
    int64_t end_time = 0;
    struct timespec ts;

    ts.tv_sec = (long)(timeout_duration_ms / 1000);
    ts.tv_nsec = (long)((timeout_duration_ms % 1000) * 1000000);

    rc = mtx_timedlock(mtx, &ts);

    end_time = util_time_ms();

    if(remaining_duration_ms) {
        if(end_time <= start_time + timeout_duration_ms) {
            *remaining_duration_ms = (uint32_t)0;
        } else {
            *remaining_duration_ms = (uint32_t)(end_time - start_time);
        }
    }

    return rc;
}


int cnd_timedwait_ms(cnd_t *cond, mtx_t *mtx, const uint32_t timeout_duration_ms, uint32_t *remaining_duration_ms) {
    int rc = thrd_success;
    int64_t start_time = util_time_ms();
    int64_t end_time = 0;
    struct timespec ts;

    if(timeout_duration_ms == 0) { return thrd_error; }

    ts.tv_sec = (long)(timeout_duration_ms / 1000);
    ts.tv_nsec = (long)((timeout_duration_ms % 1000) * 1000000);

    rc = cnd_timedwait(cond, mtx, &ts);

    end_time = util_time_ms();

    if(remaining_duration_ms) {
        if(end_time <= start_time + timeout_duration_ms) {
            *remaining_duration_ms = (uint32_t)0;
        } else {
            *remaining_duration_ms = (uint32_t)(end_time - start_time);
        }
    }

    return rc;
}


#if defined(__unix__) || defined(APPLE) || defined(__APPLE__) || defined(__MACH__) || defined(__linux__)

#    include <signal.h>

static void (*interrupt_handler)(void) = NULL;

static void interrupt_handler_wrapper(int sig) {
    (void)sig;

    if(interrupt_handler) { interrupt_handler(); }
}

int set_interrupt_handler(void (*handler)(void)) {
    interrupt_handler = handler;

    signal(SIGINT, interrupt_handler_wrapper);
    signal(SIGTERM, interrupt_handler_wrapper);
    signal(SIGHUP, interrupt_handler_wrapper);

    return thrd_success;
}

#elif defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__) || defined(WIN64) || defined(_WIN64)

static void (*interrupt_handler)(void) = NULL;

static void interrupt_handler_wrapper(void) {
    if(interrupt_handler) { interrupt_handler(); }
}


/* straight from MS' web site :-) */
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

int set_interrupt_handler(void (*handler)(void)) {
    interrupt_handler = handler;

    /* FIXME - this can fail! */
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    return thrd_success;
}


#else
#    error "Unsupported platform!"
#endif


/* FIXME - move this all over into a compatibility/platform check header */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#    include <stdlib.h>

uint64_t util_random_u64(uint64_t upper_bound) {
    uint64_t random_number = 0;

    arc4random_buf(&random_number, sizeof(random_number));
    random_number %= upper_bound;

    return random_number;
}

#elif defined(__linux__)
#    include <stdlib.h>
#    include <sys/random.h>


uint64_t util_random_u64(uint64_t upper_bound) {
    uint64_t random_number = 0;

    if(upper_bound == 0) { return 0; }

    if(getrandom(&random_number, sizeof(random_number), GRND_NONBLOCK) < (ssize_t)sizeof(random_number)) {
        /* not enough entropy, do it the hard way. */
        srand((unsigned int)((uint64_t)time(NULL) ^ random_number));
        for(size_t i = 0; i < sizeof(random_number); ++i) { ((uint8_t *)&random_number)[i] ^= (uint8_t)(rand() % 256); }
    }

    random_number %= upper_bound;

    return random_number;
}


#elif defined(_WIN32) || defined(_WIN64)
#    include <wincrypt.h>
#    include <windows.h>


uint64_t util_random_u64(uint64_t upper_bound) {
    uint64_t random_number = 0;

    if(BCryptGenRandom(NULL, (PUCHAR)&random_number, sizeof(random_number), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        pdebug(DEBUG_WARN, "BCryptGenRandom failed, returning error");
        return RANDOM_U64_ERROR;
    }
    if(upper_bound == 0) {
        pdebug(DEBUG_WARN, "upper_bound is zero, returning 0");
        return 0;
    }
    random_number %= upper_bound;

    return random_number;
}

#else
#    error "Unsupported platform!"
#endif
