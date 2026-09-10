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
