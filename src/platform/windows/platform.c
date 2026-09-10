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

/***************************************************************************
 ******************************* WINDOWS ***********************************
 **************************************************************************/

#include <platform.h>

/* KEEP THE SPACES BETWEEN THE INCLUDES.  The order is required! */
#include <winsock2.h>

#include <windows.h>

#include <ws2tcpip.h>

#include <errno.h>
#include <io.h>
#include <limits.h>
#include <math.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strsafe.h>
#include <tchar.h>
#include <time.h>
#include <timeapi.h>

#include <libplctag/lib/libplctag.h>
#include <utils/debug.h>


/*#ifdef __cplusplus
extern "C"
{
#endif
*/

/*#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>*/


#define WINDOWS_REQUESTED_TIMER_PERIOD_MS ((unsigned int)4)


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
 * We must handle some edge cases here due to wrappers.   We could get a NULL
 * pointer or a zero-length string for either argument.
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
 * Handle the usual edge cases because Microsoft appears not to.
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
            return _stricmp(first, second);
        }
    }
}


/*
 * str_cmp_i_n
 *
 * Returns -1, 0, or 1 depending on whether the first string is "less" than the
 * second, the same as the second, or "greater" than the second.  The comparison
 * is done case insensitive.   Compares only the first count characters.
 *
 * It just passes this through to Windows stricmp.
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
            return _strnicmp(first, second, (size_t)(unsigned int)count);
        }
    }
}


/*
 * str_str_cmp_i
 *
 * Returns a pointer to the location of the needle string in the haystack string
 * or NULL if there is no match.  The comparison is done case-insensitive.
 *
 * Handle the usual edge cases.
 */


/*
 * grabbed from Apple open source, 2021/09/06, KRH.  Note public domain license.
 *
 * Modified to rename a few things and to add some checks and debugging output.
 */

/* +++Date last modified: 05-Jul-1997 */
/* $Id: stristr.c,v 1.5 2005/03/05 00:37:19 dasenbro Exp $ */

/*
** Designation:  StriStr
**
** Call syntax:  char *stristr(char *String, char *Pattern)
**
** Description:  This function is an ANSI version of strstr() with
**               case insensitivity.
**
** Return item:  char *pointer if Pattern is found in String, else
**               pointer to 0
**
** Rev History:  07/04/95  Bob Stout  ANSI-fy
**               02/03/94  Fred Cole  Original
**
** Hereby donated to public domain.
**
** Modified for use with libcyrus by Ken Murchison 06/01/00.
*/


char *str_str_cmp_i(const char *haystack, const char *needle) {
    char *nptr, *hptr, *start;
    int haystack_len = str_length(haystack);
    int needle_len = str_length(needle);

    if(!haystack_len) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Haystack string is NULL or zero length.");
        return NULL;
    }

    if(!needle_len) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Needle string is NULL or zero length.");
        return NULL;
    }

    if(haystack_len < needle_len) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Needle string is longer than haystack string.");
        return NULL;
    }

    /* while haystack length not shorter than needle length */
    for(start = (char *)haystack, nptr = (char *)needle; haystack_len >= needle_len; start++, haystack_len--) {
        /* find start of needle in haystack */
        while(toupper(*start) != toupper(*needle)) {
            start++;
            haystack_len--;

            /* if needle longer than haystack */

            if(haystack_len < needle_len) { return (NULL); }
        }

        hptr = start;
        nptr = (char *)needle;

        while(toupper(*hptr) == toupper(*nptr)) {
            hptr++;
            nptr++;

            /* if end of needle then needle was found */
            if('\0' == *nptr) { return (start); }
        }
    }

    return (NULL);
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
     * Refuse rather than truncate.  A caller that ignored a truncation would act on a partial
     * string, so there is no safe partial result to produce here.  Keeping this identical to
     * the POSIX version also keeps the contract the same on both platforms.
     */
    if(str_length(src) >= dst_size) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Source string of %d bytes does not fit a destination of %d bytes!",
               str_length(src), dst_size);
        return PLCTAG_ERR_TOO_LARGE;
    }

    strncpy_s(dst, (rsize_t)(unsigned int)dst_size, src, _TRUNCATE);

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

    return _strdup(str);
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
        /*pdebug("strtol returned %ld with errno %d",tmp_val, errno);*/
        return -1;
    }

    if(endptr == str) { return -1; }

    /*
     * long is wider than int on some platforms, so strtol() can return values that do not
     * survive the cast.  Reject those rather than handing the caller a truncated value it
     * cannot distinguish from a real one.
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
    double tmp_val_d;
    float tmp_val;

    /*
     * See str_to_int() above. This one matters more: the ERANGE test also covers
     * underflow-to-zero, so a stale ERANGE would reject a plain "0" as an error.
     */
    errno = 0;

    /* Windows does not have strtof() */
    tmp_val_d = strtod(str, &endptr);

    if(errno == ERANGE && (tmp_val_d == HUGE_VAL || tmp_val_d == -HUGE_VAL || tmp_val_d == (double)0.0)) { return -1; }

    if(endptr == str) { return -1; }

    /* FIXME - this will truncate long values. */
    tmp_val = (float)tmp_val_d;
    *val = tmp_val;

    return 0;
}


extern char **str_split(const char *str, const char *sep) {
    size_t sub_str_count = 0;
    size_t size = 0;
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
    size = (sizeof(char *) * (sub_str_count + 1)) + (size_t)(unsigned int)str_length(str) + 1;

    /* allocate enough memory */
    res = (char **)mem_alloc((int)(unsigned int)size);
    if(!res) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Unable to allocate memory for split string result!");
        return NULL;
    }

    /* calculate the beginning of the string */
    tmp = (char *)res + sizeof(char *) * (sub_str_count + 1);

    /* copy the string into the new buffer past the first part with the array of char pointers. */
    str_copy((char *)tmp, (int)(ptrdiff_t)(size - ((char *)tmp - (char *)res)), str);

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


int sleep_ms(int ms) {
    Sleep((DWORD)ms);
    return 1;
}


/*
 * time_ms
 *
 * Return current system time in millisecond units.  This is NOT an
 * Unix epoch time.  Windows uses a different epoch starting 1/1/1601.
 */

int64_t time_ms(void) {
    FILETIME ft;
    int64_t res;

    GetSystemTimeAsFileTime(&ft);

    /* calculate time as 100ns increments since Jan 1, 1601. */
    res = (int64_t)(ft.dwLowDateTime) + ((int64_t)(ft.dwHighDateTime) << 32);

    /* get time in ms.   Magic offset is for Jan 1, 1970 Unix epoch baseline. */
    res = (res - 116444736000000000) / 10000;

    return res;
}


struct tm *localtime_r(const time_t *timep, struct tm *result) {
    time_t t = *timep;

    localtime_s(result, &t);

    return result;
}


/*#ifdef __cplusplus
}
#endif
*/
