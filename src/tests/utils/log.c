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

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "log.h"
#include "buf.h"
#if defined(_WIN32) && !defined(_MSC_VER)
#include <windows.h>
#endif

/* Guard for MSVC which doesn't support C11 stdatomic.h */
#if defined(_MSC_VER)
    /* Microsoft Visual C++ compiler */
    #include <windows.h>
    /* Define atomic types and operations for MSVC */
    #define _Atomic volatile
    #define atomic_fetch_add(obj, arg) InterlockedExchangeAdd(obj, arg)
    #define LOCK_INIT false
#else
    /* Standard C11 atomics for other compilers */
    #include <stdatomic.h>
    #define LOCK_INIT false
#endif



/*
 * Debugging support.
 */


static _Atomic log_level_t global_debug_level = LOG_LEVEL_NONE;
static _Atomic int thread_num_lock = 0;  /* 0 = unlocked, 1 = locked */
static _Atomic uint32_t thread_num = 1;

/*
 * Keep the thread ID and the tag ID thread local.
 */

#if defined(_MSC_VER) && (_MSC_VER < 1900) // Visual Studio before 2015
    #define THREAD_LOCAL __declspec(thread)
#else
    // C11 standard thread local storage
    #define THREAD_LOCAL _Thread_local
#endif

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
        int expected = 0;
        int desired = 1;

#if defined(_MSC_VER)
        /* MSVC uses InterlockedCompareExchange */
        while(InterlockedCompareExchange((volatile LONG*)&thread_num_lock, desired, expected) != expected) {
            /* Busy wait - spinlock */
        }
#else
        /* Standard C11 atomic compare exchange - use atomic_compare_exchange_strong */
        while(!atomic_compare_exchange_strong(&thread_num_lock, &expected, desired)) {
            expected = 0;
            /* Busy wait - spinlock */
        }
#endif

        /* Check again inside the lock - another thread may have initialized while we waited */
        if(!this_thread_num) {
            /* Atomic fetch add, returning the old value */
            this_thread_num = atomic_fetch_add(&thread_num, 1);
        }

        /* Release the lock */
#if defined(_MSC_VER)
        InterlockedExchange((volatile LONG*)&thread_num_lock, 0);
#else
        atomic_store(&thread_num_lock, 0);
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
    // NOLINTNEXTLINE
    snprintf(prefix, sizeof(prefix), "%04d-%02d-%02d %02d:%02d:%02d.%03d thread(%u) %s %s:%d %s\n",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec, remainder_ms,
             get_thread_id(), log_level_name[debug_level], func, line_num, templ);
    prefix[sizeof(prefix) - 1] = 0;

    /* Format and emit the final message */
    va_start(va, templ);
    // NOLINTNEXTLINE
    (void)vsnprintf(output, sizeof(output), prefix, va);
    fputs(output, stderr);
    fflush(stderr);
    va_end(va);
}


#define COLUMNS (16)

/**
 * @brief Log a buffer's contents as hex.
 * 
 * @param func name of the function in which the log is generated
 * @param line_num line number in the source file
 * @param lvl log level
 * @param buf buffer containing the bytes to log
 */
void log_bytes_impl(const char *func, int line_num, log_level_t lvl, buf_t *buf) {
    if (!buf) {
        log_impl(func, line_num, lvl, "<null>");
        return;
    }

    size_t count = buf_read_size(buf);
    const uint8_t *data = buf_read_ptr(buf);

    if (!data || count == 0) {
        log_impl(func, line_num, lvl, "<empty>");
        return;
    }

    for (size_t row = 0; row < (count + (COLUMNS - 1)) / COLUMNS; ++row) {
        char row_buf[(COLUMNS * 3) + 6] = {0};
        char *p = row_buf;

        p += sprintf(p, "%04zx:", row * COLUMNS);

        size_t start = row * COLUMNS;
        size_t end = start + COLUMNS;

        if (end > count) { end = count; }

        for (size_t i = start; i < end; ++i) {
            p += sprintf(p, " %02x", (unsigned)data[i]);
        }

        log_impl(func, line_num, lvl, "%s", row_buf);
    }
}


