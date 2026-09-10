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
 * Interruptible sleep.  See utils/nap.h for what this primitive is and is not.
 *
 * All platform-specific code is confined to the static functions at the
 * bottom of this file.  The public functions hold the argument checking,
 * allocation, timeout arithmetic, interrupt handling and logging that both
 * platforms share.
 */

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#else
#    include <errno.h>
#    include <pthread.h>
#    include <time.h>
#endif

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include <libplctag/lib/libplctag.h>
#include <platform.h>
#include <utils/nap.h>
#include <utils/debug.h>


struct nap_t {
#ifdef _WIN32
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cond;
#else
    pthread_mutex_t mutex;
    pthread_cond_t cond;
#endif
    bool interrupted;
};


static int32_t nap_init(struct nap_t *n);
static int32_t nap_lock(struct nap_t *n);
static int32_t nap_unlock(struct nap_t *n);
static int32_t nap_sleep(struct nap_t *n, int64_t time_left_ms);
static int32_t nap_signal(struct nap_t *n);
static void nap_close(struct nap_t *n);


/*
 * Create a new nap.
 */

extern int32_t nap_create(nap_p *n) {
    int32_t rc = PLCTAG_STATUS_OK;
    nap_p tmp_nap = NULL;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Starting.");

    if(!n) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Null pointer to nap pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(*n) { pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Nap pointer is not null, was it not deleted first?"); }

    /* clear the output first. */
    *n = NULL;

    tmp_nap = (struct nap_t *)mem_alloc((int)(unsigned int)sizeof(*tmp_nap));

    if(!tmp_nap) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Unable to allocate new nap!");
        return PLCTAG_ERR_NO_MEM;
    }

    rc = nap_init(tmp_nap);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Unable to initialize nap!");
        mem_free(tmp_nap);
        return rc;
    }

    tmp_nap->interrupted = false;

    *n = tmp_nap;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * Sleep until interrupted or until the timeout expires.
 *
 * A pending interrupt is taken on the way out, so each interrupt releases
 * exactly one waiter.  Timing out is the normal path.
 */

extern int32_t nap_wait_impl(const char *func, int32_t line_num, nap_p n, int32_t timeout_ms) {
    int32_t rc = PLCTAG_STATUS_OK;
    int64_t start_time = time_ms();

    pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Starting. Called from %s:%d.", func, (int)line_num);

    if(!n) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Nap pointer is null in call from %s:%d!", func, (int)line_num);
        return PLCTAG_ERR_NULL_PTR;
    }

    /* FIXME - should this be a BAD_PARAM error instead as it was? */
    if(timeout_ms <= 0) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Timeout must be a positive value but was %d in call from %s:%d!",
               (int)timeout_ms, func, (int)line_num);
        return PLCTAG_ERR_TIMEOUT;
    }

    rc = nap_lock(n);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Unable to lock mutex!");
        return rc;
    }

    while(!n->interrupted) {
        int64_t time_left = (int64_t)timeout_ms - (time_ms() - start_time);

        pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Waiting for %" PRId64 "ms.", time_left);

        if(time_left <= 0) {
            pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Timed out.");
            rc = PLCTAG_ERR_TIMEOUT;
            break;
        }

        rc = nap_sleep(n, time_left);

        if(rc == PLCTAG_ERR_TIMEOUT) {
            pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Nap ran to its timeout.");
            break;
        }

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Error while napping!");
            break;
        }

        /* we might need to wait again.  could be a spurious wake up. */
        pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Nap wait returned.");
    }

    if(n->interrupted) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Nap interrupted for call at %s:%d.", func, (int)line_num);

        /* take the interrupt now that we've responded to it. */
        n->interrupted = false;
    } else {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Nap terminated due to error or timeout for call at %s:%d.", func,
               (int)line_num);
    }

    if(nap_unlock(n) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Unable to unlock mutex!");
        return PLCTAG_ERR_MUTEX_UNLOCK;
    }

    pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Done for call at %s:%d.", func, (int)line_num);

    return rc;
}


/*
 * Post an interrupt and wake one waiter.
 */

extern int32_t nap_interrupt_impl(const char *func, int32_t line_num, nap_p n) {
    int32_t rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Starting.  Called from %s:%d.", func, (int)line_num);

    if(!n) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Nap pointer is null in call at %s:%d!", func, (int)line_num);
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = nap_lock(n);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Unable to lock mutex!");
        return rc;
    }

    n->interrupted = true;

    /*
     * The wake happens with the lock held.  Both platforms allow it and it
     * keeps the pending interrupt and the wake from being separated by a
     * waiter that gets in between them.
     */
    rc = nap_signal(n);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Interrupt of nap failed in call at %s:%d!", func, (int)line_num);
    }

    if(nap_unlock(n) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Unable to unlock mutex!");
        return PLCTAG_ERR_MUTEX_UNLOCK;
    }

    pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Done. Called from %s:%d.", func, (int)line_num);

    return rc;
}


/*
 * Discard a pending interrupt without waiting.
 */

extern int32_t nap_clear_impl(const char *func, int32_t line_num, nap_p n) {
    int32_t rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Starting.  Called from %s:%d.", func, (int)line_num);

    if(!n) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Nap pointer is null in call at %s:%d!", func, (int)line_num);
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = nap_lock(n);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Unable to lock mutex!");
        return rc;
    }

    n->interrupted = false;

    if(nap_unlock(n) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Unable to unlock mutex!");
        return PLCTAG_ERR_MUTEX_UNLOCK;
    }

    pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Done. Called from %s:%d.", func, (int)line_num);

    return rc;
}


/*
 * Destroy the nap and free the handle.
 */

extern int32_t nap_destroy(nap_p *n) {
    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Starting.");

    if(!n || !*n) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Nap pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    nap_close(*n);

    mem_free(*n);

    *n = NULL;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/***************************************************************************
 ************************* Platform-specific code **************************
 **************************************************************************/

#ifdef _WIN32

static int32_t nap_init(struct nap_t *n) {
    InitializeCriticalSection(&(n->cs));
    InitializeConditionVariable(&(n->cond));

    return PLCTAG_STATUS_OK;
}


static int32_t nap_lock(struct nap_t *n) {
    EnterCriticalSection(&(n->cs));

    return PLCTAG_STATUS_OK;
}


static int32_t nap_unlock(struct nap_t *n) {
    LeaveCriticalSection(&(n->cs));

    return PLCTAG_STATUS_OK;
}


static int32_t nap_sleep(struct nap_t *n, int64_t time_left_ms) {
    if(SleepConditionVariableCS(&(n->cond), &(n->cs), (DWORD)time_left_ms)) { return PLCTAG_STATUS_OK; }

    /* error or timeout. */
    if(GetLastError() == ERROR_TIMEOUT) { return PLCTAG_ERR_TIMEOUT; }

    return PLCTAG_ERR_BAD_STATUS;
}


static int32_t nap_signal(struct nap_t *n) {
    WakeConditionVariable(&(n->cond));

    return PLCTAG_STATUS_OK;
}


static void nap_close(struct nap_t *n) {
    /*
     * NOTE: the old platform code freed the handle without ever deleting the
     * critical section, leaking its wait resources.
     */
    DeleteCriticalSection(&(n->cs));
}

#else

static int32_t nap_init(struct nap_t *n) {
    if(pthread_mutex_init(&(n->mutex), NULL)) { return PLCTAG_ERR_CREATE; }

    if(pthread_cond_init(&(n->cond), NULL)) {
        pthread_mutex_destroy(&(n->mutex));
        return PLCTAG_ERR_CREATE;
    }

    return PLCTAG_STATUS_OK;
}


static int32_t nap_lock(struct nap_t *n) {
    if(pthread_mutex_lock(&(n->mutex))) { return PLCTAG_ERR_MUTEX_LOCK; }

    return PLCTAG_STATUS_OK;
}


static int32_t nap_unlock(struct nap_t *n) {
    if(pthread_mutex_unlock(&(n->mutex))) { return PLCTAG_ERR_MUTEX_UNLOCK; }

    return PLCTAG_STATUS_OK;
}


static int32_t nap_sleep(struct nap_t *n, int64_t time_left_ms) {
    struct timespec timeout;
    int wait_rc = 0;

    /*
     * NOTE: pthread_cond_timedwait() takes an _ABSOLUTE_ time, not a relative
     * delay.  time_ms() is gettimeofday()-based, the same clock the default
     * condition variable attributes use.
     */
    int64_t end_time = time_ms() + time_left_ms;

    timeout.tv_sec = (time_t)(end_time / 1000);
    timeout.tv_nsec = (long)1000000 * (long)(end_time % 1000);

    wait_rc = pthread_cond_timedwait(&(n->cond), &(n->mutex), &timeout);

    if(wait_rc == 0) { return PLCTAG_STATUS_OK; }

    if(wait_rc == ETIMEDOUT) { return PLCTAG_ERR_TIMEOUT; }

    /*
     * pthread_cond_timedwait() returns the error directly and never touches
     * errno, so wait_rc is the only meaningful value to report here.
     */
    pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Error %d waiting on nap condition variable!", wait_rc);

    return PLCTAG_ERR_BAD_STATUS;
}


static int32_t nap_signal(struct nap_t *n) {
    /*
     * pthread_cond_signal() returns the error directly and never touches
     * errno, so capture the return value rather than reporting an unrelated
     * stale errno.
     */
    int signal_rc = pthread_cond_signal(&(n->cond));

    if(signal_rc) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Signal of nap condition variable returned error %d!", signal_rc);
        return PLCTAG_ERR_BAD_STATUS;
    }

    return PLCTAG_STATUS_OK;
}


static void nap_close(struct nap_t *n) {
    pthread_cond_destroy(&(n->cond));
    pthread_mutex_destroy(&(n->mutex));
}

#endif
