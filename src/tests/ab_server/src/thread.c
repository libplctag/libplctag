/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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

#include "memory.h"
#include "thread.h"
#include "utils.h"


struct thread_t {
#if IS_WINDOWS
    HANDLE h_thread;
#else
    pthread_t p_thread;
#endif
    int initialized;
};


/*
 * thread_create()
 *
 * Start up a new thread.  This allocates the thread_t structure and starts
 * the passed function.  The arg argument is passed to the function.
 *
 * TODO - use the stacksize!
 */

extern int thread_create(thread_p *t, thread_func_t func, int stacksize, void *arg) {
    info("DETAIL: Starting.");

    info("DETAIL: Warning: ignoring stacksize (%d) parameter.", stacksize);

    if(!t) {
        info("WARN: null thread pointer.");
        return THREAD_ERR_NULL_PTR;
    }

    *t = (thread_p)mem_alloc(sizeof(struct thread_t));

    if(!*t) {
        error("ERROR: Failed to allocate memory for thread.");
        return THREAD_ERR_NULL_PTR;
    }

    /* create a thread. */
#if IS_WINDOWS
    (*t)->h_thread = CreateThread(NULL,  /* default security attributes */
                                  0,     /* use default stack size      */
                                  func,  /* thread function             */
                                  arg,   /* argument to thread function */
                                  0,     /* use default creation flags  */
                                  NULL); /* do not need thread ID       */

    if(!(*t)->h_thread) {
        info("WARN: error creating thread.");
        mem_free(*t);
        *t = NULL;

        return THREAD_ERR_THREAD_CREATE;
    }

    /* mark as initialized */
    (*t)->initialized = 1; /* note this is never used and not even set in the posix version */
#else
    if(pthread_create(&((*t)->p_thread), NULL, func, arg)) {
        error("ERROR: error creating thread.");
        return THREAD_ERR_THREAD_CREATE;
    }
#endif

    info("DETAIL: Done.");

    return THREAD_STATUS_OK;
}


/*
 * platform_thread_stop()
 *
 * Stop the current thread.  Does not take arguments.  Note: the thread
 * ends completely and this function does not return!
 */
void thread_stop(void) {
#if IS_WINDOWS
    ExitThread((DWORD)0);
#else
    pthread_exit((void *)0);
#endif
}


/*
 * thread_kill()
 *
 * Kill another thread.
 */

void thread_kill(thread_p t) {
    if(t) {
#if IS_WINDOWS
        TerminateThread(t->h_thread, (DWORD)0);
#else
#    ifdef __ANDROID__
        pthread_kill(t->p_thread, 0);
#    else
        pthread_cancel(t->p_thread);
#    endif /* __ANDROID__ */
#endif     /* IS_WINDOWS */
    }
}

/*
 * thread_join()
 *
 * Wait for the argument thread to stop and then continue.
 */

int thread_join(thread_p t) {
#if !defined(IS_WINDOWS)
    void *unused;
#endif

    info("DETAIL: Starting.");

    if(!t) {
        info("WARN: null thread pointer.");
        return THREAD_ERR_NULL_PTR;
    }

#if IS_WINDOWS
    /* FIXME - check for uninitialized threads */
    if(WaitForSingleObject(t->h_thread, (DWORD)INFINITE)) {
#else
    if(pthread_join(t->p_thread, &unused)) {
#endif
        error("ERROR: Error joining thread.");
        return THREAD_ERR_THREAD_JOIN;
    }

    info("DETAIL: Done.");

    return THREAD_STATUS_OK;
}


/*
 * thread_detach
 *
 * Detach the thread.  You cannot call thread_join on a detached thread!
 */

extern int thread_detach(void) {
#if IS_WINDOWS
    /* FIXME - it does not look like you can do this on Windows??? */
#else
    pthread_detach(pthread_self());
#endif

    return THREAD_STATUS_OK;
}


/*
 * thread_destroy
 *
 * This gets rid of the resources of a thread struct.  The thread in
 * question must be dead first!
 */
extern int thread_destroy(thread_p *t) {
    info("DETAIL: Starting.");

    if(!t || !*t) {
        info("WARN: null thread pointer.");
        return THREAD_ERR_NULL_PTR;
    }

#if IS_WINDOWS
    CloseHandle((*t)->h_thread);
#endif

    mem_free(*t);

    *t = NULL;

    info("DETAIL: Done.");

    return THREAD_STATUS_OK;
}
