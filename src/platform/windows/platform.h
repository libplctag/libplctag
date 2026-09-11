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
 * NOTE: platform.h no longer holds any code of its own.  Threads, spin locks,
 * mutexes, the old "condition variables" (now interruptible sleeps), sockets,
 * memory, strings, time and the struct packing macros have all moved into
 * utils/, and both platform.c files are gone.  This header survives only so that
 * the ~180 files including <platform.h> keep compiling,
 * and to supply the two things that are genuinely Windows-only: the winsock
 * include block and the ssize_t typedef that ab/cip.c needs.  New code should
 * include the utils/ headers directly.
 */
#include <utils/macros.h>
#include <utils/mem.h>
#include <utils/mutex.h>
#include <utils/nap.h>
#include <utils/socket.h>
#include <utils/spinlock.h>
#include <utils/str.h>
#include <utils/thread.h>
#include <utils/time.h>

/* Apparently ssize_t is not on Windows. */
#if defined(_MSC_VER)
#    include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif



/*#ifdef __cplusplus
}
#endif
*/


#endif
