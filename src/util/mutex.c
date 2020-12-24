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
#include <util/mutex.h>
#include <util/platform.h>

#include <stddef.h>

#ifdef PLATFORM_IS_WINDOWS
    #include <synchapi.h>

    struct mutex_t {
        HANDLE h_mutex;
        int initialized;
    };

#else
    #include <pthread.h>

    struct mutex_t {
        pthread_mutex_t p_mutex;
        pthread_mutexattr_t mutex_attr;
        int initialized;
    };

#endif


int mutex_create(mutex_p *m)
{
    pdebug(DEBUG_DETAIL, "Starting.");

    if(*m) {
        pdebug(DEBUG_WARN, "Called with non-NULL pointer!");
    }

    *m = (struct mutex_t *)mem_alloc(sizeof(struct mutex_t));

    if(! *m) {
        pdebug(DEBUG_ERROR,"null mutex pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

#ifdef PLATFORM_IS_WINDOWS
    /* set up the mutex */
    (*m)->h_mutex = CreateMutex(
                        NULL,                   /* default security attributes  */
                        FALSE,                  /* initially not owned          */
                        NULL);                  /* unnamed mutex                */

    if(!(*m)->h_mutex) {
        mem_free(*m);
        *m = NULL;
        pdebug(DEBUG_WARN, "Error initializing mutex!");
        return PLCTAG_ERR_MUTEX_INIT;
    }
#else
    if(pthread_mutexattr_init(&((*m)->mutex_attr))) {
        pdebug(DEBUG_WARN, "Unable to initialize pthread attributes!");
        return PLCTAG_ERR_MUTEX_INIT;
    }

    if(pthread_mutexattr_settype(&((*m)->mutex_attr), PTHREAD_MUTEX_RECURSIVE)) {
        pdebug(DEBUG_WARN, "Unable to set pthread attributes!");
        return PLCTAG_ERR_MUTEX_INIT;
    }

    if(pthread_mutex_init(&((*m)->p_mutex), &((*m)->mutex_attr))) {
        mem_free(*m);
        *m = NULL;
        pdebug(DEBUG_WARN,"Error initializing mutex.");
        return PLCTAG_ERR_MUTEX_INIT;
    }
#endif

    (*m)->initialized = 1;

    pdebug(DEBUG_DETAIL, "Done creating mutex %p.", *m);

    return PLCTAG_STATUS_OK;
}


int mutex_lock_impl(const char *func, int line, mutex_p m)
{
#ifdef PLATFORM_IS_WINDOWS
    DWORD dwWaitResult = 0;
#endif

    pdebug(DEBUG_SPEW,"locking mutex %p, called from %s:%d.", m, func, line);

    if(!m) {
        pdebug(DEBUG_WARN, "null mutex pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!m->initialized) {
        pdebug(DEBUG_WARN, "Mutex is not initialized!");
        return PLCTAG_ERR_MUTEX_INIT;
    }

#ifdef PLATFORM_IS_WINDOWS
    /* FIXME - can this fail such that we should fall out of this? */

    dwWaitResult = ~WAIT_OBJECT_0;

    /* FIXME - This will potentially hang forever! */
    while(dwWaitResult != WAIT_OBJECT_0) {
        dwWaitResult = WaitForSingleObject(m->h_mutex, INFINITE);
    }
#else
    if(pthread_mutex_lock(&(m->p_mutex))) {
        pdebug(DEBUG_WARN, "error locking mutex.");
        return PLCTAG_ERR_MUTEX_LOCK;
    }
#endif

    pdebug(DEBUG_SPEW,"Done.");

    return PLCTAG_STATUS_OK;
}


int mutex_try_lock_impl(const char *func, int line, mutex_p m)
{
#ifdef PLATFORM_IS_WINDOWS
    DWORD dwWaitResult = 0;
#endif

    pdebug(DEBUG_SPEW,"trying to lock mutex %p, called from %s:%d.", m, func, line);

    if(!m) {
        pdebug(DEBUG_WARN, "null mutex pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!m->initialized) {
        pdebug(DEBUG_WARN, "Mutex is not initialized!");
        return PLCTAG_ERR_MUTEX_INIT;
    }

#ifdef PLATFORM_IS_WINDOWS
    dwWaitResult = WaitForSingleObject(m->h_mutex, 0);
    if(dwWaitResult == WAIT_OBJECT_0) {
        /* we got the lock */
        pdebug(DEBUG_SPEW, "Done.");
        return PLCTAG_STATUS_OK;
    } else {
        pdebug(DEBUG_SPEW, "Done, unable to lock mutex.");
        return PLCTAG_ERR_MUTEX_LOCK;
    }
#else
    if(pthread_mutex_trylock(&(m->p_mutex)) == 0) {
        pdebug(DEBUG_SPEW, "Done.");
        return PLCTAG_STATUS_OK;
    } else {
        pdebug(DEBUG_SPEW, "Done, unable to lock mutex.");
        return PLCTAG_ERR_MUTEX_LOCK;
    }
#endif
}



int mutex_unlock_impl(const char *func, int line, mutex_p m)
{
    pdebug(DEBUG_SPEW,"unlocking mutex %p, called from %s:%d.", m, func, line);

    if(!m) {
        pdebug(DEBUG_WARN,"null mutex pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!m->initialized) {
        pdebug(DEBUG_WARN, "Mutex is not initialized!");
        return PLCTAG_ERR_MUTEX_INIT;
    }

#ifdef PLATFORM_IS_WINDOWS
    if(!ReleaseMutex(m->h_mutex)) {
        pdebug(DEBUG_WARN, "error unlocking mutex.");
        return PLCTAG_ERR_MUTEX_UNLOCK;
    }
#else
    if(pthread_mutex_unlock(&(m->p_mutex))) {
        pdebug(DEBUG_WARN, "error unlocking mutex.");
        return PLCTAG_ERR_MUTEX_UNLOCK;
    }
#endif

    pdebug(DEBUG_SPEW,"Done.");

    return PLCTAG_STATUS_OK;
}


int mutex_destroy(mutex_p *m)
{
    pdebug(DEBUG_DETAIL, "Starting to destroy mutex %p.", m);

    if(!m || !*m) {
        pdebug(DEBUG_WARN, "null mutex pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

#ifdef PLATFORM_IS_WINDOWS
    CloseHandle((*m)->h_mutex);
#else
    if(pthread_mutex_destroy(&((*m)->p_mutex))) {
        pdebug(DEBUG_WARN, "error while attempting to destroy mutex.");
        return PLCTAG_ERR_MUTEX_DESTROY;
    }

    if(pthread_mutexattr_destroy(&((*m)->mutex_attr))) {
        pdebug(DEBUG_WARN, "error while attempting to destroy mutex attributes.");
        return PLCTAG_ERR_MUTEX_DESTROY;
    }
#endif

    mem_free(*m);

    *m = NULL;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}



