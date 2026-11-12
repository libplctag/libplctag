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
 *   License along with this program; if not, write to the                *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

/*
 * Mutex wait shim for perf_benchmark (Linux only).
 *
 * LD_PRELOAD this into a perf_benchmark linked against the shared library.
 * It wraps pthread_mutex_lock(): a lock that trylock gets at once costs
 * nothing extra and is not counted; a lock that has to block is counted and
 * its wait time added up.  perf_benchmark reads the totals through
 * perf_mutex_wait_stats() at the start and end of the measured window.
 *
 * Works with any library version, so v2.7.2 and HEAD are measured the same
 * way.  Only pthread mutexes are seen: not spin locks, atomics or the mutex
 * reacquire inside pthread_cond_wait().
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

/* the build uses -fvisibility=hidden, and these two must be seen to interpose. */
#define SHIM_EXPORT __attribute__((visibility("default")))

static int (*real_lock)(pthread_mutex_t *m) = NULL;
static atomic_int_fast64_t blocked_count = 0;
static atomic_int_fast64_t wait_ns = 0;


static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}


__attribute__((constructor)) static void shim_init(void) {
    /* POSIX allows this cast; the dlopen(3) idiom avoids the ISO C object-to-function warning. */
    *(void **)(&real_lock) = dlsym(RTLD_NEXT, "pthread_mutex_lock");
}


SHIM_EXPORT int pthread_mutex_lock(pthread_mutex_t *m) {
    int64_t start = 0;
    int rc = 0;

    /* a constructor in another library can lock before ours runs. */
    if(!real_lock) { shim_init(); }

    if(pthread_mutex_trylock(m) == 0) { return 0; }

    start = now_ns();
    rc = real_lock(m);
    atomic_fetch_add_explicit(&wait_ns, now_ns() - start, memory_order_relaxed);
    atomic_fetch_add_explicit(&blocked_count, 1, memory_order_relaxed);

    return rc;
}


SHIM_EXPORT void perf_mutex_wait_stats(int64_t *blocked, int64_t *waited_ns) {
    *blocked = (int64_t)atomic_load(&blocked_count);
    *waited_ns = (int64_t)atomic_load(&wait_ns);
}
