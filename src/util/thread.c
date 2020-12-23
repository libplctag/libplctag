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

#include <lib/libplctag.h>
#include <util/debug.h>
#include <util/mem.h>
#include <util/platform.h>
#include <util/thread.h>

#include <stddef.h>

#ifdef PLATFORM_IS_WINDOWS
    #include <processthreadsapi.h>

    struct thread_t {
        HANDLE h_thread;
        int initialized;
    };
#else
    #include <pthread.h>

    struct thread_t {
        pthread_t p_thread;
        int initialized;
    };
#endif

/*
 * thread_create()
 *
 * Start up a new thread.  This allocates the thread_t structure and starts
 * the passed function.  The arg argument is passed to the function.
 *
 * FIXME - use the stacksize!
 */

extern int thread_create(thread_p *t, thread_func_t func, int stacksize, void *arg)
{
    pdebug(DEBUG_DETAIL, "Starting.");

    pdebug(DEBUG_DETAIL, "Warning: ignoring stacksize (%d) parameter.", stacksize);

    if(!t) {
        pdebug(DEBUG_WARN, "null thread pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    *t = (thread_p)mem_alloc(sizeof(struct thread_t));

    if(! *t) {
        pdebug(DEBUG_ERROR, "Failed to allocate memory for thread.");
        return PLCTAG_ERR_NULL_PTR;
    }

#ifdef PLATFORM_IS_WINDOWS
    /* create a thread. */
    (*t)->h_thread = CreateThread(
                         NULL,                   /* default security attributes */
                         0,                      /* use default stack size      */
                         func,                   /* thread function             */
                         arg,                    /* argument to thread function */
                         0,                      /* use default creation flags  */
                         NULL);                  /* do not need thread ID       */

    if(!(*t)->h_thread) {
        pdebug(DEBUG_ERROR, "error creating thread.");
        mem_free(*t);
        *t=NULL;

        return PLCTAG_ERR_THREAD_CREATE;
    }
#else
    /* create a pthread.  0 means success. */
    if(pthread_create(&((*t)->p_thread), NULL, func, arg)) {
        mem_free(*t);
        *t = NULL;
        pdebug(DEBUG_ERROR, "error creating thread.");
        return PLCTAG_ERR_THREAD_CREATE;
    }
#endif

    (*t)->initialized = 1;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * thread_stop()
 *
 * Stop the current thread.  Does not take arguments.  Note: the thread
 * ends completely and this function does not return!
 */
void thread_stop(void)
{
#ifdef PLATFORM_IS_WINDOWS
    ExitThread((DWORD)0);
#else
    pthread_exit((void*)0);
#endif
}


/*
 * thread_kill()
 *
 * Kill another thread.
 */

void thread_kill(thread_p t)
{
    pdebug(DEBUG_DETAIL, "Starting.");

    if(!t) {
        pdebug(DEBUG_WARN, "null thread pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(! t->initialized) {
        pdebug(DEBUG_WARN, "Thread is not initialized!");
        return PLCTAG_ERR_NULL_PTR;
    }

#ifdef PLATFORM_IS_WINDOWS
        TerminateThread(t->h_thread, (DWORD)0);
#elif defined(PLATFORM_IS_ANDROID)
        pthread_kill(t->p_thread, 0);
#else
    /* generic POSIX */
        pthread_cancel(t->p_thread);
#endif

    pdebug(DEBUG_DETAIL, "Done.");
}

/*
 * thread_join()
 *
 * Wait for the argument thread to stop and then continue.
 */

int thread_join(thread_p t)
{
    void *unused;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!t) {
        pdebug(DEBUG_WARN, "null thread pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(! t->initialized) {
        pdebug(DEBUG_WARN, "Thread is not initialized!");
        return PLCTAG_ERR_NULL_PTR;
    }

#ifdef PLATFORM_IS_WINDOWS
    if(WaitForSingleObject(t->h_thread, (DWORD)INFINITE)) {
        /*pdebug("Error joining thread.");*/
        return PLCTAG_ERR_THREAD_JOIN;
    }
#else
    if(pthread_join(t->p_thread,&unused)) {
        pdebug(DEBUG_ERROR, "Error joining thread.");
        return PLCTAG_ERR_THREAD_JOIN;
    }
#endif

    t->initialized = 0;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * thread_detach
 *
 * Detach the thread.  You cannot call thread_join on a detached thread!
 */

extern int thread_detach()
{
#ifdef PLATFORM_IS_WINDOWS
    pdebug(DEBUG_WARN, "Detaching a thread is unsupported on Windows!");
    return PLCTAG_ERR_UNSUPPORTED;
#else
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
extern int thread_destroy(thread_p *t)
{
    pdebug(DEBUG_DETAIL, "Starting.");

    if(!t || ! *t) {
        pdebug(DEBUG_WARN, "null thread pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    (*t)->initialized = 0;

    mem_free(*t);

    *t = NULL;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


