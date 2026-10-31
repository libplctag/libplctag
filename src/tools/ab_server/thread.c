/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *   Author Heath Raftery                                                  *
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

#include "compat.h"

#if IS_WINDOWS
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>

#    include <handleapi.h>
#    include <process.h>
#    include <processthreadsapi.h>
#    include <synchapi.h>
#else
#    include <pthread.h>
#endif

#include <stdlib.h>
#include <libplctag/api/libplctag.h>
#include "thread.h"
#include "log.h"


struct thread_t {
#if IS_WINDOWS
    HANDLE h_thread;
#else
    pthread_t p_thread;
#endif
};


/*
 * thread_create()
 *
 * Start up a new thread.  This allocates the thread_t structure and starts
 * the passed function.  The arg argument is passed to the function.  POSIX threads get a
 * fixed 1 MB stack: the TCP connection thread keeps two 64 KB buffers on it.
 */

int32_t thread_create(thread_p *t, thread_func_t func, void *arg) {
    log_detail("Starting.");

    if(!t) {
        log_warn("Null thread pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    *t = (thread_p)calloc(1, sizeof(struct thread_t));

    if(!*t) {
        log_error("Failed to allocate memory for thread.");
        return PLCTAG_ERR_NULL_PTR;
    }

#if IS_WINDOWS
    (*t)->h_thread = CreateThread(NULL,  /* default security attributes */
                                  0,     /* use default stack size      */
                                  func,  /* thread function             */
                                  arg,   /* argument to thread function */
                                  0,     /* use default creation flags  */
                                  NULL); /* do not need thread ID       */

    if(!(*t)->h_thread) {
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);

    int32_t rc = pthread_create(&((*t)->p_thread), &attr, func, arg);
    pthread_attr_destroy(&attr);

    if(rc) {
#endif
        log_error("Error creating thread.");
        free(*t);
        *t = NULL;
        return PLCTAG_ERR_THREAD_CREATE;
    }

    log_detail("Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * thread_join()
 *
 * Wait for the argument thread to stop and then continue.
 */

int32_t thread_join(thread_p t) {
#if !defined(IS_WINDOWS)
    void *unused;
#endif

    log_detail("Starting.");

    if(!t) {
        log_warn("Null thread pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

#if IS_WINDOWS
    if(WaitForSingleObject(t->h_thread, (DWORD)INFINITE)) {
#else
    if(pthread_join(t->p_thread, &unused)) {
#endif
        log_error("Error joining thread.");
        return PLCTAG_ERR_THREAD_JOIN;
    }

    log_detail("Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * thread_detach
 *
 * Detach the calling thread.  You cannot call thread_join on a detached thread!
 * Windows has no equivalent; thread_destroy closing the handle does the job there.
 */

int32_t thread_detach(void) {
#if !defined(IS_WINDOWS)
    pthread_detach(pthread_self());
#endif

    return PLCTAG_STATUS_OK;
}


/*
 * thread_destroy
 *
 * This gets rid of the resources of a thread struct.  The thread in
 * question must be dead first!
 */

int32_t thread_destroy(thread_p *t) {
    log_detail("Starting.");

    if(!t || !*t) {
        log_warn("Null thread pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

#if IS_WINDOWS
    CloseHandle((*t)->h_thread);
#endif

    free(*t);
    *t = NULL;

    log_detail("Done.");

    return PLCTAG_STATUS_OK;
}
