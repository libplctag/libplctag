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
 ****************************** WINDOWS ************************************
 **************************************************************************/


#ifndef __PLATFORM_H__
#define __PLATFORM_H__

/*#ifdef __cplusplus
extern "C"
{
#endif
*/

/* KEEP THE SPACES BETWEEN LINES!  The order is required! */
#include <winsock2.h>

#include <windows.h>

#include <ws2tcpip.h>


#include <tchar.h>
#include <strsafe.h>
#include <io.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include <process.h>
#include <time.h>
#include <stdio.h>

#include <stdint.h>
#include <malloc.h>

/*
 * NOTE: platform.h is being refactored.  Threads, spin locks, mutexes, the old
 * "condition variables" (now interruptible sleeps), sockets, memory and strings
 * have moved to utils/thread.h, utils/spinlock.h, utils/mutex.h, utils/nap.h,
 * utils/socket.h, utils/mem.h and utils/str.h.  They are included here so that
 * existing consumers keep compiling unchanged.  Time and a couple of macros are
 * what is left.  New code should include the utils/ headers directly.
 */
#include <utils/mem.h>
#include <utils/mutex.h>
#include <utils/nap.h>
#include <utils/socket.h>
#include <utils/spinlock.h>
#include <utils/str.h>
#include <utils/thread.h>


/* WinSock does not define this or support signals */
#define MSG_NOSIGNAL 0

#ifdef _MSC_VER
/* MS Visual Studio C compiler. */
#    define START_PACK __pragma(pack(push, 1))
#    define END_PACK __pragma(pack(pop))
#    define __PRETTY_FUNCTION__ __FUNCTION__
#else
/* MinGW on Windows. */
#    define START_PACK
#    define END_PACK __attribute__((packed))
#    define __PRETTY_FUNCTION__ __func__
#endif

/* export definitions. */

#define USE_STD_VARARG_MACROS 1

/* Apparently ssize_t is not on Windows. */
#if defined(_MSC_VER)
#    include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif


/* time functions */
extern int sleep_ms(int ms);
extern int64_t time_ms(void);
extern struct tm *localtime_r(const time_t *timep, struct tm *result);

/* some functions can be simply replaced */
#define snprintf_platform sprintf_s


/*#ifdef __cplusplus
}
#endif
*/


#endif
