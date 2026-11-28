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
#include <stdbool.h>
#include <malloc.h>


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

/* VS C++ uses foo[] to denote a zero length array. */
#define ZLA_SIZE

/* export definitions. */

#define USE_STD_VARARG_MACROS 1

/* Apparently ssize_t is not on Windows. */
#if defined(_MSC_VER)
#    include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif


/* memory functions/defs */
extern void *mem_alloc(int32_t size);
extern void *mem_realloc(void *orig, int32_t size);
extern void mem_free(const void *mem);
extern void mem_set(void *d1, int c, int32_t size);
extern void mem_copy(void *dest, void *src, int32_t size);
extern int32_t mem_copy_bounded(void *dest, int32_t dest_capacity, void *src, int32_t size);
extern void mem_move(void *dest, void *src, int32_t size);

/*
 * Standard library calls whose names differ between platforms.  The portable
 * bodies in platform_common.c use these rather than an #ifdef per call site.
 */
#define platform_strcasecmp _stricmp
#define platform_strncasecmp _strnicmp
#define platform_strdup _strdup

/* string functions/defs */
extern int str_cmp(const char *first, const char *second);
extern int str_cmp_i(const char *first, const char *second);
extern int str_cmp_i_n(const char *first, const char *second, int num_chars);
extern char *str_str_cmp_i(const char *haystack, const char *needle);
extern int str_copy(char *dst, int dst_size, const char *src);
extern int str_length(const char *str);
extern char *str_dup(const char *str);
extern int str_to_int(const char *str, int *val);
extern char **str_split(const char *str, const char *sep);

/* mutex functions/defs */
/*
 * Mutexes are recursive and embedded by value in their owner.  Locking cannot fail: a mutex that
 * is not initialized means its owner is already gone, which no return code can make safe
 * (review U5, Option C).  An owner initializes its mutex before any step that can fail, and
 * mutex_destroy() on a zeroed, never-initialized mutex does nothing.
 */
typedef struct mutex_t {
    CRITICAL_SECTION cs;
    bool initialized;
} mutex_t;
typedef mutex_t *mutex_p;

extern int mutex_init(mutex_p m);
extern void mutex_destroy(mutex_p m);

extern void mutex_lock_impl(const char *func, int line_num, mutex_p m);
/* PLCTAG_STATUS_OK when locked, PLCTAG_ERR_MUTEX_LOCK when another thread holds it. */
extern int mutex_try_lock_impl(const char *func, int line_num, mutex_p m);
/* PLCTAG_ERR_MUTEX_UNLOCK when the calling thread does not hold it; plc_tag_unlock() reports that. */
extern int mutex_unlock_impl(const char *func, int line_num, mutex_p m);

#if defined(_WIN32) && defined(_MSC_VER)
/* MinGW on Windows does not need this. */
#    define __func__ __FUNCTION__
#endif

#define mutex_lock(m) mutex_lock_impl(__func__, __LINE__, m)
#define mutex_try_lock(m) mutex_try_lock_impl(__func__, __LINE__, m)
#define mutex_unlock(m) mutex_unlock_impl(__func__, __LINE__, m)

/* macros are evil */

/* paste a per-line suffix onto a name, so nested blocks on different lines get distinct names. */
#define PLCTAG_CAT2(a, b) a##b
#define PLCTAG_CAT(a, b) PLCTAG_CAT2(a, b)
#define LINE_ID(base) PLCTAG_CAT(base, __LINE__)

/*
 * Use this one like this:
 *
 *     critical_block(&obj->mutex) {
 *         locked_data++;
 *         foo(locked_data);
 *     }
 *
 * The macro locks and unlocks for you.  break and continue leave the block and unlock.  Never
 * use return or goto inside it: they skip the unlock.  break does NOT leave a loop that
 * surrounds the block.
 */
#define critical_block(lock)                                                                                       \
    for(bool LINE_ID(cb_outer_) = (mutex_lock(lock), true); LINE_ID(cb_outer_); LINE_ID(cb_outer_) = false, mutex_unlock(lock)) \
        for(bool LINE_ID(cb_inner_) = true; LINE_ID(cb_inner_); LINE_ID(cb_inner_) = false)

/* thread functions/defs */
typedef struct thread_t *thread_p;
// typedef PTHREAD_START_ROUTINE thread_func_t;
// typedef DWORD /*WINAPI*/ (*thread_func_t)(void *lpParam );
extern int thread_create(thread_p *t, LPTHREAD_START_ROUTINE func, int stacksize, void *arg);
extern int thread_join(thread_p t);
extern int thread_destroy(thread_p *t);

#define THREAD_FUNC(func) DWORD __stdcall func(LPVOID arg)
#define THREAD_RETURN(val) return (DWORD)val;


#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && (defined(__MINGW32__) || defined(__MINGW64__))
#    define THREAD_LOCAL _Thread_local
#else /* use Windows __declspec attribute */
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#    include <processthreadsapi.h>
#    define THREAD_LOCAL __declspec(thread)
#    define thread_func_t LPTHREAD_START_ROUTINE
#    define NO_RETURN __declspec(noreturn)
#    define THREAD_FUNC(func) DWORD __stdcall func(LPVOID arg)
#    define THREAD_RETURN(val) return (DWORD)val;

#endif


/* atomic operations */
#define spin_block(lock)                                                                                 \
    for(int LINE_ID(sb_outer_) = lock_acquire(lock); LINE_ID(sb_outer_); LINE_ID(sb_outer_) = 0, lock_release(lock)) \
        for(bool LINE_ID(sb_inner_) = true; LINE_ID(sb_inner_); LINE_ID(sb_inner_) = false)

typedef volatile long int lock_t;

#define LOCK_INIT (0)

/* returns non-zero when lock acquired, zero when lock operation failed */
extern int lock_acquire_try(lock_t *lock);
extern int lock_acquire(lock_t *lock);
extern void lock_release(lock_t *lock);


/* condition variables */
typedef struct cond_t *cond_p;
extern int cond_create(cond_p *c);
extern int cond_wait_impl(const char *func, int line_num, cond_p c, int timeout_ms);
extern int cond_signal_impl(const char *func, int line_num, cond_p c);
extern int cond_clear_impl(const char *func, int line_num, cond_p c);
extern int cond_destroy(cond_p *c);

#define cond_wait(c, t) cond_wait_impl(__func__, __LINE__, c, t)
#define cond_signal(c) cond_signal_impl(__func__, __LINE__, c)
#define cond_clear(c) cond_clear_impl(__func__, __LINE__, c)

/* socket functions */
typedef struct sock_t *sock_p;
typedef enum {
    SOCK_EVENT_NONE = 0,
    SOCK_EVENT_TIMEOUT = (1 << 0),
    SOCK_EVENT_DISCONNECT = (1 << 1),
    SOCK_EVENT_ERROR = (1 << 2),
    SOCK_EVENT_CAN_READ = (1 << 3),
    SOCK_EVENT_CAN_WRITE = (1 << 4),
    SOCK_EVENT_WAKE_UP = (1 << 5),
    SOCK_EVENT_CONNECT = (1 << 6),

    SOCK_EVENT_DEFAULT_MASK = (SOCK_EVENT_TIMEOUT | SOCK_EVENT_DISCONNECT | SOCK_EVENT_ERROR | SOCK_EVENT_WAKE_UP)
} sock_event_t;
extern int socket_create(sock_p *s);
extern int socket_connect_tcp_start(sock_p s, const char *host, int port);
extern int socket_connect_tcp_check(sock_p s, int timeout_ms);
extern int socket_wait_event(sock_p sock, int events, int timeout_ms);
extern int socket_wake(sock_p sock);
extern int socket_read(sock_p s, uint8_t *buf, int size, int timeout_ms);
extern int socket_write(sock_p s, uint8_t *buf, int size, int timeout_ms);
extern int socket_close(sock_p s);
extern int socket_destroy(sock_p *s);



/* time functions */
extern int sleep_ms(int ms);
extern struct tm *localtime_r(const time_t *timep, struct tm *result);

/* some functions can be simply replaced */
#define snprintf_platform sprintf_s


/*#ifdef __cplusplus
}
#endif
*/


#endif
