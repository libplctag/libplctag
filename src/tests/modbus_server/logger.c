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

#include "logger.h"
#include <stdio.h>
#include <time.h>
#include <stdint.h>

#ifdef _WIN32
    #include <Windows.h>
#else
    #include <sys/time.h>
#endif

static bool debug_enabled = false;

void logger_init(bool debug) {
    debug_enabled = debug;
}

void logger_set_debug(bool enabled) {
    debug_enabled = enabled;
}

bool logger_is_debug_enabled(void) {
    return debug_enabled;
}

static const char* level_to_string(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_ERROR: return "ERROR";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_DEBUG: return "DEBUG";
        default:              return "UNKNOWN";
    }
}

static void log_with_timestamp(log_level_t level, const char *fmt, va_list args) {
    time_t now;
    struct tm *timeinfo;
    char timestamp[32];
    long milliseconds;
    
    /* Skip debug messages if debug is not enabled */
    if (level == LOG_LEVEL_DEBUG && !debug_enabled) {
        return;
    }
    
    /* Get current time with milliseconds */
#ifdef _WIN32
    SYSTEMTIME st;
    GetSystemTime(&st);
    now = time(NULL);
    timeinfo = localtime(&now);
    milliseconds = st.wMilliseconds;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    now = ts.tv_sec;
    milliseconds = ts.tv_nsec / 1000000;
    timeinfo = localtime(&now);
#endif
    
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);
    
    /* Print timestamp with milliseconds and level */
    fprintf(stderr, "%s.%03ld %s ", timestamp, milliseconds, level_to_string(level));
    
    /* Print the actual message */
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    fflush(stderr);
}

void log_message(log_level_t level, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_with_timestamp(level, fmt, args);
    va_end(args);
}

void log_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_with_timestamp(LOG_LEVEL_ERROR, fmt, args);
    va_end(args);
}

void log_warn(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_with_timestamp(LOG_LEVEL_WARN, fmt, args);
    va_end(args);
}

void log_info(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_with_timestamp(LOG_LEVEL_INFO, fmt, args);
    va_end(args);
}

void log_debug(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_with_timestamp(LOG_LEVEL_DEBUG, fmt, args);
    va_end(args);
}

void log_dump_bytes(const char *prefix, const uint8_t *data, size_t len) {
    if (!debug_enabled || !data || len == 0) {
        return;
    }
    
    log_debug("%s (%zu bytes):", prefix, len);
    
    for (size_t i = 0; i < len; i += 16) {
        char hex_part[64] = {0};
        char ascii_part[20] = {0};
        size_t hex_pos = 0;
        size_t ascii_pos = 0;
        
        for (size_t j = 0; j < 16 && (i + j) < len; j++) {
            uint8_t byte = data[i + j];
            int res = snprintf(hex_part + hex_pos, sizeof(hex_part) - hex_pos, 
                              "%02X ", byte);
            if (res < 0) break;
            hex_pos += (size_t)res;
            
            ascii_part[ascii_pos++] = (byte >= 32 && byte <= 126) ? (char)byte : '.';
        }
        
        fprintf(stderr, "  %04zX: %-48s %s\n", i, hex_part, ascii_part);
    }
    fflush(stderr);
}
