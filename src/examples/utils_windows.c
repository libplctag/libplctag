/***************************************************************************
 *   Copyright (C) 2016 by OmanTek                                         *
 *   Author Kyle Hayes  kylehayes@omantek.com                              *
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



#include "utils.h"

#define _WINSOCKAPI_
#include <windows.h>
#include <Winsock2.h>
#include <Ws2tcpip.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>

/*
 * This file contains useful utilities for the sample programs.
 */


/*
 * sleep_ms
 *
 * Sleep the passed number of milliseconds.
 */

int sleep_ms(int ms)
{
    Sleep(ms);
    return 1;
}



/*
 * time_ms
 *
 * Return current system time in millisecond units.  This is NOT an
 * Unix epoch time.  Windows uses a different epoch starting 1/1/1601.
 */

uint64_t time_ms(void)
{
    FILETIME ft;
    uint64_t res;

    GetSystemTimeAsFileTime(&ft);

    /* calculate time as 100ns increments since Jan 1, 1601. */
    res = (uint64_t)(ft.dwLowDateTime) + ((uint64_t)(ft.dwHighDateTime) << 32);

    /* get time in ms */
    res = res / 10000;

    return  res;
}


