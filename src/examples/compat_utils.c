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

#if defined(POSIX_PLATFORM)

#    include <pthread.h>
#    include <sched.h>
#    include <signal.h>
#    include <time.h>
#    include <unistd.h>

/* threads */
int compat_thread_create(compat_thread_t *thread, void *(*start_routine)(void *), void *arg) {
    return pthread_create(thread, NULL, start_routine, arg);
}

int compat_thread_detach(compat_thread_t thread) { return pthread_detach(thread); }

void compat_thread_exit(void *retval) { pthread_exit(retval); }

int compat_thread_join(compat_thread_t thread, void **retval) { return pthread_join(thread, retval); }

compat_thread_t compat_thread_self(void) { return pthread_self(); }

int compat_thread_once(compat_once_t *once_control, void (*init_routine)(void)) {
    return pthread_once(once_control, init_routine);
}


void compat_thread_yield(void) { sched_yield(); }


/* mutexes */
int compat_mutex_init(compat_mutex_t *mutex) { return pthread_mutex_init(mutex, NULL); }

int compat_mutex_lock(compat_mutex_t *mutex) { return pthread_mutex_lock(mutex); }


#    ifndef __APPLE__
int compat_mutex_timedlock(compat_mutex_t *mutex, const uint32_t timeout_duration_ms) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    ts.tv_sec += (time_t)(timeout_duration_ms / 1000);
    ts.tv_nsec += (long)((timeout_duration_ms % 1000) * 1000000);

    return pthread_mutex_timedlock(mutex, &ts);
}

#    else /* __APPLE__ is defined */
/* macOS does not provide an implementation of pthread_mutex_timedlock() */
int compat_mutex_timedlock(compat_mutex_t *mutex, const uint32_t timeout_duration_ms) {
    int64_t end_time = compat_time_ms() + timeout_duration_ms;

    if(!mutex) { return -1; }

    while(1) {
        if(compat_mutex_trylock(mutex)) { return 0; }

        if(compat_time_ms() >= end_time) { return -1; /* timeout */ }

        /* yield the CPU */
        compat_thread_yield();
    }

    return 0;
}

#    endif /* __APPLE__ */


int compat_mutex_trylock(compat_mutex_t *mutex) { return pthread_mutex_trylock(mutex); }

int compat_mutex_unlock(compat_mutex_t *mutex) { return pthread_mutex_unlock(mutex); }

int compat_mutex_destroy(compat_mutex_t *mutex) { return pthread_mutex_destroy(mutex); }

/* condition variables */
int compat_cond_init(compat_cond_t *cond) { return pthread_cond_init(cond, NULL); }

int compat_cond_signal(compat_cond_t *cond) { return pthread_cond_signal(cond); }

int compat_cond_broadcast(compat_cond_t *cond) { return pthread_cond_broadcast(cond); }

int compat_cond_wait(compat_cond_t *cond, compat_mutex_t *mutex) { return pthread_cond_wait(cond, mutex); }

int compat_cond_timedwait(compat_cond_t *cond, compat_mutex_t *mutex, const uint32_t timeout_duration_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += (time_t)(timeout_duration_ms / 1000);
    ts.tv_nsec += (long)((timeout_duration_ms % 1000) * 1000000);
    if(ts.tv_nsec >= 1000000000) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000;
    }
    return pthread_cond_timedwait(cond, mutex, &ts);
}

int compat_cond_destroy(compat_cond_t *cond) { return pthread_cond_destroy(cond); }


int64_t compat_time_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

int compat_sleep_ms(uint32_t sleep_duration_ms, uint32_t *remaining_duration_ms) {
    struct timespec duration_ts;
    struct timespec remaining_ts;

    duration_ts.tv_sec = (time_t)(sleep_duration_ms / 1000);
    duration_ts.tv_nsec = (long)((sleep_duration_ms % 1000) * 1000000);

    nanosleep(&duration_ts, &remaining_ts);

    if(remaining_duration_ms) {
        *remaining_duration_ms = (uint32_t)(remaining_ts.tv_sec * 1000 + remaining_ts.tv_nsec / 1000000);
    }

    return 0;
}


static void (*interrupt_handler)(void) = NULL;

static void interrupt_handler_wrapper(int sig) {
    (void)sig;

    if(interrupt_handler) { interrupt_handler(); }
}

int compat_set_interrupt_handler(void (*handler)(void)) {
    interrupt_handler = handler;

    signal(SIGINT, interrupt_handler_wrapper);
    signal(SIGTERM, interrupt_handler_wrapper);
    signal(SIGHUP, interrupt_handler_wrapper);

    return 0;
}


#elif defined(WINDOWS_PLATFORM)

#    include <process.h>
#    include <processthreadsapi.h>


/* threads */
int compat_thread_create(compat_thread_t *thread, void *(*start_routine)(void *), void *arg) {
    *thread = (HANDLE)_beginthreadex(NULL, 0, (unsigned int(__stdcall *)(void *))start_routine, arg, 0, NULL);
    return *thread ? 0 : -1;
}


int compat_thread_detach(compat_thread_t thread) {
    CloseHandle(thread);

    return 0;
}


void compat_thread_exit(void *retval) {
    unsigned int temp_return_val = 0;

    if(retval) { temp_return_val = *((unsigned int *)retval); }

    _endthreadex(temp_return_val);
}


int compat_thread_join(compat_thread_t thread, void **retval) {
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


compat_thread_t compat_thread_self(void) { return (compat_thread_t)GetCurrentThread(); }


int compat_thread_once(compat_once_t *once_control, void (*init_routine)(void)) {
    if(!once_control) { return -1; }

    if(InterlockedCompareExchange((volatile long *)once_control, 1, 0) == 0) { init_routine(); }

    return 0;
}


void compat_thread_yield(void) { SwitchToThread(); }


/* Mutex functions */

int compat_mutex_init(compat_mutex_t *mutex) {
    InitializeCriticalSection(mutex);
    return 0;
}


int compat_mutex_destroy(compat_mutex_t *mutex) {
    DeleteCriticalSection(mutex);

    return 0;
}


int compat_mutex_lock(compat_mutex_t *mutex) {
    EnterCriticalSection(mutex);

    return 0;
}


int compat_mutex_timedlock(compat_mutex_t *mutex, const uint32_t timeout_duration_ms) {
    int64_t start_time = compat_time_ms();
    int64_t end_time = start_time + timeout_duration_ms;

    while(1) {
        if(TryEnterCriticalSection(mutex)) { return 0; }

        if(compat_time_ms() >= end_time) { return -1; /* we timed out */ }

        /* yield the CPU */
        SwitchToThread();
    }

    return 0;
}


int compat_mutex_trylock(compat_mutex_t *mutex) {
    if(TryEnterCriticalSection(mutex)) {
        return 0;
    } else {
        return -1;
    }
}


int compat_mutex_unlock(compat_mutex_t *mutex) {
    LeaveCriticalSection(mutex); /* FIXME- what happens if the mutex was not locked? */
    return 0;
}


/* condition variable functions */

int compat_cond_init(compat_cond_t *cond) {
    InitializeConditionVariable(cond);
    return 0;
}

int compat_cond_signal(compat_cond_t *cond) {
    WakeConditionVariable(cond);
    return 0;
}

int compat_cond_broadcast(compat_cond_t *cond) {
    WakeAllConditionVariable(cond);
    return 0;
}

int compat_cond_wait(compat_cond_t *cond, compat_mutex_t *mutex) {
    SleepConditionVariableCS(cond, mutex, INFINITE); /* Can this be interrupted? */
    return 0;
}

int compat_cond_timedwait(compat_cond_t *cond, compat_mutex_t *mutex, const uint32_t timeout_duration_ms) {
    if(SleepConditionVariableCS(cond, mutex, (DWORD)timeout_duration_ms)) {
        return 0;
    } else {
        return -1;
    }
}

int compat_cond_destroy(compat_cond_t *cond) {
    (void)cond;

    /* nothing to do on Windows */
    return 0;
}


int64_t compat_time_ms(void) {
    FILETIME ft;
    int64_t res;

    GetSystemTimeAsFileTime(&ft);

    /* calculate time as 100ns increments since Jan 1, 1601. */
    res = (int64_t)(ft.dwLowDateTime) + ((int64_t)(ft.dwHighDateTime) << 32);

    /* get time in ms */
    res = res / 10000;

    return res;
}


int compat_sleep_ms(uint32_t sleep_duration_ms, uint32_t *remaining_duration_ms) {
    int64_t start_time_ms = compat_time_ms() + sleep_duration_ms;
    int64_t end_time_ms = start_time_ms + sleep_duration_ms;

    Sleep(sleep_duration_ms);

    if(remaining_duration_ms) {
        int64_t remaining_ms = end_time_ms - compat_time_ms();

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

int compat_set_interrupt_handler(void (*handler)(void)) {
    interrupt_handler = handler;

    /* FIXME - this can fail! */
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    return 0;
}


#else
#    error "Not a supported platform!"
#endif

#define BUFFER_SIZE 1024


int compat_fprintf(FILE *out, const char *format, ...) {
    // ... inside a function ...
    char buffer[BUFFER_SIZE] = {0};
    int formatted_len;
    va_list va;

    /* Initialize the variable argument list */
    va_start(va, format);

    /* print the string into the buffer. */
    formatted_len = vsnprintf(buffer, sizeof(buffer), format, va);

    /* done with the vararg list */
    va_end(va);

    /* output the data if any */
    if(formatted_len > 0) { fputs(buffer, out); }

    return formatted_len;
}
