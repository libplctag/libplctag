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
 * Portable condition variables.  See utils/condvar.h for what this primitive
 * is and is not.
 *
 * All platform-specific code is confined to the static functions at the
 * bottom of this file.  The public functions hold the argument checking,
 * allocation, timeout arithmetic, flag handling and logging that both
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
#include <utils/condvar.h>
#include <utils/debug.h>


struct cond_t {
#ifdef _WIN32
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cond;
#else
    pthread_mutex_t mutex;
    pthread_cond_t cond;
#endif
    bool flag;
};


static int32_t condvar_init(struct cond_t *c);
static int32_t condvar_lock(struct cond_t *c);
static int32_t condvar_unlock(struct cond_t *c);
static int32_t condvar_sleep(struct cond_t *c, int64_t time_left_ms);
static int32_t condvar_wake(struct cond_t *c);
static void condvar_close(struct cond_t *c);


/*
 * Create a new condition variable.
 */

extern int32_t cond_create(cond_p *c) {
    int32_t rc = PLCTAG_STATUS_OK;
    cond_p tmp_cond = NULL;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Starting.");

    if(!c) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Null pointer to condition var pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(*c) { pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Condition var pointer is not null, was it not deleted first?"); }

    /* clear the output first. */
    *c = NULL;

    tmp_cond = (struct cond_t *)mem_alloc((int)(unsigned int)sizeof(*tmp_cond));

    if(!tmp_cond) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Unable to allocate new condition var!");
        return PLCTAG_ERR_NO_MEM;
    }

    rc = condvar_init(tmp_cond);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Unable to initialize condition var!");
        mem_free(tmp_cond);
        return rc;
    }

    tmp_cond->flag = false;

    *c = tmp_cond;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * Wait for the condition var to be signaled or for the timeout to expire.
 *
 * The flag is consumed on the way out, so each signal releases exactly one
 * waiter.
 */

extern int32_t cond_wait_impl(const char *func, int32_t line_num, cond_p c, int32_t timeout_ms) {
    int32_t rc = PLCTAG_STATUS_OK;
    int64_t start_time = time_ms();

    pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Starting. Called from %s:%d.", func, (int)line_num);

    if(!c) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Condition var pointer is null in call from %s:%d!", func, (int)line_num);
        return PLCTAG_ERR_NULL_PTR;
    }

    /* FIXME - should this be a BAD_PARAM error instead as it was? */
    if(timeout_ms <= 0) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Timeout must be a positive value but was %d in call from %s:%d!",
               (int)timeout_ms, func, (int)line_num);
        return PLCTAG_ERR_TIMEOUT;
    }

    rc = condvar_lock(c);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Unable to lock mutex!");
        return rc;
    }

    while(!c->flag) {
        int64_t time_left = (int64_t)timeout_ms - (time_ms() - start_time);

        pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Waiting for %" PRId64 "ms.", time_left);

        if(time_left <= 0) {
            pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Timed out.");
            rc = PLCTAG_ERR_TIMEOUT;
            break;
        }

        rc = condvar_sleep(c, time_left);

        if(rc == PLCTAG_ERR_TIMEOUT) {
            pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Timeout response from condition var wait.");
            break;
        }

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Error waiting on condition variable!");
            break;
        }

        /* we might need to wait again.  could be a spurious wake up. */
        pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Condition var wait returned.");
    }

    if(c->flag) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Condition var signaled for call at %s:%d.", func, (int)line_num);

        /* clear the flag now that we've responded. */
        c->flag = false;
    } else {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Condition wait terminated due to error or timeout for call at %s:%d.", func,
               (int)line_num);
    }

    if(condvar_unlock(c) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Unable to unlock mutex!");
        return PLCTAG_ERR_MUTEX_UNLOCK;
    }

    pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Done for call at %s:%d.", func, (int)line_num);

    return rc;
}


/*
 * Raise the flag and wake one waiter.
 */

extern int32_t cond_signal_impl(const char *func, int32_t line_num, cond_p c) {
    int32_t rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Starting.  Called from %s:%d.", func, (int)line_num);

    if(!c) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Condition var pointer is null in call at %s:%d!", func, (int)line_num);
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = condvar_lock(c);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Unable to lock mutex!");
        return rc;
    }

    c->flag = true;

    /*
     * The wake happens with the lock held.  Both platforms allow it and it
     * keeps the flag and the wake from being separated by a waiter that gets
     * in between them.
     */
    rc = condvar_wake(c);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Signal of condition var failed in call at %s:%d!", func, (int)line_num);
    }

    if(condvar_unlock(c) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Unable to unlock mutex!");
        return PLCTAG_ERR_MUTEX_UNLOCK;
    }

    pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Done. Called from %s:%d.", func, (int)line_num);

    return rc;
}


/*
 * Drop the flag without waiting.
 */

extern int32_t cond_clear_impl(const char *func, int32_t line_num, cond_p c) {
    int32_t rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Starting.  Called from %s:%d.", func, (int)line_num);

    if(!c) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Condition var pointer is null in call at %s:%d!", func, (int)line_num);
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = condvar_lock(c);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Unable to lock mutex!");
        return rc;
    }

    c->flag = false;

    if(condvar_unlock(c) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Unable to unlock mutex!");
        return PLCTAG_ERR_MUTEX_UNLOCK;
    }

    pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Done. Called from %s:%d.", func, (int)line_num);

    return rc;
}


/*
 * Destroy the condition var and free the handle.
 */

extern int32_t cond_destroy(cond_p *c) {
    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Starting.");

    if(!c || !*c) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Condition var pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    condvar_close(*c);

    mem_free(*c);

    *c = NULL;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/***************************************************************************
 ************************* Platform-specific code **************************
 **************************************************************************/

#ifdef _WIN32

static int32_t condvar_init(struct cond_t *c) {
    InitializeCriticalSection(&(c->cs));
    InitializeConditionVariable(&(c->cond));

    return PLCTAG_STATUS_OK;
}


static int32_t condvar_lock(struct cond_t *c) {
    EnterCriticalSection(&(c->cs));

    return PLCTAG_STATUS_OK;
}


static int32_t condvar_unlock(struct cond_t *c) {
    LeaveCriticalSection(&(c->cs));

    return PLCTAG_STATUS_OK;
}


static int32_t condvar_sleep(struct cond_t *c, int64_t time_left_ms) {
    if(SleepConditionVariableCS(&(c->cond), &(c->cs), (DWORD)time_left_ms)) { return PLCTAG_STATUS_OK; }

    /* error or timeout. */
    if(GetLastError() == ERROR_TIMEOUT) { return PLCTAG_ERR_TIMEOUT; }

    return PLCTAG_ERR_BAD_STATUS;
}


static int32_t condvar_wake(struct cond_t *c) {
    WakeConditionVariable(&(c->cond));

    return PLCTAG_STATUS_OK;
}


static void condvar_close(struct cond_t *c) {
    /*
     * NOTE: the old platform code freed the handle without ever deleting the
     * critical section, leaking its wait resources.
     */
    DeleteCriticalSection(&(c->cs));
}

#else

static int32_t condvar_init(struct cond_t *c) {
    if(pthread_mutex_init(&(c->mutex), NULL)) { return PLCTAG_ERR_CREATE; }

    if(pthread_cond_init(&(c->cond), NULL)) {
        pthread_mutex_destroy(&(c->mutex));
        return PLCTAG_ERR_CREATE;
    }

    return PLCTAG_STATUS_OK;
}


static int32_t condvar_lock(struct cond_t *c) {
    if(pthread_mutex_lock(&(c->mutex))) { return PLCTAG_ERR_MUTEX_LOCK; }

    return PLCTAG_STATUS_OK;
}


static int32_t condvar_unlock(struct cond_t *c) {
    if(pthread_mutex_unlock(&(c->mutex))) { return PLCTAG_ERR_MUTEX_UNLOCK; }

    return PLCTAG_STATUS_OK;
}


static int32_t condvar_sleep(struct cond_t *c, int64_t time_left_ms) {
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

    wait_rc = pthread_cond_timedwait(&(c->cond), &(c->mutex), &timeout);

    if(wait_rc == 0) { return PLCTAG_STATUS_OK; }

    if(wait_rc == ETIMEDOUT) { return PLCTAG_ERR_TIMEOUT; }

    /*
     * pthread_cond_timedwait() returns the error directly and never touches
     * errno, so wait_rc is the only meaningful value to report here.
     */
    pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Error %d waiting on condition variable!", wait_rc);

    return PLCTAG_ERR_BAD_STATUS;
}


static int32_t condvar_wake(struct cond_t *c) {
    /*
     * pthread_cond_signal() returns the error directly and never touches
     * errno, so capture the return value rather than reporting an unrelated
     * stale errno.
     */
    int signal_rc = pthread_cond_signal(&(c->cond));

    if(signal_rc) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Signal of condition var returned error %d!", signal_rc);
        return PLCTAG_ERR_BAD_STATUS;
    }

    return PLCTAG_STATUS_OK;
}


static void condvar_close(struct cond_t *c) {
    pthread_cond_destroy(&(c->cond));
    pthread_mutex_destroy(&(c->mutex));
}

#endif
