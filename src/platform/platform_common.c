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
 * These were duplicated in src/platform/posix/platform.c and
 * src/platform/windows/platform.c.  The few standard library calls that are
 * genuinely spelled differently on the two platforms come in through the
 * platform_str* macros that each platform.h defines.
 *
 * Declared in platform.h alongside the rest, so nothing that calls them changes.
 */

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <platform.h>
#include <stdlib.h>
#include <string.h>

#include <libplctag/api/libplctag.h>
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


void mem_copy(void *dest, void *src, int size) {
    if(!dest) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Destination pointer is NULL!");
        return;
    }

    if(!src) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Source pointer is NULL!");
        return;
    }

    if(size < 0) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Size to copy must not be negative!");
        return;
    }

    if(size == 0) { return; }

    // NOLINTNEXTLINE
    memcpy(dest, src, (size_t)(unsigned int)size);
}


void mem_move(void *dest, void *src, int size) {
    if(!dest) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Destination pointer is NULL!");
        return;
    }

    if(!src) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Source pointer is NULL!");
        return;
    }

    if(size < 0) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Size to move must not be negative!");
        return;
    }

    if(size == 0) { return; }

    // NOLINTNEXTLINE
    memmove(dest, src, (size_t)(unsigned int)size);
}


/*
 * Returns -1, 0, or 1 depending on whether the first string is "less" than the
 * second, the same as the second, or "greater" than the second.  The comparison
 * is done case insensitive.
 *
 * Handle the usual edge cases.
 */
int str_cmp_i(const char *first, const char *second) {
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
            return platform_strcasecmp(first, second);
        }
    }
}


/*
 * As str_cmp_i(), but comparing only the first count characters.
 */
int str_cmp_i_n(const char *first, const char *second, int count) {
    int first_zero = !str_length(first);
    int second_zero = !str_length(second);

    if(count < 0) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Illegal negative count!");
        return -1;
    }

    if(count == 0) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Called with comparison count of zero!");
        return 0;
    }

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
            return platform_strncasecmp(first, second, (size_t)(unsigned int)count);
        }
    }
}


/*
 * Returns a pointer to the location of the needle string in the haystack string
 * or NULL if there is no match.  The comparison is done case-insensitive.
 *
 * There is no portable case-insensitive strstr() -- strcasestr() is a POSIX
 * extension that Windows does not have -- so scan with str_cmp_i_n(), which
 * already resolves to each platform's own case-insensitive compare.
 */
char *str_str_cmp_i(const char *haystack, const char *needle) {
    int haystack_length = str_length(haystack);
    int needle_length = str_length(needle);

    if(!haystack_length) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Haystack string is NULL or zero length.");
        return NULL;
    }

    if(!needle_length) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Needle string is NULL or zero length.");
        return NULL;
    }

    for(int start = 0; start + needle_length <= haystack_length; start++) {
        if(str_cmp_i_n(&haystack[start], needle, needle_length) == 0) { return (char *)&haystack[start]; }
    }

    return NULL;
}


int str_copy(char *dst, int dst_size, const char *src) {
    if(!dst) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Destination string pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!src) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Source string pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(dst_size <= 0) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Destination size is negative or zero!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    /*
     * Refuse rather than truncate.  A caller that ignored a truncation would be left holding
     * a partial string, and with strncpy() an unterminated one when the source fills the
     * destination exactly.  There is no safe partial result here, so do not produce one.
     *
     * Because the source is known to fit at this point, copying it with its terminator is
     * exact.  That also avoids strncpy(), which the Windows compilers refuse without _s.
     */
    if(str_length(src) >= dst_size) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Source string of %d bytes does not fit a destination of %d bytes!",
               str_length(src), dst_size);
        return PLCTAG_ERR_TOO_LARGE;
    }

    memcpy(dst, src, (size_t)(unsigned int)(str_length(src) + 1));

    return PLCTAG_STATUS_OK;
}


/*
 * Copy the passed string and return a pointer to the copy.
 * The caller is responsible for freeing the memory.
 */
char *str_dup(const char *str) {
    if(!str) { return NULL; }

    return platform_strdup(str);
}


/*
 * Convert the characters in the passed string into an int.  Return the value
 * through the passed pointer and a status from the function.
 */
int str_to_int(const char *str, int *val) {
    char *endptr;
    long int tmp_val;

    /*
     * strtol() only ever sets errno, it never clears it, so a stale ERANGE left
     * behind by any earlier library or system call would be read back below as
     * this conversion's own failure. Clear it first so the check means something.
     */
    errno = 0;

    tmp_val = strtol(str, &endptr, 0);

    if(errno == ERANGE && (tmp_val == LONG_MAX || tmp_val == LONG_MIN)) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "strtol returned %ld with errno %d", tmp_val, errno);
        return -1;
    }

    if(endptr == str) { return -1; }

    /*
     * long is wider than int on most 64-bit platforms, so strtol() happily returns values
     * that do not survive the cast.  Without this check "4294967296" converts to zero on
     * LP64 and the caller has no way to tell that from a real zero.  Reject instead.
     */
    if(tmp_val > (long int)INT_MAX || tmp_val < (long int)INT_MIN) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Value %ld does not fit in an int!", tmp_val);
        return -1;
    }

    *val = (int)tmp_val;

    return 0;
}


/*
 * Convert the characters in the passed string into a float.
 *
 * strtod() rather than strtof() because the Windows C runtime has no strtof().
 */
int str_to_float(const char *str, float *val) {
    char *endptr;
    double tmp_val;

    /* clear errno for the same reason as in str_to_int(). */
    errno = 0;

    tmp_val = strtod(str, &endptr);

    if(errno == ERANGE && (tmp_val == HUGE_VAL || tmp_val == -HUGE_VAL || tmp_val == 0.0)) { return -1; }

    if(endptr == str) { return -1; }

    /* FIXME - this will truncate long values. */
    *val = (float)tmp_val;

    return 0;
}


char **str_split(const char *str, const char *sep) {
    int sub_str_count = 0;
    int size = 0;
    const char *sub;
    const char *tmp;
    char **res;

    /* first, count the sub strings */
    tmp = str;
    sub = strstr(tmp, sep);

    while(sub && *sub) {
        /* separator could be at the front, ignore that. */
        if(sub != tmp) { sub_str_count++; }

        tmp = sub + str_length(sep);
        sub = strstr(tmp, sep);
    }

    if(tmp && *tmp && (!sub || !*sub)) { sub_str_count++; }

    /* calculate total size for string plus pointers */
    size = ((int)sizeof(char *) * (sub_str_count + 1) + str_length(str) + 1);

    /* allocate enough memory */
    res = mem_alloc(size);
    if(!res) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Unable to allocate memory for the split string result!");
        return NULL;
    }

    /* calculate the beginning of the string */
    tmp = (char *)res + sizeof(char *) * (size_t)(sub_str_count + 1);

    /* copy the string into the new buffer past the first part with the array of char pointers. */
    str_copy((char *)tmp, (int)(size - ((char *)tmp - (char *)res)), str);

    /* set up the pointers */
    sub_str_count = 0;
    sub = strstr(tmp, sep);

    while(sub && *sub) {
        /* separator could be at the front, ignore that. */
        if(sub != tmp) {
            /* store the pointer */
            res[sub_str_count] = (char *)tmp;

            sub_str_count++;
        }

        /* zero out the separator chars */
        mem_set((char *)sub, 0, str_length(sep));

        /* point past the separator (now zero) */
        tmp = sub + str_length(sep);

        /* find the next separator */
        sub = strstr(tmp, sep);
    }

    /* if there is a chunk at the end, store it. */
    if(tmp && *tmp && (!sub || !*sub)) { res[sub_str_count] = (char *)tmp; }

    return res;
}
