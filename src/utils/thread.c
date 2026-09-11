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

/*
 * Portable thread creation and joining.
 *
 * All platform-specific code is confined to the static functions at the
 * bottom of this file.  The public functions hold the argument checking,
 * allocation and logging that both platforms share.
 */

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#    include <processthreadsapi.h>
#else
#    include <pthread.h>
#    include <sched.h>
#endif

#include <inttypes.h>
#include <stdint.h>

#include <libplctag/lib/libplctag.h>
#include <utils/mem.h>
#include <utils/time.h>
#include <utils/debug.h>
#include <utils/thread.h>


struct thread_t {
#ifdef _WIN32
    HANDLE h_thread;
#else
    pthread_t p_thread;
#endif
};


static int32_t thread_start(struct thread_t *t, thread_func_t func, void *arg);
static int32_t thread_wait(struct thread_t *t);
static void thread_close(struct thread_t *t);


/*
 * Start a new thread running func with arg passed to it.
 *
 * The stacksize argument is accepted but ignored on both platforms.  It is
 * kept so that existing callers do not change and so that a future
 * implementation has somewhere to put the value.
 */
extern int32_t thread_create(thread_p *t, thread_func_t func, int32_t stacksize, void *arg) {
    int32_t rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Starting.");

    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Ignoring stacksize (%" PRId32 ") parameter.", stacksize);

    if(!t) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Null pointer to thread pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!func) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Null thread function pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    *t = (thread_p)mem_alloc((int)sizeof(struct thread_t));

    if(!*t) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_ERROR, 0, "Unable to allocate memory for thread!");
        return PLCTAG_ERR_NO_MEM;
    }

    rc = thread_start(*t, func, arg);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_ERROR, 0, "Error creating thread!");
        mem_free(*t);
        *t = NULL;
        return rc;
    }

    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * Wait for the thread to terminate, then release the underlying OS resources
 * and free the handle.  The caller's thread pointer is set to NULL.
 */
extern int32_t thread_join(thread_p *t) {
    int64_t join_start_time = 0;
    int32_t rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Starting.");

    if(!t || !*t) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Null thread pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    join_start_time = time_ms();

    rc = thread_wait(*t);

    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, 0, "Thread join completed after %" PRId64 "ms.", (time_ms() - join_start_time));

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_ERROR, 0, "Error joining thread!");
        return rc;
    }

    thread_close(*t);

    mem_free(*t);

    *t = NULL;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * Terminate the calling thread.  Does not return.
 */
extern void thread_stop(void) {
#ifdef _WIN32
    ExitThread((DWORD)0);
#else
    pthread_exit((void *)0);
#endif
}


/*
 * Give up the remainder of this thread's timeslice.
 */
extern void thread_yield(void) {
#ifdef _WIN32
    /*
     * SwitchToThread() yields only to another thread on this core, so fall
     * back to a zero-length sleep, which will also consider other cores.
     */
    if(!SwitchToThread()) { Sleep(0); }
#else
    sched_yield();
#endif
}


#ifdef _WIN32

static int32_t thread_start(struct thread_t *t, thread_func_t func, void *arg) {
    t->h_thread = CreateThread(NULL,  /* default security attributes */
                               0,     /* use default stack size      */
                               func,  /* thread function             */
                               arg,   /* argument to thread function */
                               0,     /* use default creation flags  */
                               NULL); /* do not need thread ID       */

    if(!t->h_thread) { return PLCTAG_ERR_THREAD_CREATE; }

    return PLCTAG_STATUS_OK;
}


static int32_t thread_wait(struct thread_t *t) {
    if(WaitForSingleObject(t->h_thread, (DWORD)INFINITE)) { return PLCTAG_ERR_THREAD_JOIN; }

    return PLCTAG_STATUS_OK;
}


/* Waiting on a Windows thread does not release its handle. */
static void thread_close(struct thread_t *t) { CloseHandle(t->h_thread); }

#else

static int32_t thread_start(struct thread_t *t, thread_func_t func, void *arg) {
    /* pthread_create() returns 0 on success. */
    if(pthread_create(&(t->p_thread), NULL, func, arg)) { return PLCTAG_ERR_THREAD_CREATE; }

    return PLCTAG_STATUS_OK;
}


static int32_t thread_wait(struct thread_t *t) {
    if(pthread_join(t->p_thread, NULL)) { return PLCTAG_ERR_THREAD_JOIN; }

    return PLCTAG_STATUS_OK;
}


/* pthread_join() already released everything. */
static void thread_close(struct thread_t *t) { (void)t; }

#endif

