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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <utils/debug.h>

#if defined(_WIN32) || defined(_WIN64)
#    include <windows.h>
#else
#    include <sys/time.h>
#endif


/*
 * Debugging support.
 */


static volatile int global_debug_level = DEBUG_NONE;
static lock_t thread_num_lock = LOCK_INIT;
static volatile uint32_t thread_num = 1;
static lock_t logger_callback_lock = LOCK_INIT;
static void (*volatile log_callback_func)(int32_t tag_id, int debug_level, const char *message);

/* Buffering control for stderr logging performance */
static volatile int stderr_buffering_initialized = 0;
static lock_t stderr_init_lock = LOCK_INIT;
static volatile int log_call_count = 0;


/* Module name lookup table is now defined in debug_generated.h */

/*
 * Per-module debug levels - indexed directly by debug_module_t value.
 * Exposed as extern so debug_is_enabled() can be inlined in debug.h.
 */
volatile uint8_t debug_module_levels[DEBUG_MODULE_COUNT];


static THREAD_LOCAL uint32_t this_thread_num = 0;


// /* only output the version once */
// static lock_t printed_version = LOCK_INIT;


int set_debug_level(int level) {
    int old_level = global_debug_level;

    global_debug_level = level;

    /* Push the global level into every module slot so the inline
     * debug_is_enabled() check needs only a single array lookup. */
    for(int i = 0; i < DEBUG_MODULE_COUNT; i++) { debug_module_levels[i] = (uint8_t)level; }

    return old_level;
}


int get_debug_level(void) { return global_debug_level; }



void debug_module_set_level(debug_module_t module, int level) {
    if((unsigned)module < DEBUG_MODULE_COUNT) { debug_module_levels[module] = (uint8_t)level; }
}


int debug_module_get_level(debug_module_t module) {
    if((unsigned)module < DEBUG_MODULE_COUNT) { return debug_module_levels[module]; }
    return DEBUG_NONE;
}


void debug_set_all_modules(int level) {
    for(int i = 0; i < DEBUG_MODULE_COUNT; i++) { debug_module_levels[i] = (uint8_t)level; }
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
    /* Initialize stderr buffering once for better performance */
    if(!stderr_buffering_initialized) {
        spin_block(&stderr_init_lock) {
            if(!stderr_buffering_initialized) {
                /* Set stderr to full buffering with 8KB buffer for better performance */
                setvbuf(stderr, NULL, _IOFBF, 8192);
                stderr_buffering_initialized = 1;
            }
        }
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
    if(log_callback_func) {
        log_callback_func(tag_id, debug_level, output);
    } else {
        fputs(output, stderr);

        /* Flush periodically for better performance while ensuring timely output */
        log_call_count++;
        if(debug_level <= DEBUG_ERROR || (log_call_count % 100) == 0) {
            /* Flush on errors (critical info) or every 100 log entries */
            fflush(stderr);
        }
    }

    va_end(va);
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

    /* FIXME - make this a mutex */
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

    return rc;
}


void debug_flush(void) {
    /* Flush stderr to ensure all buffered log output is written */
    if(!log_callback_func) { fflush(stderr); }
}


