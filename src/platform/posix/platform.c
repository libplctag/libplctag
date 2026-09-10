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

#ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <platform.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <libplctag/lib/libplctag.h>
#include <utils/debug.h>


#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__bsdi__) \
    || defined(__DragonFly__)
#    define BSD_OS_TYPE
#    if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#        define _DARWIN_C_SOURCE _POSIX_C_SOURCE
#    endif
#endif


/***************************************************************************
 ******************************* Strings ***********************************
 **************************************************************************/


/*
 * str_cmp
 *
 * Return -1, 0, or 1 depending on whether the first string is "less" than the
 * second, the same as the second, or "greater" than the second.  This routine
 * just passes through to POSIX strcmp.
 *
 * Handle edge cases when NULL or zero length strings are passed.
 */
extern int str_cmp(const char *first, const char *second) {
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


/*
 * str_cmp_i
 *
 * Returns -1, 0, or 1 depending on whether the first string is "less" than the
 * second, the same as the second, or "greater" than the second.  The comparison
 * is done case insensitive.
 *
 * Handle the usual edge cases.
 */
extern int str_cmp_i(const char *first, const char *second) {
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
            return strcasecmp(first, second);
        }
    }
}


/*
 * str_cmp_i_n
 *
 * Returns -1, 0, or 1 depending on whether the first string is "less" than the
 * second, the same as the second, or "greater" than the second.  The comparison
 * is done case insensitive.  Compares only the first count characters.
 *
 * It just passes this through to POSIX strncasecmp.
 */
extern int str_cmp_i_n(const char *first, const char *second, int count) {
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
            return strncasecmp(first, second, (size_t)(unsigned int)count);
        }
    }
    return strncasecmp(first, second, (size_t)(unsigned int)count);
}


/*
 * str_str_cmp_i
 *
 * Returns a pointer to the location of the needle string in the haystack string
 * or NULL if there is no match.  The comparison is done case-insensitive.
 *
 * Handle the usual edge cases.
 */
extern char *str_str_cmp_i(const char *haystack, const char *needle) {
    int haystack_zero = !str_length(haystack);
    int needle_zero = !str_length(needle);

    if(haystack_zero) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Haystack string is NULL or zero length.");
        return NULL;
    }

    if(needle_zero) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Needle string is NULL or zero length.");
        return NULL;
    }

    return strcasestr(haystack, needle);
}


/*
 * str_copy
 *
 * Returns
 */
extern int str_copy(char *dst, int dst_size, const char *src) {
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
     * Refuse rather than truncate.  strncpy() writes no terminator when the source fills the
     * destination exactly, so a caller that ignored a truncation would be left holding an
     * unterminated buffer -- every later str_length() or print of it runs off the end.  There
     * is no safe partial result here, so do not produce one.
     */
    if(str_length(src) >= dst_size) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Source string of %d bytes does not fit a destination of %d bytes!",
               str_length(src), dst_size);
        return PLCTAG_ERR_TOO_LARGE;
    }

    // NOLINTNEXTLINE
    strncpy(dst, src, (size_t)(unsigned int)dst_size);

    return PLCTAG_STATUS_OK;
}


/*
 * str_length
 *
 * Return the length of the string.  If a null pointer is passed, return
 * null.
 */
extern int str_length(const char *str) {
    if(!str) { return 0; }

    return (int)strlen(str);
}


/*
 * str_dup
 *
 * Copy the passed string and return a pointer to the copy.
 * The caller is responsible for freeing the memory.
 */
extern char *str_dup(const char *str) {
    if(!str) { return NULL; }

    return strdup(str);
}


/*
 * str_to_int
 *
 * Convert the characters in the passed string into
 * an int.  Return an int in integer in the passed
 * pointer and a status from the function.
 */
extern int str_to_int(const char *str, int *val) {
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


extern int str_to_float(const char *str, float *val) {
    char *endptr;
    float tmp_val;

    /*
     * See str_to_int() above. This one matters more: the ERANGE test also covers
     * underflow-to-zero, so a stale ERANGE would reject a plain "0" as an error.
     */
    errno = 0;

    tmp_val = strtof(str, &endptr);

    if(errno == ERANGE && (tmp_val == HUGE_VALF || tmp_val == -HUGE_VALF || tmp_val == 0)) { return -1; }

    if(endptr == str) { return -1; }

    /* FIXME - this will truncate long values. */
    *val = tmp_val;

    return 0;
}


extern char **str_split(const char *str, const char *sep) {
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

    if(!res) { return NULL; }

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


/***************************************************************************
 ***************************** Miscellaneous *******************************
 **************************************************************************/


/*
 * sleep_ms
 *
 * Sleep the passed number of milliseconds.  This handles the case of being
 * interrupted by a signal.
 *
 * TODO - should the signal interrupt handling be done here or in app code?
 */
int sleep_ms(int ms) {
    struct timespec wait_time;
    struct timespec remainder;
    int done = 1;
    int rc = PLCTAG_STATUS_OK;

    if(ms < 0) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "called with negative time %d!", ms);
        return PLCTAG_ERR_BAD_PARAM;
    }

    wait_time.tv_sec = ms / 1000;
    wait_time.tv_nsec = ((long)ms % 1000) * 1000000; /* convert to nanoseconds */

    do {
        rc = nanosleep(&wait_time, &remainder);
        if(rc < 0 && errno == EINTR) {
            /* we were interrupted, keep going. */
            wait_time = remainder;
            done = 0;
        } else {
            done = 1;

            if(rc < 0) {
                /* error condition. */
                rc = PLCTAG_ERR_BAD_REPLY;
            }
        }
    } while(!done);

    return rc;


    // rc = nanosleep(&wait_time, &remainder);
    // if(rc < 0 && errno != EINTR) {
    //     rc = PLCTAG_ERR_BAD_REPLY;
    // }

    // return rc;
    // struct timeval tv;

    // tv.tv_sec = ms/1000;
    // tv.tv_usec = (ms % 1000)*1000;

    // return select(0,NULL,NULL,NULL, &tv);
}


/*
 * time_ms
 *
 * Return the current epoch time in milliseconds.
 */
int64_t time_ms(void) {
    struct timeval tv;

    gettimeofday(&tv, NULL);

    return ((int64_t)tv.tv_sec * 1000) + ((int64_t)tv.tv_usec / 1000);
}
