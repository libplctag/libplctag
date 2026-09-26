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

/*
 * Platform functions with no platform-specific content.
 *
 * These were duplicated verbatim in src/platform/posix/platform.c and
 * src/platform/windows/platform.c.  Everything still in those files has a real
 * reason to differ -- a different system call, or a function one platform lacks
 * (strtof, strcasestr) -- so only the genuinely portable bodies live here.
 *
 * Declared in platform.h alongside the rest, so nothing that calls them changes.
 */

#include <platform.h>
#include <stdlib.h>
#include <string.h>
#include <utils/debug.h>


void *mem_alloc(int size) {
    if(size <= 0) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Allocation size must be greater than zero bytes!");
        return NULL;
    }

    return calloc((size_t)(unsigned int)size, 1);
}


void mem_free(const void *mem) {
    if(mem) { free((void *)mem); }
}


void *mem_realloc(void *orig, int size) {
    if(size <= 0) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "New allocation size must be greater than zero bytes!");
        return NULL;
    }

    return realloc(orig, (size_t)(ssize_t)size);
}


void mem_set(void *dest, int c, int size) {
    if(!dest) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Destination pointer is NULL!");
        return;
    }

    if(size <= 0) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Size to set must be a positive number!");
        return;
    }

    // NOLINTNEXTLINE
    memset(dest, c, (size_t)(ssize_t)size);
}


int mem_cmp(void *src1, int src1_size, void *src2, int src2_size) {
    if(!src1 || src1_size <= 0) {
        if(!src2 || src2_size <= 0) {
            /* both are NULL or zero length, but that is "equal" for our purposes. */
            return 0;
        } else {
            /* first one is "less" than second. */
            return -1;
        }
    } else {
        if(!src2 || src2_size <= 0) {
            /* first is "greater" than second */
            return 1;
        } else {
            /* both pointers are non-NULL and the lengths are positive. */

            /* short circuit the comparison if the blocks are different lengths */
            if(src1_size != src2_size) { return (src1_size - src2_size); }

            return memcmp(src1, src2, (size_t)(unsigned int)src1_size);
        }
    }
}


int str_length(const char *str) {
    if(!str) { return 0; }

    return (int)strlen(str);
}


int str_cmp(const char *first, const char *second) {
    int first_zero = !str_length(first);
    int second_zero = !str_length(second);

    if(first_zero) {
        if(second_zero) {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "NULL or zero length strings passed.");
            return 0;
        } else {
            /* first is "less" than second. */
            return -1;
        }
    } else {
        if(second_zero) {
            /* first is "more" than second. */
            return 1;
        } else {
            /* both are non-zero length. */
            return strcmp(first, second);
        }
    }
}


char *str_concat_impl(int num_args, ...) {
    va_list arg_list;
    int total_length = 0;
    char *result = NULL;
    char *tmp = NULL;

    /* first loop to find the length */
    va_start(arg_list, num_args);
    for(int i = 0; i < num_args; i++) {
        tmp = va_arg(arg_list, char *);
        if(tmp) { total_length += str_length(tmp); }
    }
    va_end(arg_list);

    /* make a buffer big enough */
    total_length += 1;

    result = mem_alloc(total_length);
    if(!result) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_ERROR, 0, "Unable to allocate new string buffer!");
        return NULL;
    }

    /* loop to copy the strings */
    result[0] = 0;
    va_start(arg_list, num_args);
    for(int i = 0; i < num_args; i++) {
        tmp = va_arg(arg_list, char *);
        if(tmp) {
            int len = str_length(result);
            str_copy(&result[len], total_length - len, tmp);
        }
    }
    va_end(arg_list);

    return result;
}
