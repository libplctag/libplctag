/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
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
#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/version.h>
#include <platform.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <utils/atomic_utils.h>
#include <utils/debug.h>

#if defined(_WIN32) || defined(_WIN64)
#    include <windows.h>
#else
#    include <sys/time.h>
#endif


/*
 * Debugging support.
 */


static atomic_int32_t global_debug_level = ATOMIC_INT_STATIC_INIT; /* DEBUG_NONE == 0 */
static lock_t thread_num_lock = LOCK_INIT;
static volatile uint32_t thread_num = 1;

/* Logger callback and the lock guarding it. This must be a spinlock, not a real
 * mutex: mutex_lock_impl()/mutex_unlock_impl() call pdebug() themselves to trace
 * the lock attempt, and register/unregister below take this lock -- using a real
 * mutex here means those calls re-enter pdebug_impl() while still inside this same
 * critical section, recursing without ever completing the original lock attempt.
 * lock_acquire()/lock_release() are deliberately pdebug-free for exactly this
 * reason. lock_t is also valid from static initialization alone (LOCK_INIT is just
 * 0), unlike a mutex_p, which needs a runtime mutex_create() -- and pdebug_impl()/
 * debug_register_logger()/debug_unregister_logger() can all run before
 * initialize_modules() ever does, e.g. a caller may register a logger as their very
 * first API call.
 *
 * IMPORTANT: pdebug_impl() must NOT take this lock. It is a pure spin with no
 * yield, so a waiter burns CPU for as long as the holder takes. Emitting a log line
 * means an fputs() and periodic fflush() -- a write(2) -- so holding the lock across
 * output makes every thread in the process spin for the duration of another thread's
 * syscall. Instead pdebug_impl() uses the in-flight counter below to coordinate with
 * unregister, and does its output holding no lock of ours at all.
 */
static lock_t logger_callback_lock = LOCK_INIT;
static void (*volatile log_callback_func)(int32_t tag_id, int debug_level, const char *message);

/*
 * Count of pdebug_impl() calls that have loaded a non-NULL log_callback_func and may
 * still be inside it. Only ever raised while a callback is registered, so once
 * debug_unregister_logger() clears the pointer this is guaranteed to drain to zero:
 * later callers see NULL and never touch it.
 */
static atomic_int32_t log_callback_in_flight = ATOMIC_INT_STATIC_INIT;

/* Buffering control for stderr logging performance */
static atomic_bool stderr_buffering_initialized = ATOMIC_BOOL_STATIC_INIT;
static atomic_int32_t log_call_count = ATOMIC_INT_STATIC_INIT;


/* Module name lookup table is now defined in debug_generated.h */

/* Per-module debug levels - indexed directly by debug_module_t value. Written by
 * set_debug_level()/debug_module_set_level()/debug_set_all_modules() (rarely, from
 * application code) and read by debug_is_enabled() (constantly, on every pdebug()
 * call from every thread), so this needs real atomics, not just volatile. Static
 * storage duration zero-initializes every element to 0 == DEBUG_NONE. */
static atomic_int32_t debug_module_levels[DEBUG_MODULE_COUNT];


bool debug_is_enabled(debug_module_t module, int level) {
    return level > DEBUG_NONE && (unsigned)module < DEBUG_MODULE_COUNT && level <= atomic_get_int32(&debug_module_levels[module]);
}


static THREAD_LOCAL uint32_t this_thread_num = 0;


// /* only output the version once */
// static lock_t printed_version = LOCK_INIT;


int set_debug_level(int level) {
    int old_level = atomic_set_int32(&global_debug_level, level);

    /* Push the global level into every module slot so the inline
     * debug_is_enabled() check needs only a single array lookup. */
    for(int i = 0; i < DEBUG_MODULE_COUNT; i++) { atomic_set_int32(&debug_module_levels[i], level); }

    return old_level;
}


int get_debug_level(void) { return atomic_get_int32(&global_debug_level); }



void debug_module_set_level(debug_module_t module, int level) {
    if((unsigned)module < DEBUG_MODULE_COUNT) { atomic_set_int32(&debug_module_levels[module], level); }
}


int debug_module_get_level(debug_module_t module) {
    if((unsigned)module < DEBUG_MODULE_COUNT) { return atomic_get_int32(&debug_module_levels[module]); }
    return DEBUG_NONE;
}


void debug_set_all_modules(int level) {
    for(int i = 0; i < DEBUG_MODULE_COUNT; i++) { atomic_set_int32(&debug_module_levels[i], level); }
}



static uint32_t get_thread_id(void) {
    if(!this_thread_num) {
        spin_block(&thread_num_lock) {
            this_thread_num = thread_num;
            thread_num++;
        }
    }

    return this_thread_num;
}


static const char *format_module_name(debug_module_t module) {
    if((unsigned)module < DEBUG_MODULE_COUNT && debug_module_names[module]) { return debug_module_names[module]; }
    return "UNKNOWN";
}


static const char *debug_level_name[DEBUG_END] = {"NONE", "ERROR", "WARN", "INFO", "DETAIL", "SPEW"};


/*
 * Get current epoch time in microseconds.
 * Works on Linux, BSD, macOS, and Windows.
 */
static int64_t time_us(void) {
#if defined(_WIN32) || defined(_WIN64)
    /* Windows implementation using GetSystemTimePreciseAsFileTime (Windows 8+) */
    FILETIME ft;
    ULARGE_INTEGER uli;

    GetSystemTimePreciseAsFileTime(&ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;

    /* FILETIME is in 100-nanosecond intervals since Jan 1, 1601 */
    /* Convert to microseconds and adjust to Unix epoch (Jan 1, 1970) */
    /* Difference is 11644473600 seconds = 11644473600000000 microseconds */
    return (int64_t)((uli.QuadPart / 10) - 11644473600000000LL);
#else
    /* POSIX implementation using gettimeofday (Linux, BSD, macOS) */
    struct timeval tv;

    gettimeofday(&tv, NULL);

    return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
#endif
}


static void ensure_stderr_buffering(void) {
    /* Initialize stderr buffering exactly once. The thread that wins the
     * compare-and-set performs the setup; everyone else just proceeds. */
    if(atomic_compare_and_set_bool(&stderr_buffering_initialized, false, true) == false) {
        /* Set stderr to full buffering with 8KB buffer for better performance */
        setvbuf(stderr, NULL, _IOFBF, 8192);
    }
}


/*
 * Hand a finished log line to the registered callback, or to stderr if there is
 * none. Takes no spinlock: see the note on logger_callback_lock above for why
 * output must never run inside one.
 *
 * Coordinating with debug_unregister_logger() without a lock takes two steps.
 * Load the pointer, and if it is set, raise the in-flight count and load it a
 * second time. The atomics are full barriers, so by the time the second load
 * happens our increment is visible to everyone. That leaves unregister only two
 * possibilities: it cleared the pointer before our second load, and we see NULL
 * and fall back to stderr rather than calling into a callback the caller is
 * retiring; or it had not cleared it yet, in which case it must see our raised
 * count and wait for us to finish. Either way unregister cannot return while a
 * call to the callback it cleared is in flight.
 */
static void emit_log_line(int32_t tag_id, int debug_level, const char *output) {
    void (*callback)(int32_t tag_id, int debug_level, const char *message) = log_callback_func;

    if(callback) {
        atomic_add_int32(&log_callback_in_flight, 1);

        /* re-read now that our presence is published. */
        callback = log_callback_func;

        if(callback) { callback(tag_id, debug_level, output); }

        atomic_add_int32(&log_callback_in_flight, -1);

        if(callback) { return; }

        /* unregistered underneath us -- fall through and write it to stderr. */
    }

    fputs(output, stderr);

    /* Flush periodically for better performance while ensuring timely output */
    int32_t call_count = atomic_add_int32(&log_call_count, 1) + 1;
    if(debug_level <= DEBUG_ERROR || (call_count % 100) == 0) {
        /* Flush on errors (critical info) or every 100 log entries */
        fflush(stderr);
    }
}


extern void pdebug_impl(const char *func, int line_num, int debug_level, debug_module_t module, int32_t tag_id, const char *templ, ...) {
    va_list va;
    struct tm t;
    time_t epoch;
    int64_t epoch_us;
    int remainder_us;
    const char *module_name = format_module_name(module);
    char prefix[1000]; /* MAGIC */
    char output[1000];

    /* Ensure stderr buffering is set up (one-time initialization) */
    ensure_stderr_buffering();

    /* get the time parts */
    epoch_us = time_us();
    epoch = (time_t)(epoch_us / 1000000);
    remainder_us = (int)(epoch_us % 1000000);

    /* FIXME - should capture error return! */
    localtime_r(&epoch, &t);

    /* build the output string template */
    // NOLINTNEXTLINE
    snprintf(prefix, sizeof(prefix), "%04d-%02d-%02d %02d:%02d:%02d.%06d thread(%u) tag(%" PRId32 ") [%s] %s %s:%d %s\n",
             t.tm_year + 1900, t.tm_mon + 1, /* month is 0-11? */
             t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, remainder_us, get_thread_id(), tag_id, module_name,
             debug_level_name[debug_level], func, line_num, templ);

    /* make sure it is zero terminated */
    prefix[sizeof(prefix) - 1] = 0;

    /* print it out. */
    va_start(va, templ);

    /* FIXME - check the output size */
    // NOLINTNEXTLINE
    /*output_size = */ vsnprintf(output, sizeof(output), prefix, va);

    va_end(va);

    emit_log_line(tag_id, debug_level, output);
}


#define COLUMNS (16)

void pdebug_dump_bytes_impl(const char *func, int line_num, int debug_level, debug_module_t module, int32_t tag_id,
                            uint8_t *data, int count) {
    int max_row, row, column;
    char row_buf[(COLUMNS * 3) + 5 + 1];

    /* determine the number of rows we will need to print. */
    max_row = (count + (COLUMNS - 1)) / COLUMNS;

    for(row = 0; row < max_row; row++) {
        int offset = (row * COLUMNS);
        int row_offset = 0;

        /* print the offset in the packet */
        // NOLINTNEXTLINE
        row_offset = snprintf(&row_buf[0], sizeof(row_buf), "%05d", offset);

        for(column = 0; column < COLUMNS && ((row * COLUMNS) + column) < count && row_offset < (int)sizeof(row_buf); column++) {
            offset = (row * COLUMNS) + column;
            // NOLINTNEXTLINE
            row_offset += snprintf(&row_buf[row_offset], sizeof(row_buf) - (size_t)row_offset, " %02x", data[offset]);
        }

        /* terminate the row string*/
        row_buf[sizeof(row_buf) - 1] = 0; /* just in case */

        /* output it, finally */
        pdebug_impl(func, line_num, debug_level, module, tag_id, row_buf);
    }
}


int debug_register_logger(void (*log_callback_func_arg)(int32_t tag_id, int debug_level, const char *message)) {
    int rc = PLCTAG_STATUS_OK;

    spin_block(&logger_callback_lock) {
        if(!log_callback_func) {
            log_callback_func = log_callback_func_arg;
        } else {
            rc = PLCTAG_ERR_DUPLICATE;
        }
    }

    return rc;
}


int debug_unregister_logger(void) {
    int rc = PLCTAG_STATUS_OK;

    spin_block(&logger_callback_lock) {
        if(log_callback_func) {
            log_callback_func = NULL;
        } else {
            rc = PLCTAG_ERR_NOT_FOUND;
        }
    }

    if(rc == PLCTAG_STATUS_OK) {
        /*
         * The pointer is clear, so no new caller can reach the callback. Wait out
         * the ones that loaded it just before we cleared it, so this does not return
         * while a call is still running -- the caller is free to tear down whatever
         * the callback touches the moment we do. See emit_log_line() for how the two
         * sides interlock. This drains: callers arriving from here on see NULL and
         * never raise the count.
         */
        while(atomic_get_int32(&log_callback_in_flight) > 0) { sleep_ms(1); }
    }

    return rc;
}


void debug_flush(void) {
    /* Flush stderr to ensure all buffered log output is written */
    bool has_logger = false;
    spin_block(&logger_callback_lock) { has_logger = (log_callback_func != NULL); }

    if(!has_logger) { fflush(stderr); }
}


