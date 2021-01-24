/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
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

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

#if defined(WIN32) || defined(WIN64) || defined(_WIN32) || defined(_WIN64)
    #define _WINSOCKAPI_
    #include <windows.h>
    #include <tchar.h>
    #include <strsafe.h>
    #include <io.h>
    #include <Winsock2.h>
    #include <Ws2tcpip.h>
#else
    /* assume it is POSIX of some sort... */
    #include <sys/time.h>
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <netdb.h>
#endif

#include "utils.h"


/*
 * This contains the utilities used by the test harness.
 */


#ifdef IS_WINDOWS

int util_sleep_ms(int ms)
{
    Sleep(ms);
    return 1;
}

#else

int util_sleep_ms(int ms)
{
    struct timeval tv;

    tv.tv_sec = ms/1000;
    tv.tv_usec = (ms % 1000)*1000;

    return select(0,NULL,NULL,NULL, &tv);
}

#endif


/*
 * time_ms
 *
 * Return the current epoch time in milliseconds.
 */

#ifdef IS_WINDOWS
int64_t util_time_ms(void)
{
    FILETIME ft;
    int64_t res;

    GetSystemTimeAsFileTime(&ft);

    /* calculate time as 100ns increments since Jan 1, 1601. */
    res = (int64_t)(ft.dwLowDateTime) + ((int64_t)(ft.dwHighDateTime) << 32);

    /* get time in ms */

    res = res / 10000;

    return  res;
}

#else


int64_t util_time_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv,NULL);

    return  ((int64_t)tv.tv_sec*1000)+ ((int64_t)tv.tv_usec/1000);
}

#endif


/*
 * string helpers
 */

int match_chars(const char* source, int start_index, const char* chars)
{
    return (int)(unsigned int)strspn(source + start_index, chars);
}


/*
 * Logging routines.
 */

static bool debug_is_on = false;


void debug_on(void)
{
    debug_is_on = true;
}

void debug_off(void)
{
    debug_is_on = false;
}



void error_impl(const char *func, int line, const char *templ, ...)
{
    va_list va;

    /* print it out. */
    fprintf(stderr, "ERROR %s:%d ", func, line);
    va_start(va,templ);
    vfprintf(stderr,templ,va);
    va_end(va);
    fprintf(stderr,"\n");

    exit(1);
}



void info_impl(const char *func, int line, const char *templ, ...)
{
    va_list va;

    if(!debug_is_on) {
        return;
    }

    /* print it out. */
    fprintf(stderr, "INFO %s:%d ", func, line);
    va_start(va,templ);
    vfprintf(stderr,templ,va);
    va_end(va);
    fprintf(stderr,"\n");
}


#define COLUMNS (size_t)(10)

void slice_dump(slice_s s)
{
    size_t max_row, row, column;
    char row_buf[300]; /* MAGIC */

    if(!debug_is_on) {
        return;
    }

    /* determine the number of rows we will need to print. */
    max_row = (slice_len(s)  + (COLUMNS - 1))/COLUMNS;

    for(row = 0; row < max_row; row++) {
        size_t offset = (row * COLUMNS);
        size_t row_offset;

        /* print the prefix and address */
        row_offset = (size_t)snprintf(&row_buf[0], sizeof(row_buf),"%04zu  ", offset);

        for(column = 0; column < COLUMNS && ((row * COLUMNS) + column) < slice_len(s) && row_offset < (int)sizeof(row_buf); column++) {
            offset = (row * COLUMNS) + column;
            row_offset += (size_t)snprintf(&row_buf[row_offset], sizeof(row_buf) - row_offset, " %02x", slice_get_uint8(s, offset));
        }

        /* zero terminate */
        if(row_offset < sizeof(row_buf)) {
            row_buf[row_offset] = (char)0;
        } else {
            /* this might truncate the string, but it is safe. */
            row_buf[sizeof(row_buf)-1] = (char)0;
        }

        /* output it, finally */
        fprintf(stderr,"%s\n", row_buf);
    }
}


