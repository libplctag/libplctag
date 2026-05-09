/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever    *
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

/*
 * Copied from modbus_server/log.c.  buf_t dependency removed:
 *   - #include "buf.h" removed
 *   - log_bytes_impl() function removed
 */

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "log.h"
#include "utils.h"
#if defined(_WIN32) && !defined(_MSC_VER)
#    include <windows.h>
#endif

/* Guard for MSVC which doesn't support C11 stdatomic.h */
#if defined(_MSC_VER)
#    include <windows.h>
#    define _Atomic volatile
#    define atomic_fetch_add(obj, arg) InterlockedExchangeAdd((volatile LONG *)(obj), arg)
#    define LOCK_INIT false
#else
#    include <stdatomic.h>
#    define LOCK_INIT false
#endif


/* Module name lookup table - generated from log_modules.def */
static const char *log_module_names[] = {
#define LOG_MODULE_ENTRY(name, bit) [bit] = #name,
#include "log_modules.def"
#undef LOG_MODULE_ENTRY
};

#define LOG_MODULE_COUNT (sizeof(log_module_names) / sizeof(log_module_names[0]))

/* Per-module log levels */
static _Atomic log_level_t module_log_levels[64];

static _Atomic int thread_num_lock = 0;
static _Atomic uint32_t thread_num = 1;

#if defined(_MSC_VER) && (_MSC_VER < 1900)
#    define THREAD_LOCAL __declspec(thread)
#else
#    define THREAD_LOCAL _Thread_local
#endif

static THREAD_LOCAL uint32_t this_thread_num = 0;


static inline int log_module_to_index(log_module_t module) {
    if (module == 0) { return -1; }

    int pos = 0;
    uint64_t m = module;
    while ((m & 1) == 0) {
        m >>= 1;
        pos++;
        if (pos >= 64) { return -1; }
    }
    return pos;
}


void log_module_set_level(log_module_t module, log_level_t level) {
    int idx = log_module_to_index(module);
    if (idx >= 0 && idx < 64) { module_log_levels[idx] = level; }
}


log_level_t log_module_get_level(log_module_t module) {
    int idx = log_module_to_index(module);
    if (idx >= 0 && idx < 64) { return module_log_levels[idx]; }
    return LOG_LEVEL_NONE;
}


void log_set_all_modules(log_level_t level) {
    for (int i = 0; i < 64; i++) { module_log_levels[i] = level; }
}


bool log_is_enabled(log_module_mask_t modules, log_level_t level) {
    for (int i = 0; i < 64; i++) {
        if (modules & (1ULL << i)) {
            if (level <= module_log_levels[i]) { return true; }
        }
    }
    return false;
}


static uint32_t get_thread_id(void) {
    if (!this_thread_num) {
        int expected = 0;
        int desired  = 1;

#if defined(_MSC_VER)
        while (InterlockedCompareExchange((volatile LONG *)&thread_num_lock, desired, expected) != expected) {
            /* spin */
        }
#else
        while (!atomic_compare_exchange_strong(&thread_num_lock, &expected, desired)) {
            expected = 0;
        }
#endif

        if (!this_thread_num) {
            this_thread_num = atomic_fetch_add(&thread_num, 1);
        }

#if defined(_MSC_VER)
        InterlockedExchange((volatile LONG *)&thread_num_lock, 0);
#else
        atomic_store(&thread_num_lock, 0);
#endif
    }

    return this_thread_num;
}


static const char *log_level_name[LOG_LEVEL_END] = {"NONE", "ERROR", "WARN", "INFO", "DETAIL", "SPEW"};

static void format_module_names(log_module_mask_t modules, char *buf, size_t buf_size) {
    if (buf_size == 0) { return; }

    buf[0] = '\0';
    buf[buf_size - 1] = '\0';
    size_t offset = 0;
    bool first = true;

    for (int i = 0; i < (int)LOG_MODULE_COUNT && i < 64; i++) {
        if (modules & (1ULL << i)) {
            if (!first && offset < buf_size - 1) {
                buf[offset++] = '|';
                buf[offset]   = '\0';
            }

            const char *name = log_module_names[i];
            size_t remaining = buf_size - offset;
            if (name && remaining > 0) {
                size_t name_len = strlen(name);
                if (name_len < remaining) {
                    strcpy(&buf[offset], name);
                    offset += name_len;
                } else {
                    strncpy(&buf[offset], name, remaining - 1);
                    buf[buf_size - 1] = '\0';
                    return;
                }
            }
            first = false;
        }
    }
}


extern void log_impl(const char *func, int line_num, log_level_t debug_level,
                     log_module_mask_t modules, const char *templ, ...) {
    va_list va;
    char prefix[1000];
    char output[1000];

    int64_t epoch_us    = util_time_us();
    int     remainder_us = (int)(epoch_us % 1000000);
    time_t  epoch       = (time_t)(epoch_us / 1000000);
    struct tm t;

#if defined(_WIN32)
    localtime_s(&t, &epoch);
#else
    (void)localtime_r(&epoch, &t);
#endif

    char module_str[128];
    format_module_names(modules, module_str, sizeof(module_str));

    // NOLINTNEXTLINE
    snprintf(prefix, sizeof(prefix),
             "%04d-%02d-%02d %02d:%02d:%02d.%06d thread(%u) [%s] %s %s:%d %s\n",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec, remainder_us,
             get_thread_id(), module_str,
             log_level_name[debug_level], func, line_num, templ);
    prefix[sizeof(prefix) - 1] = 0;

    va_start(va, templ);
    // NOLINTNEXTLINE
    (void)vsnprintf(output, sizeof(output), prefix, va);
    fputs(output, stderr);
    fflush(stderr);
    va_end(va);
}
