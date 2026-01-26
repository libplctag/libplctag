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
 * Per-module debug levels - array indexed by bit position
 */
static volatile int module_debug_levels[64];


/*
 * Keep the thread ID and the tag ID thread local.
 */

static THREAD_LOCAL uint32_t this_thread_num = 0;
static THREAD_LOCAL int32_t tag_id = 0;


// /* only output the version once */
// static lock_t printed_version = LOCK_INIT;


int set_debug_level(int level) {
    int old_level = global_debug_level;

    global_debug_level = level;

    return old_level;
}


int get_debug_level(void) { return global_debug_level; }


void debug_set_tag_id(int32_t t_id) { tag_id = t_id; }


void debug_module_set_level(debug_module_t module, int level) {
    /* module is a pre-shifted bitmask (1 << bit), extract the bit position */
    int bit = 0;
    uint64_t shifted = (uint64_t)module;

    while(shifted > 1) {
        shifted >>= 1;
        bit++;
    }

    if(bit < 64) { module_debug_levels[bit] = level; }
}


int debug_module_get_level(debug_module_t module) {
    /* module is a pre-shifted bitmask (1 << bit), extract the bit position */
    int bit = 0;
    uint64_t shifted = (uint64_t)module;

    while(shifted > 1) {
        shifted >>= 1;
        bit++;
    }

    if(bit < 64) { return module_debug_levels[bit]; }
    return DEBUG_NONE;
}


void debug_set_all_modules(int level) {
    for(int i = 0; i < 64; i++) { module_debug_levels[i] = level; }
}


bool debug_is_enabled(debug_module_mask_t modules, int level) {
    /* If global debug level is NONE, check module levels */
    if(global_debug_level == DEBUG_NONE) {
        /* Check if any module in the mask has this level enabled */
        for(int bit = 0; bit < 64; bit++) {
            if(modules & (1ULL << bit)) {
                if(level <= module_debug_levels[bit]) { return true; }
            }
        }
        return false;
    }

    /* If global debug level is set, use it as override */
    return level <= global_debug_level;
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


static void format_module_names(debug_module_mask_t modules, char *buf, size_t buf_size) {
    bool first = true;
    buf[0] = '\0';

    for(int bit = 0; bit < 64 && buf_size > 1; bit++) {
        if(modules & (1ULL << bit)) {
            if(bit < (int)DEBUG_MODULE_COUNT && debug_module_names[bit]) {
                size_t name_len = strlen(debug_module_names[bit]);
                if(!first && buf_size > 1) {
                    strncat(buf, "|", buf_size - 1);
                    buf_size--;
                }
                strncat(buf, debug_module_names[bit], buf_size - 1);
                buf_size -= (name_len > buf_size) ? buf_size : name_len;
                first = false;
            }
        }
    }
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

extern void pdebug_impl(const char *func, int line_num, int debug_level, debug_module_mask_t modules, const char *templ, ...) {
    va_list va;
    struct tm t;
    time_t epoch;
    int64_t epoch_us;
    int remainder_us;
    char module_buf[256];
    char prefix[1000]; /* MAGIC */
    char output[1000];

    /* Ensure stderr buffering is set up (one-time initialization) */
    ensure_stderr_buffering();

    /* format the module names */
    format_module_names(modules, module_buf, sizeof(module_buf));

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
             t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, remainder_us, get_thread_id(), tag_id, module_buf,
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

void pdebug_dump_bytes_impl(const char *func, int line_num, int debug_level, debug_module_mask_t modules, uint8_t *data,
                            int count) {
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
        pdebug_impl(func, line_num, debug_level, modules, row_buf);
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


/*
 * Convert a module name string to a module ID.
 * Returns the module ID (pre-shifted bitmask) or 0 if not found.
 */
debug_module_t debug_module_id(const char *module_name) {
    if(!module_name) { return 0; }

/* Map module name strings to enum values */
#define MODULE_CASE(name) \
    if(str_cmp_i(module_name, #name) == 0) { return DEBUG_MODULE_##name; }

    MODULE_CASE(LIB)
    MODULE_CASE(INIT)
    MODULE_CASE(VERSION)
    MODULE_CASE(UTILS)
    MODULE_CASE(AB_SESSION)
    MODULE_CASE(AB_PCCC)
    MODULE_CASE(AB_CIP)
    MODULE_CASE(AB_COMMON)
    MODULE_CASE(AB_EIP_CIP)
    MODULE_CASE(AB_EIP_CIP_SPECIAL)
    MODULE_CASE(AB_EIP_LGX_PCCC)
    MODULE_CASE(AB_EIP_PLC5_PCCC)
    MODULE_CASE(AB_EIP_PLC5_DHP)
    MODULE_CASE(AB_EIP_SLC_PCCC)
    MODULE_CASE(AB_EIP_SLC_DHP)
    MODULE_CASE(AB_ERROR)
    MODULE_CASE(OMRON_CONN)
    MODULE_CASE(OMRON_CIP)
    MODULE_CASE(OMRON_COMMON)
    MODULE_CASE(OMRON_STANDARD_TAG)
    MODULE_CASE(OMRON_RAW_TAG)
    MODULE_CASE(MODBUS)
    MODULE_CASE(SYSTEM)

#undef MODULE_CASE

    return 0;
}


/*
 * Convert a debug level name string to a debug level ID.
 * Returns the debug level ID or -1 if not found.
 */
int debug_level_id(const char *level_name) {
    if(!level_name) { return -1; }

    if(str_cmp_i(level_name, "NONE") == 0 || str_cmp_i(level_name, "DEBUG_NONE") == 0) { return DEBUG_NONE; }
    if(str_cmp_i(level_name, "ERROR") == 0 || str_cmp_i(level_name, "DEBUG_ERROR") == 0) { return DEBUG_ERROR; }
    if(str_cmp_i(level_name, "WARN") == 0 || str_cmp_i(level_name, "DEBUG_WARN") == 0) { return DEBUG_WARN; }
    if(str_cmp_i(level_name, "INFO") == 0 || str_cmp_i(level_name, "DEBUG_INFO") == 0) { return DEBUG_INFO; }
    if(str_cmp_i(level_name, "DETAIL") == 0 || str_cmp_i(level_name, "DEBUG_DETAIL") == 0) { return DEBUG_DETAIL; }
    if(str_cmp_i(level_name, "SPEW") == 0 || str_cmp_i(level_name, "DEBUG_SPEW") == 0) { return DEBUG_SPEW; }

    return -1;
}
