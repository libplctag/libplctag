/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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

#ifdef __cplusplus
extern "C" {
#endif

/* FIXME - centralize this platform setting! */
#if defined(__unix__) || defined(APPLE) || defined(__APPLE__) || defined(__MACH__) || defined(__linux__)
#    include <pthread.h>
#    include <strings.h>
#    include <unistd.h>
#    define snprintf_platform snprintf
#    define sscanf_platform sscanf

#    define POSIX_PLATFORM


#elif defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__) || defined(WIN64) || defined(_WIN64)
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>

#    define localtime_r(a, b) localtime_s((b), (a))
#    define strcasecmp _stricmp
#    define strdup _strdup
#    define snprintf_platform sprintf_s
#    define sscanf_platform sscanf_s

#    define WINDOWS_PLATFORM

typedef HANDLE pthread_t;
typedef volatile long pthread_once_t;
typedef CRITICAL_SECTION pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;

typedef struct pthread_attr_t pthread_attr_t;
typedef struct pthread_mutexattr_t pthread_mutexattr_t;
typedef struct pthread_condattr_t pthread_condattr_t;

/* threads */
extern int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg);
extern int pthread_detach(pthread_t thread);
extern void pthread_exit(void *retval); /* no return */
extern int pthread_join(pthread_t thread, void **retval);
extern pthread_t pthread_self(void);

extern int pthread_once(pthread_once_t *once_control, void (*init_routine)(void));

extern void system_yield(void);


/* mutexes*/
#    define PTHREAD_MUTEX_INITIALIZER {0}


extern int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *mutexattr);
extern int pthread_mutex_destroy(pthread_mutex_t *mutex);
extern int pthread_mutex_lock(pthread_mutex_t *mutex);
extern int pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *abstime);
extern int pthread_mutex_trylock(pthread_mutex_t *mutex);
extern int pthread_mutex_unlock(pthread_mutex_t *mutex);


/* condition variables */

#    define PTHREAD_COND_INITIALIZER {0}


extern int pthread_cond_init(pthread_cond_t *cond, pthread_condattr_t *cond_attr);
extern int pthread_cond_signal(pthread_cond_t *cond);
extern int pthread_cond_broadcast(pthread_cond_t *cond);
extern int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
extern int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime);
extern int pthread_cond_destroy(pthread_cond_t *cond);


#else
#    error "Not a supported platform for threads, mutexes and condition variables!!"
#endif

#include <stdint.h>


/* Need some way to get the current system time in milliseconds. */
extern int64_t system_time_ms(void);

/* cross platform sleep in milliseconds */
extern int system_sleep_ms(uint32_t sleep_duration_ms, uint32_t *remaining_duration_ms);

/* cross platform yield CPU */
extern void system_yield(void);

/*
 * helpful versions of the pthreads theads API, struct timespec is
 * useful but not particularly friendly.
 */

extern int pthread_mutex_timedlock_ms(pthread_mutex_t *mtx, const uint32_t timeout_duration_ms);
extern int pthread_cond_timedwait_ms(pthread_cond_t *cond, pthread_mutex_t *mtx, const uint32_t timeout_duration_ms);

enum { INTERRUPT_HANDLER_SUCCESS, INTERRUPT_HANDLER_ERROR };

/* catch terminate/interrupt signals/events */
extern int set_interrupt_handler(void (*handler)(void));


#define RANDOM_U64_ERROR (UINT64_MAX)
extern uint64_t util_random_u64(uint64_t upper_bound);


#ifdef __cplusplus
}
#endif
