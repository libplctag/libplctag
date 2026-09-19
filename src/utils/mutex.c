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
 * Portable recursive mutexes.
 *
 * All platform-specific code is confined to the static functions at the
 * bottom of this file.  The public functions hold the argument checking,
 * allocation and logging that both platforms share.
 */

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#else
#    include <pthread.h>
#endif

#include <stdbool.h>
#include <stdint.h>

#include <libplctag/lib/libplctag.h>
#include <utils/mem.h>
#include <utils/debug.h>
#include <utils/mutex.h>


struct mutex_t {
#ifdef _WIN32
    HANDLE h_mutex;
#else
    pthread_mutex_t p_mutex;
#endif
    bool initialized;
};


static int32_t mutex_init(struct mutex_t *m);
static int32_t mutex_acquire(struct mutex_t *m);
static int32_t mutex_acquire_try(struct mutex_t *m);
static int32_t mutex_release(struct mutex_t *m);
static void mutex_close(struct mutex_t *m);


/*
 * Create a new recursive mutex.
 */

extern int32_t mutex_create(mutex_p *m) {
    int32_t rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Starting.");

    if(!m) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Null mutex pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(*m) { pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Called with non-NULL pointer!"); }

    *m = (struct mutex_t *)mem_alloc(sizeof(struct mutex_t));

    if(!*m) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_ERROR, 0, "Unable to allocate mutex!");
        return PLCTAG_ERR_NO_MEM;
    }

    rc = mutex_init(*m);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_ERROR, 0, "Error initializing mutex!");
        mem_free(*m);
        *m = NULL;
        return rc;
    }

    (*m)->initialized = true;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Done creating mutex %p.", (void *)*m);

    return PLCTAG_STATUS_OK;
}


/*
 * Lock the mutex, waiting as long as necessary.
 */

extern int32_t mutex_lock_impl(const char *func, int32_t line_num, mutex_p m) {
    pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "locking mutex %p, called from %s:%d.", (void *)m, func, (int)line_num);

    if(!m) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Null mutex pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!m->initialized) { return PLCTAG_ERR_MUTEX_INIT; }

    if(mutex_acquire(m) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Error locking mutex!");
        return PLCTAG_ERR_MUTEX_LOCK;
    }

    return PLCTAG_STATUS_OK;
}


/*
 * Lock the mutex if it is free.  Returns PLCTAG_ERR_MUTEX_LOCK immediately if
 * another thread holds it.
 */

extern int32_t mutex_try_lock_impl(const char *func, int32_t line_num, mutex_p m) {
    pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "trying to lock mutex %p, called from %s:%d.", (void *)m, func, (int)line_num);

    if(!m) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Null mutex pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!m->initialized) { return PLCTAG_ERR_MUTEX_INIT; }

    if(mutex_acquire_try(m) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Mutex is already locked.");
        return PLCTAG_ERR_MUTEX_LOCK;
    }

    return PLCTAG_STATUS_OK;
}


/*
 * Unlock the mutex.
 */

extern int32_t mutex_unlock_impl(const char *func, int32_t line_num, mutex_p m) {
    pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "unlocking mutex %p, called from %s:%d.", (void *)m, func, (int)line_num);

    if(!m) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Null mutex pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!m->initialized) { return PLCTAG_ERR_MUTEX_INIT; }

    if(mutex_release(m) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Error unlocking mutex!");
        return PLCTAG_ERR_MUTEX_UNLOCK;
    }

    return PLCTAG_STATUS_OK;
}


/*
 * Destroy the mutex and free the handle.
 */

extern int32_t mutex_destroy(mutex_p *m) {
    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Starting to destroy mutex %p.", (void *)m);

    if(!m || !*m) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Null mutex pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    mutex_close(*m);

    mem_free(*m);

    *m = NULL;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/***************************************************************************
 ************************* Platform-specific code **************************
 **************************************************************************/

#ifdef _WIN32

static int32_t mutex_init(struct mutex_t *m) {
    /* Win32 mutex objects are recursive for the owning thread. */
    m->h_mutex = CreateMutex(NULL,  /* default security attributes  */
                             FALSE, /* initially not owned          */
                             NULL); /* unnamed mutex                */

    if(!m->h_mutex) { return PLCTAG_ERR_MUTEX_INIT; }

    return PLCTAG_STATUS_OK;
}


static int32_t mutex_acquire(struct mutex_t *m) {
    DWORD wait_result = ~WAIT_OBJECT_0;

    /* FIXME - This will potentially hang forever! */
    while(wait_result != WAIT_OBJECT_0) { wait_result = WaitForSingleObject(m->h_mutex, INFINITE); }

    return PLCTAG_STATUS_OK;
}


static int32_t mutex_acquire_try(struct mutex_t *m) {
    if(WaitForSingleObject(m->h_mutex, 0) == WAIT_OBJECT_0) { return PLCTAG_STATUS_OK; }

    return PLCTAG_ERR_MUTEX_LOCK;
}


static int32_t mutex_release(struct mutex_t *m) {
    if(!ReleaseMutex(m->h_mutex)) { return PLCTAG_ERR_MUTEX_UNLOCK; }

    return PLCTAG_STATUS_OK;
}


static void mutex_close(struct mutex_t *m) { CloseHandle(m->h_mutex); }

#else

static int32_t mutex_init(struct mutex_t *m) {
    pthread_mutexattr_t mutex_attribs;

    /* set up for recursive locking. */
    pthread_mutexattr_init(&mutex_attribs);
    pthread_mutexattr_settype(&mutex_attribs, PTHREAD_MUTEX_RECURSIVE);

    if(pthread_mutex_init(&(m->p_mutex), &mutex_attribs)) {
        pthread_mutexattr_destroy(&mutex_attribs);
        return PLCTAG_ERR_MUTEX_INIT;
    }

    pthread_mutexattr_destroy(&mutex_attribs);

    return PLCTAG_STATUS_OK;
}


static int32_t mutex_acquire(struct mutex_t *m) {
    if(pthread_mutex_lock(&(m->p_mutex))) { return PLCTAG_ERR_MUTEX_LOCK; }

    return PLCTAG_STATUS_OK;
}


static int32_t mutex_acquire_try(struct mutex_t *m) {
    if(pthread_mutex_trylock(&(m->p_mutex))) { return PLCTAG_ERR_MUTEX_LOCK; }

    return PLCTAG_STATUS_OK;
}


static int32_t mutex_release(struct mutex_t *m) {
    if(pthread_mutex_unlock(&(m->p_mutex))) { return PLCTAG_ERR_MUTEX_UNLOCK; }

    return PLCTAG_STATUS_OK;
}


static void mutex_close(struct mutex_t *m) { pthread_mutex_destroy(&(m->p_mutex)); }

#endif
