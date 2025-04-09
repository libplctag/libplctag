/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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

#include "compat_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * This file contains useful utilities for the sample programs.
 */


/*
 * system_time_ms
 *
 * Return current system time in millisecond units.  This is NOT an
 * Unix epoch time.  Windows uses a different epoch starting 1/1/1601.
 */

#if defined(POSIX_PLATFORM)

#    include <pthread.h>
#    include <sched.h>
#    include <signal.h>
#    include <time.h>
#    include <unistd.h>

int64_t system_time_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

int system_sleep_ms(uint32_t sleep_duration_ms, uint32_t *remaining_duration_ms) {
    struct timespec duration_ts;
    struct timespec remaining_ts;

    duration_ts.tv_sec = sleep_duration_ms / 1000;
    duration_ts.tv_nsec = (sleep_duration_ms % 1000) * 1000000;

    nanosleep(&duration_ts, &remaining_ts);

    if(remaining_duration_ms) {
        *remaining_duration_ms = (uint32_t)(remaining_ts.tv_sec * 1000 + remaining_ts.tv_nsec / 1000000);
    }

    return 0;
}

void system_yield(void) { sched_yield(); }


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

    return 0;
}


#    ifdef __APPLE__
/* macOS does not support pthread_mutex_timedlock() */
int pthread_mutex_timedlock(pthread_mutex_t *mtx, const struct timespec *abstime) {
    int64_t abs_timeout_ms = (int64_t)(abstime->tv_sec * 1000 + abstime->tv_nsec / 1000000);
    int64_t start_time_ms = system_time_ms();

    if(!mtx) { return -1; }

    while(1) {
        if(pthread_mutex_trylock(mtx)) { return 0; }

        if(system_time_ms() - start_time_ms >= abs_timeout_ms) { return -1; /* timeout */ }

        /* yield the CPU */
        system_yield();
    }

    return 0;
}

#    endif


#elif defined(WINDOWS_PLATFORM)

#    include <process.h>
#    include <processthreadsapi.h>

int64_t system_time_ms(void) {
    FILETIME ft;
    int64_t res;

    GetSystemTimeAsFileTime(&ft);

    /* calculate time as 100ns increments since Jan 1, 1601. */
    res = (int64_t)(ft.dwLowDateTime) + ((int64_t)(ft.dwHighDateTime) << 32);

    /* get time in ms */
    res = res / 10000;

    return res;
}


int system_sleep_ms(uint32_t sleep_duration_ms, uint32_t *remaining_duration_ms) {
    int64_t start_time_ms = system_time_ms() + sleep_duration_ms;
    int64_t end_time_ms = start_time_ms + sleep_duration_ms;

    Sleep(sleep_duration_ms);

    if(remaining_duration_ms) {
        int64_t remaining_ms = end_time_ms - system_time_ms();

        *remaining_duration_ms = (remaining_ms < 0) ? 0 : (uint32_t)remaining_ms;
    }

    return 0;
}

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

int set_interrupt_handler(void (*handler)(void)) {
    interrupt_handler = handler;

    /* FIXME - this can fail! */
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    return 0;
}


/* threads */
int pthread_create(pthread_t *restrict thread, const pthread_attr_t *restrict attr, void *(*start_routine)(void *),
                   void *restrict arg) {
    *thread = (HANDLE)_beginthreadex(NULL, 0, (unsigned int(__stdcall *)(void *))start_routine, arg, 0, NULL);
    return *thread ? 0 : -1;
}


int pthread_detach(pthread_t thread) {
    CloseHandle(thread);

    return 0;
}


void pthread_exit(void *retval) {
    unsigned int temp_return_val = 0;

    if(retval) { temp_return_val = *((unsigned int *)retval); }

    _endthreadex(temp_return_val);
}


int pthread_join(pthread_t thread, void **retval) {
    if(!thread) { return -1; }

    if(WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0) { return -1; }

    if(retval) {
        DWORD temp_ret_val = 0;
        GetExitCodeThread(thread, (LPDWORD)&temp_ret_val);
        *retval = (void *)(intptr_t)temp_ret_val;
    }

    CloseHandle(thread);

    return 0;
}


pthread_t pthread_self(void) { return (pthread_t)GetCurrentThread(); }


int pthread_once(pthread_once_t *once_control, void (*init_routine)(void)) {
    if(!once_control) { return -1; }

    if(InterlockedCompareExchange((volatile long *)once_control, 1, 0) == 0) { init_routine(); }

    return 0;
}


void system_yield(void) { SwitchToThread(); }


/* mutexes*/
/*
Need to emulate the following


pthread_mutex_t fastmutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t recmutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
pthread_mutex_t errchkmutex = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;
*/


int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *mutexattr) {
    (void)mutexattr;

    InitializeCriticalSection(mutex);
    return 0;
}


int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    DeleteCriticalSection(mutex);

    return 0;
}


int pthread_mutex_lock(pthread_mutex_t *mutex) {
    EnterCriticalSection(mutex);

    return 0;
}


int pthread_mutex_timedlock(pthread_mutex_t *restrict mutex, const struct timespec *restrict abs_timeout_time) {
    DWORD ms = (DWORD)(abs_timeout_time->tv_sec * 1000 + abs_timeout_time->tv_nsec / 1000000);
    DWORD start_time = GetTickCount();

    while(1) {
        if(TryEnterCriticalSection(mutex)) { return 0; }

        if(GetTickCount() - start_time >= ms) {
            return -1;  // Timeout
        }

        /* yield the CPU */
        SwitchToThread();
    }

    return 0;
}


int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    if(TryEnterCriticalSection(mutex)) {
        return 0;
    } else {
        return -1;
    }
}


int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    LeaveCriticalSection(mutex); /* FIXME- what happens if the mutex was not locked? */
    return 0;
}


/* condition variables */

int pthread_cond_init(pthread_cond_t *cond, pthread_condattr_t *cond_attr) {
    (void)cond_attr;

    InitializeConditionVariable(cond);
    return 0;
}

int pthread_cond_signal(pthread_cond_t *cond) {
    WakeConditionVariable(cond);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
    WakeAllConditionVariable(cond);
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    SleepConditionVariableCS(cond, mutex, INFINITE); /* Can this be interrupted? */
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abs_timeout_time) {
    int64_t start_time_ms = system_time_ms();
    int64_t end_time_ms = start_time_ms + (int64_t)(abs_timeout_time->tv_sec * 1000 + abs_timeout_time->tv_nsec / 1000000);

    int64_t delta_ms = end_time_ms - start_time_ms;

    if(delta_ms > LONG_MAX) { delta_ms = LONG_MAX; }

    if(delta_ms < 0) { delta_ms = 0; }

    if(SleepConditionVariableCS(cond, mutex, (DWORD)delta_ms)) {
        return 0;
    } else {
        return -1;
    }
}

int pthread_cond_destroy(pthread_cond_t *cond) {
    (void)cond;

    /* nothing to do on Windows */
    return 0;
}


#else
#    error "Not a supported platform!"
#endif

int pthread_mutex_timedlock_ms(pthread_mutex_t *mtx, const uint32_t timeout_duration_ms) {
    struct timespec abs_ts;

    abs_ts.tv_sec = timeout_duration_ms / 1000;
    abs_ts.tv_nsec = (timeout_duration_ms % 1000) * 1000000;

    if(pthread_mutex_timedlock(mtx, &abs_ts) == 0) {
        return 0;
    } else {
        return -1;
    }

    return 0;
}

int pthread_cond_timedwait_ms(pthread_cond_t *cond, pthread_mutex_t *mtx, const uint32_t timeout_duration_ms) {
    struct timespec abs_ts;

    abs_ts.tv_sec = timeout_duration_ms / 1000;
    abs_ts.tv_nsec = (timeout_duration_ms % 1000) * 1000000;

    if(pthread_cond_timedwait(cond, mtx, &abs_ts) == 0) {
        return 0;
    } else {
        return -1;
    }
}
