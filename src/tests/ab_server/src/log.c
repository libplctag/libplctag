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

#include "compat.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "log.h"

/* Thread-local storage */
#if defined(_MSC_VER)
    #define THREAD_LOCAL __declspec(thread)
#else
    #define THREAD_LOCAL __thread
#endif



/*
 * Debugging support.
 */

static volatile log_level_t global_debug_level = LOG_LEVEL_NONE;
static volatile int thread_num_lock = 0;  /* 0 = unlocked, 1 = locked */
static volatile uint32_t thread_num = 1;

/*
 * Keep the thread ID thread local.
 * Using volatile for lock to ensure it's visible across threads.
 */

static THREAD_LOCAL uint32_t this_thread_num = 0;


log_level_t log_set_level(log_level_t level) {
    log_level_t old_level = global_debug_level;

    global_debug_level = level;

    return old_level;
}


log_level_t log_get_level(void) { return global_debug_level; }



static uint32_t get_thread_id(void) {
    if(!this_thread_num) {
        /* Use spinlock to ensure only one thread initializes at a time */
#if defined(_MSC_VER)
        /* MSVC uses InterlockedCompareExchange */
        int expected = 0;
        int desired = 1;
        while(InterlockedCompareExchange((volatile LONG*)&thread_num_lock, desired, expected) != expected) {
            /* Busy wait - spinlock */
        }

        /* Check again inside the lock - another thread may have initialized while we waited */
        if(!this_thread_num) {
            /* Increment and return old value */
            this_thread_num = (uint32_t)InterlockedIncrement((volatile LONG*)&thread_num) - 1;
        }

        /* Release the lock */
        InterlockedExchange((volatile LONG*)&thread_num_lock, 0);
#else
        /* POSIX: Use GCC __atomic builtins for musl compatibility */
        while(__atomic_test_and_set(&thread_num_lock, __ATOMIC_SEQ_CST)) {
            /* Busy wait - spinlock */
        }

        /* Check again inside the lock - another thread may have initialized while we waited */
        if(!this_thread_num) {
            /* Fetch and increment */
            this_thread_num = __atomic_fetch_add(&thread_num, 1, __ATOMIC_SEQ_CST);
        }

        /* Release the lock */
        __atomic_clear(&thread_num_lock, __ATOMIC_SEQ_CST);
#endif
    }

    return this_thread_num;
}


static const char *log_level_name[LOG_LEVEL_END] = {"NONE", "ERROR", "WARN", "INFO", "DETAIL", "SPEW"};

/**
 * @brief Log a message.
 *
 * @param func name of the function in which the log is generated
 * @param line_num line number in the source file
 * @param debug_level log level
 * @param templ format string for the log message
 * @param ... additional arguments for the format string
 */
extern void log_impl(const char *func, int line_num, log_level_t debug_level, const char *templ, ...) {
    va_list va;
    char prefix[1000]; /* MAGIC */
    char output[1000];

    /* Gather current time in milliseconds since Unix epoch */
    int64_t epoch_ms = 0;
#if defined(_WIN32)
    /* Windows: FILETIME (100ns intervals since 1601-01-01) */
    FILETIME ft;
    ULARGE_INTEGER epoch_time;
    GetSystemTimeAsFileTime(&ft);
    epoch_time.LowPart  = ft.dwLowDateTime;
    epoch_time.HighPart = ft.dwHighDateTime;
    epoch_ms = (int64_t)((epoch_time.QuadPart - 116444736000000000ULL) / 10000); /* -> ms */
#else
    /* POSIX: clock_gettime(CLOCK_REALTIME) */
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
        epoch_ms = 0;
    } else {
        epoch_ms = (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
    }
#endif

    int remainder_ms = (int)(epoch_ms % 1000);
    time_t epoch = (time_t)(epoch_ms / 1000);
    struct tm t;

#if defined(_WIN32)
    /* Windows: thread-safe time breakdown */
    localtime_s(&t, &epoch);
#else
    /* POSIX: thread-safe time breakdown */
    (void)localtime_r(&epoch, &t); /* ignore failure for logging */
#endif

    /* Build log prefix with timestamp, thread id, level, and site */
    /* Add bounds checking for debug_level to prevent array out-of-bounds */
    const char *level_str = "UNKNOWN";
    if(debug_level >= 0 && debug_level < LOG_LEVEL_END) {
        level_str = log_level_name[debug_level];
    }

    // NOLINTNEXTLINE
    snprintf(prefix, sizeof(prefix), "%04d-%02d-%02d %02d:%02d:%02d.%03d thread(%u) %s %s:%d %s\n",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec, remainder_ms,
             get_thread_id(), level_str, func, line_num, templ);
    prefix[sizeof(prefix) - 1] = 0;

    /* Format and emit the final message */
    va_start(va, templ);
    // NOLINTNEXTLINE
    (void)vsnprintf(output, sizeof(output), prefix, va);
    fputs(output, stderr);
    fflush(stderr);
    va_end(va);
}
