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
#include "mutex.h"
#include "utils.h"


struct mutex_t {
#if IS_WINDOWS
    HANDLE h_mutex;
#else
    pthread_mutex_t p_mutex;
#endif
    int initialized;
};

int mutex_create(mutex_p *m) {

#if !defined(IS_WINDOWS)
    pthread_mutexattr_t mutex_attribs;
#endif

    info("DETAIL: Starting.");

    if(*m) { info("WARN: Called with non-NULL pointer!"); }

    *m = (struct mutex_t *)mem_alloc(sizeof(struct mutex_t));

    if(!*m) {
        error("ERROR: null mutex pointer.");
        return MUTEX_ERR_NULL_PTR;
    }

#if IS_WINDOWS
    /* set up the mutex */
    (*m)->h_mutex = CreateMutex(NULL,  /* default security attributes  */
                                FALSE, /* initially not owned          */
                                NULL); /* unnamed mutex                */

    if(!(*m)->h_mutex) {
#else
    /* set up for recursive locking. */
    pthread_mutexattr_init(&mutex_attribs);
    pthread_mutexattr_settype(&mutex_attribs, PTHREAD_MUTEX_RECURSIVE);

    if(pthread_mutex_init(&((*m)->p_mutex), &mutex_attribs)) {
        pthread_mutexattr_destroy(&mutex_attribs);
#endif
        mem_free(*m);
        *m = NULL;
        error("ERROR: Error initializing mutex.");
        return MUTEX_ERR_MUTEX_INIT;
    }

    (*m)->initialized = 1;

#if IS_WINDOWS
#else
    pthread_mutexattr_destroy(&mutex_attribs);
#endif

    info("DETAIL: Done creating mutex %p.", *m);

    return MUTEX_STATUS_OK;
}


int mutex_lock_impl(const char *func, int line, mutex_p m) {
#if IS_WINDOWS
    DWORD dwWaitResult = ~WAIT_OBJECT_0;
#else
#endif

    info("SPEW: locking mutex %p, called from %s:%d.", m, func, line);

    if(!m) {
        info("WARN: null mutex pointer.");
        return MUTEX_ERR_NULL_PTR;
    }

    if(!m->initialized) { return MUTEX_ERR_MUTEX_INIT; }

#if IS_WINDOWS
    /* FIXME - This will potentially hang forever! */
    while(dwWaitResult != WAIT_OBJECT_0) { dwWaitResult = WaitForSingleObject(m->h_mutex, INFINITE); }
#else
    if(pthread_mutex_lock(&(m->p_mutex))) {
        info("WARN: error locking mutex.");
        return MUTEX_ERR_MUTEX_LOCK;
    }
#endif

    // info("SPEW: Done.");

    return MUTEX_STATUS_OK;
}


int mutex_try_lock_impl(const char *func, int line, mutex_p m) {
#if IS_WINDOWS
    DWORD dwWaitResult = 0;
#else
#endif

    info("SPEW: trying to lock mutex %p, called from %s:%d.", m, func, line);

    if(!m) {
        info("WARN: null mutex pointer.");
        return MUTEX_ERR_NULL_PTR;
    }

    if(!m->initialized) { return MUTEX_ERR_MUTEX_INIT; }

#if IS_WINDOWS
    dwWaitResult = WaitForSingleObject(m->h_mutex, 0);
    if(dwWaitResult != WAIT_OBJECT_0) {
#else
    if(pthread_mutex_trylock(&(m->p_mutex))) {
#endif
        info("SPEW: error locking mutex.");
        return MUTEX_ERR_MUTEX_LOCK;
    }
    /* else, we got the lock */

    /*info("DETAIL: Done.");*/

    return MUTEX_STATUS_OK;
}


int mutex_unlock_impl(const char *func, int line, mutex_p m) {
    info("SPEW: unlocking mutex %p, called from %s:%d.", m, func, line);

    if(!m) {
        info("WARN: null mutex pointer.");
        return MUTEX_ERR_NULL_PTR;
    }

    if(!m->initialized) { return MUTEX_ERR_MUTEX_INIT; }

#if IS_WINDOWS
    if(!ReleaseMutex(m->h_mutex)) {
#else
    if(pthread_mutex_unlock(&(m->p_mutex))) {
#endif
        info("WARN: error unlocking mutex.");
        return MUTEX_ERR_MUTEX_UNLOCK;
    }

    // info("SPEW: Done.");

    return MUTEX_STATUS_OK;
}


int mutex_destroy(mutex_p *m) {
    info("DETAIL: Starting to destroy mutex %p.", m);

    if(!m || !*m) {
        info("WARN: null mutex pointer.");
        return MUTEX_ERR_NULL_PTR;
    }

#if IS_WINDOWS
    CloseHandle((*m)->h_mutex);
#else
    if(pthread_mutex_destroy(&((*m)->p_mutex))) {
        info("WARN: error while attempting to destroy mutex.");
        return MUTEX_ERR_MUTEX_DESTROY;
    }
#endif

    mem_free(*m);

    *m = NULL;

    info("DETAIL: Done.");

    return MUTEX_STATUS_OK;
}
