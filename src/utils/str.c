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
 * String helpers.  See utils/str.h.
 *
 * The two platform shims carried nearly the same code here.  All the argument
 * checking, the NULL/empty ordering and the logging are shared and live in the
 * public functions below; the six places where the platforms genuinely differ
 * are the static wrappers at the bottom of this file, under one #ifdef.
 *
 * The (size_t)(uint32_t) casts are for -Wconversion, which is on in every
 * build.  Each sits behind a check that has already proven the count positive.
 */

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <libplctag/lib/libplctag.h>
#include <utils/debug.h>
#include <utils/mem.h>
#include <utils/str.h>


/*
 * The platform seam.  Six primitives, defined at the bottom of this file.
 */
static int platform_casecmp(const char *first, const char *second);
static int platform_ncasecmp(const char *first, const char *second, size_t count);
static char *platform_casestr(const char *haystack, const char *needle);
static void platform_strncpy(char *dst, int dst_size, const char *src);
static char *platform_strdup(const char *str);
static float platform_strtof(const char *str, char **endptr);


/*
 * str_cmp
 *
 * Return -1, 0, or 1 depending on whether the first string is "less" than the
 * second, the same as the second, or "greater" than the second.  This routine
 * just passes through to the C library's strcmp().
 *
 * Handle edge cases when NULL or zero length strings are passed.
 */
extern int str_cmp(const char *first, const char *second) {
    int first_zero = !str_length(first);
    int second_zero = !str_length(second);

    if(first_zero) {
        if(second_zero) {
            pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "NULL or zero length strings passed.");
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
            pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "NULL or zero length strings passed.");
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
            return platform_casecmp(first, second);
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
 * It just passes this through to the platform's case-insensitive compare.
 */
extern int str_cmp_i_n(const char *first, const char *second, int count) {
    int first_zero = !str_length(first);
    int second_zero = !str_length(second);

    if(count < 0) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Illegal negative count!");
        return -1;
    }

    if(count == 0) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Called with comparison count of zero!");
        return 0;
    }

    if(first_zero) {
        if(second_zero) {
            pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "NULL or zero length strings passed.");
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
            return platform_ncasecmp(first, second, (size_t)(uint32_t)count);
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
extern char *str_str_cmp_i(const char *haystack, const char *needle) {
    int haystack_zero = !str_length(haystack);
    int needle_zero = !str_length(needle);

    if(haystack_zero) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Haystack string is NULL or zero length.");
        return NULL;
    }

    if(needle_zero) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Needle string is NULL or zero length.");
        return NULL;
    }

    return platform_casestr(haystack, needle);
}


/*
 * str_copy
 *
 * Returns
 */
extern int str_copy(char *dst, int dst_size, const char *src) {
    if(!dst) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Destination string pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!src) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Source string pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(dst_size <= 0) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Destination size is negative or zero!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    /*
     * Refuse rather than truncate.  POSIX strncpy() writes no terminator when the source
     * fills the destination exactly, so a caller that ignored a truncation would be left
     * holding an unterminated buffer -- every later str_length() or print of it runs off the
     * end.  There is no safe partial result here, so do not produce one.  The check is done
     * here rather than in the platform wrapper so that both platforms refuse identically,
     * whatever their copy primitive would have done on its own.
     */
    if(str_length(src) >= dst_size) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Source string of %d bytes does not fit a destination of %d bytes!",
               str_length(src), dst_size);
        return PLCTAG_ERR_TOO_LARGE;
    }

    platform_strncpy(dst, dst_size, src);

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

    return platform_strdup(str);
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
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "strtol returned %" PRId64 " with errno %d", (int64_t)tmp_val, errno);
        return -1;
    }

    if(endptr == str) { return -1; }

    /*
     * long is wider than int on most 64-bit platforms, so strtol() happily returns values
     * that do not survive the cast.  Without this check "4294967296" converts to zero on
     * LP64 and the caller has no way to tell that from a real zero.  Reject instead.
     */
    if(tmp_val > (long int)INT_MAX || tmp_val < (long int)INT_MIN) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Value %" PRId64 " does not fit in an int!", (int64_t)tmp_val);
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

    tmp_val = platform_strtof(str, &endptr);

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

    if(!res) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Unable to allocate memory for split string result!");
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
        pdebug(DEBUG_MODULE_UTILS, DEBUG_ERROR, 0, "Unable to allocate new string buffer!");
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
 ************************** Platform Wrappers ******************************
 **************************************************************************/

#ifdef _WIN32

static int platform_casecmp(const char *first, const char *second) { return _stricmp(first, second); }


static int platform_ncasecmp(const char *first, const char *second, size_t count) {
    return _strnicmp(first, second, count);
}


/*
 * Windows has no strcasestr().
 *
 * Grabbed from Apple open source, 2021/09/06, KRH.  Note public domain license.
 * Modified to rename a few things and to add some checks and debugging output.
 *
 * +++Date last modified: 05-Jul-1997
 * $Id: stristr.c,v 1.5 2005/03/05 00:37:19 dasenbro Exp $
 *
 * ** Designation:  StriStr
 * **
 * ** Call syntax:  char *stristr(char *String, char *Pattern)
 * **
 * ** Description:  This function is an ANSI version of strstr() with
 * **               case insensitivity.
 * **
 * ** Return item:  char *pointer if Pattern is found in String, else
 * **               pointer to 0
 * **
 * ** Rev History:  07/04/95  Bob Stout  ANSI-fy
 * **               02/03/94  Fred Cole  Original
 * **
 * ** Hereby donated to public domain.
 * **
 * ** Modified for use with libcyrus by Ken Murchison 06/01/00.
 *
 * The caller has already rejected empty haystacks and needles.
 */
static char *platform_casestr(const char *haystack, const char *needle) {
    char *nptr, *hptr, *start;
    int haystack_len = str_length(haystack);
    int needle_len = str_length(needle);

    if(haystack_len < needle_len) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Needle string is longer than haystack string.");
        return NULL;
    }

    /* while haystack length not shorter than needle length */
    for(start = (char *)haystack, nptr = (char *)needle; haystack_len >= needle_len; start++, haystack_len--) {
        /* find start of needle in haystack */
        while(toupper(*start) != toupper(*needle)) {
            start++;
            haystack_len--;

            /* if needle longer than haystack */
            if(haystack_len < needle_len) { return NULL; }
        }

        hptr = start;
        nptr = (char *)needle;

        while(toupper(*hptr) == toupper(*nptr)) {
            hptr++;
            nptr++;

            /* if end of needle then needle was found */
            if('\0' == *nptr) { return start; }
        }
    }

    return NULL;
}


/*
 * The caller has already proven the source fits, so _TRUNCATE never triggers.
 */
static void platform_strncpy(char *dst, int dst_size, const char *src) {
    strncpy_s(dst, (rsize_t)(uint32_t)dst_size, src, _TRUNCATE);
}


static char *platform_strdup(const char *str) { return _strdup(str); }


/* Windows has no strtof(). */
static float platform_strtof(const char *str, char **endptr) { return (float)strtod(str, endptr); }

#else

static int platform_casecmp(const char *first, const char *second) { return strcasecmp(first, second); }


static int platform_ncasecmp(const char *first, const char *second, size_t count) {
    return strncasecmp(first, second, count);
}


static char *platform_casestr(const char *haystack, const char *needle) { return strcasestr(haystack, needle); }


static void platform_strncpy(char *dst, int dst_size, const char *src) {
    // NOLINTNEXTLINE
    strncpy(dst, src, (size_t)(uint32_t)dst_size);
}


static char *platform_strdup(const char *str) { return strdup(str); }


static float platform_strtof(const char *str, char **endptr) { return strtof(str, endptr); }

#endif
