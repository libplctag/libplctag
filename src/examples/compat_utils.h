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

#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

/* FIXME - centralize this platform setting! */
#if defined(__unix__) || defined(APPLE) || defined(__APPLE__) || defined(__MACH__) || defined(__linux__)
#    include <pthread.h>
#    include <strings.h>
#    include <unistd.h>

#    define POSIX_PLATFORM

#    define compat_localtime_r localtime_r
#    define compat_strcasecmp strcasecmp
#    define compat_strdup strdup
#    define compat_snprintf snprintf
#    define compat_sscanf sscanf

typedef pthread_t compat_thread_t;
typedef pthread_once_t compat_once_t;
typedef pthread_mutex_t compat_mutex_t;
typedef pthread_cond_t compat_cond_t;

#elif defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__) || defined(WIN64) || defined(_WIN64)
#    define WINDOWS_PLATFORM

#    define WIN32_LEAN_AND_MEAN

#    include <Windows.h>

#    include <time.h>


static inline int compat_localtime_r(const time_t *timep, struct tm *result) { return localtime_s(result, timep); }

static inline int compat_strcasecmp(const char *s1, const char *s2) { return stricmp(s1, s2); }

static inline char *compat_strdup(const char *s) { return _strdup(s); }

static inline int compat_snprintf(char *str, size_t size, const char *format, ...) {
    va_list args;
    int rc;

    va_start(args, format);
    rc = vsnprintf_s(str, size, _TRUNCATE, format, args);
    va_end(args);

    return rc;
}

static inline int compat_sscanf(const char *str, const char *format, ...) {
    va_list args;
    int rc;

    va_start(args, format);
#    ifdef _MSVC_VER
    rc = vsscanf_s(str, format, args);
#    else
    rc = vsscanf(str, format, args);
#    endif
    va_end(args);

    return rc;
}

typedef HANDLE compat_thread_t;
typedef volatile long compat_once_t;
typedef CRITICAL_SECTION compat_mutex_t;
typedef CONDITION_VARIABLE compat_cond_t;


#else
#    error "Unsupported platform!"
#endif


/* threads */
extern int compat_thread_create(compat_thread_t *thread, void *(*start_routine)(void *), void *arg);
extern int compat_thread_detach(compat_thread_t thread);
extern void compat_thread_exit(void *retval); /* no return */
extern int compat_thread_join(compat_thread_t thread, void **retval);
extern compat_thread_t compat_thread_self(void);

extern int compat_thread_once(compat_once_t *once_control, void (*init_routine)(void));

extern void compat_thread_yield(void);


/* mutexes*/

extern int compat_mutex_init(compat_mutex_t *mutex);
extern int compat_mutex_lock(compat_mutex_t *mutex);
extern int compat_mutex_timedlock(compat_mutex_t *mtx, const uint32_t timeout_duration_ms);
extern int compat_mutex_trylock(compat_mutex_t *mutex);
extern int compat_mutex_unlock(compat_mutex_t *mutex);
extern int compat_mutex_destroy(compat_mutex_t *mutex);


/* condition variables */

extern int compat_cond_init(compat_cond_t *cond);
extern int compat_cond_signal(compat_cond_t *cond);
extern int compat_cond_broadcast(compat_cond_t *cond);
extern int compat_cond_wait(compat_cond_t *cond, compat_mutex_t *mutex);
extern int compat_cond_timedwait(compat_cond_t *cond, compat_mutex_t *mtx, const uint32_t timeout_duration_ms);
extern int compat_cond_destroy(compat_cond_t *cond);


/* current system epoch time in milliseconds. */
extern int64_t compat_time_ms(void);

/* cross platform sleep in milliseconds */
extern int compat_sleep_ms(uint32_t sleep_duration_ms, uint32_t *remaining_duration_ms);

/* cross platform yield CPU */
extern void compat_thread_yield(void);


/* catch terminate/interrupt signals/events */
extern int compat_set_interrupt_handler(void (*handler)(void));


#define RANDOM_U64_ERROR (UINT64_MAX)
extern uint64_t compat_random_u64(uint64_t upper_bound);


extern int compat_fprintf(FILE *stream, const char *format, ...);

#ifdef __cplusplus
}
#endif
