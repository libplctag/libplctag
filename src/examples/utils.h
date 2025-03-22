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


#pragma once

/* FIXME - centralize this platform setting! */
#if defined(__unix__) || defined(APPLE) || defined(__APPLE__) || defined(__MACH__) || defined(__linux__)
    #include <unistd.h>
    #include <strings.h>
    #define snprintf_platform snprintf
    #define sscanf_platform sscanf

    #ifdef __STDC_NO_THREADS__
        #include <pthread.h>
    #else
        #include <threads.h>
    #endif

#elif defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__) || defined(WIN64) || defined(_WIN64)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #define strcasecmp _stricmp
    #define strdup _strdup
    #define snprintf_platform sprintf_s
    #define sscanf_platform sscanf_s
#else
    #error "Not a supported platform!"
#endif

#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

extern int util_sleep_ms(int ms);
extern int64_t util_time_ms(void);

#ifdef __STDC_NO_THREADS__

/* roll our own */

typedef struct thrd_t thrd_t;
typedef struct mtx_t mtx_t;
typedef struct cnd_t cnd_t;

/* threads */

extern int thrd_create(thrd_t *thrd, thrd_start_t func, void *arg_ptr);
extern thrd_t thrd_current(void);
extern int thrd_detach(thrd_t thrd);
extern int thrd_equal(thrd_t first, thrd_t second);
extern int thrd_exit(int res);
extern int thrd_join(thrd_t thrd, int *result);
extern int thrd_sleep(const struct timespec *sleep_duration, struct timespec *remaining_duration);
extern void thrd_yield(void);


/* mutexes */

enum {
    mtx_plain = 0,
    mtx_recursive = 1,
    mtx_timed = 2
};

extern int mtx_init(mtx_t *mtx, int type);
extern int mtx_lock(mtx_t *mtx);
extern int mtx_timedlock(mtx_t *mtx, const struct timespec *timeout_time);
extern int mtx_trylock(mtx_t *mtx);
extern int mtx_unlock(mtx_t *mtx);
extern int mtx_destroy(mtx_t *mtx);


/* condition variables */

int cnd_broadcast(cnd_t *cond);
int cnd_destroy(cnd_t *cond);
int cnd_init(cnd_t *cond);
int cnd_signal(cnd_t *cond);
int cnd_timedwait(cnd_t *cond, mtx_t *mtx, const struct timespec *time_point);
int cnd_wait(cnd_t *cond, mtx_t *mtx);


#endif

#ifdef __cplusplus
}
#endif
