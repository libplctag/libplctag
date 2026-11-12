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
#    include <synchapi.h>
#else
#    include <pthread.h>
#endif

#include <stdlib.h>
#include <libplctag/api/libplctag.h>
#include "mutex.h"
#include "log.h"


struct mutex_t {
#if IS_WINDOWS
    HANDLE h_mutex;
#else
    pthread_mutex_t p_mutex;
#endif
};

int32_t mutex_create(mutex_p *m) {
#if !defined(IS_WINDOWS)
    pthread_mutexattr_t mutex_attribs;
#endif

    log_detail("Starting.");

    if(*m) { log_warn("Called with non-NULL pointer!"); }

    *m = (struct mutex_t *)calloc(1, sizeof(struct mutex_t));

    if(!*m) {
        log_error("Unable to allocate mutex.");
        return PLCTAG_ERR_NULL_PTR;
    }

#if IS_WINDOWS
    (*m)->h_mutex = CreateMutex(NULL,  /* default security attributes  */
                                FALSE, /* initially not owned          */
                                NULL); /* unnamed mutex                */

    if(!(*m)->h_mutex) {
#else
    /* set up for recursive locking. */
    pthread_mutexattr_init(&mutex_attribs);
    pthread_mutexattr_settype(&mutex_attribs, PTHREAD_MUTEX_RECURSIVE);
    int32_t rc = pthread_mutex_init(&((*m)->p_mutex), &mutex_attribs);
    pthread_mutexattr_destroy(&mutex_attribs);

    if(rc) {
#endif
        free(*m);
        *m = NULL;
        log_error("Error initializing mutex.");
        return PLCTAG_ERR_MUTEX_INIT;
    }

    log_detail("Done creating mutex %p.", *m);

    return PLCTAG_STATUS_OK;
}


int32_t mutex_lock_impl(const char *func, int32_t line, mutex_p m) {
    log_spew("Locking mutex %p, called from %s:%d.", m, func, line);

    if(!m) {
        log_warn("Null mutex pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

#if IS_WINDOWS
    if(WaitForSingleObject(m->h_mutex, INFINITE) != WAIT_OBJECT_0) {
#else
    if(pthread_mutex_lock(&(m->p_mutex))) {
#endif
        log_warn("Error locking mutex.");
        return PLCTAG_ERR_MUTEX_LOCK;
    }

    return PLCTAG_STATUS_OK;
}


int32_t mutex_unlock_impl(const char *func, int32_t line, mutex_p m) {
    log_spew("Unlocking mutex %p, called from %s:%d.", m, func, line);

    if(!m) {
        log_warn("Null mutex pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

#if IS_WINDOWS
    if(!ReleaseMutex(m->h_mutex)) {
#else
    if(pthread_mutex_unlock(&(m->p_mutex))) {
#endif
        log_warn("Error unlocking mutex.");
        return PLCTAG_ERR_MUTEX_UNLOCK;
    }

    return PLCTAG_STATUS_OK;
}
