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
