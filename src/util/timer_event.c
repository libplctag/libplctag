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
#include <platform.h>
#include <util/atomic_int.h>
#include <util/debug.h>
#include <util/timer_event.h>


#define TIMER_LONG_TIME (30000) /* 30 seconds in milliseconds */

static timer_event_callback_p callbacks = NULL;
static mutex_p callback_mutex = NULL;
static condition_var_p timer_signal = NULL;
static thread_p timer_thread = NULL;
static volatile int64_t next_wake_time = 0;
static atomic_int library_terminating = ATOMIC_INT_STATIC_INIT(0);


static THREAD_FUNC(timer_thread_handler);


int timer_event_register_callback(timer_event_callback_p callback)
{
    int rc = PLCTAG_STATUS_OK;
    int need_wakeup = 0;

    pdebug(DEBUG_SPEW, "Starting");

    critical_block(callback_mutex) {
        timer_event_callback_p *callback_walker = &callbacks;

        while(*callback_walker && (*callback_walker)->wake_time < callback->wake_time) {
            callback_walker = &((*callback_walker)->next);
        }

        /* whatever the walker points to is the place to add the new callback. */
        callback->next = *callback_walker;
        *callback_walker = callback;

        /* make sure that we capture the earliest wake time. */
        if(callback->wake_time < next_wake_time) {
            next_wake_time = callback->wake_time;
            need_wakeup = 1;
        }
    }

    /* if we need to wake up the timer thread, do so */
    if(need_wakeup) {
        condition_var_signal(timer_signal);
    }

    pdebug(DEBUG_SPEW, "Done");

    return rc;
}


int timer_event_remove_callback(timer_event_callback_p callback)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    if(callback) {
        critical_block(callback_mutex) {
            timer_event_callback_p *callback_walker = &callbacks;

            while(*callback_walker && *callback_walker != callback) {
                callback_walker = &((*callback_walker)->next);
            }

            /* did we find it? */
            if(*callback_walker == callback) {
                *callback_walker = callback->next;
                callback->next = NULL;
            } else {
                rc = PLCTAG_ERR_NOT_FOUND;
            }
        }
    } else {
        pdebug(DEBUG_WARN, "The passed callback was NULL!");
        rc = PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_INFO, "Done");

    return rc;
}



/* can be called from outside. */
int timer_event_tickler(int64_t current_time)
{
    int rc = PLCTAG_STATUS_OK;
    int done = 1;

    pdebug(DEBUG_SPEW, "Starting");

    /* see if there is anything to do. */
    do {
        timer_event_callback_p callback = NULL;

        critical_block(callback_mutex) {
            callback = callbacks;

            /* anything to do yet? */
            if(callback && callback->wake_time  <= current_time) {
                callbacks = callback->next;
                callback->next = NULL;

                /* peek to see if there is still work to do. */
                if(callbacks) {
                    next_wake_time = callbacks->wake_time;
                } else {
                    next_wake_time = time_ms() + TIMER_LONG_TIME;
                }
            }

            done = (next_wake_time > current_time);
        }

        /* was there an event to fire? */
        if(callback) {
            pdebug(DEBUG_SPEW, "Calling timer callback.");
            callback->timer_event_handler(TIMER_FIRED, current_time, callback->context);
        }
    } while(!done);

    pdebug(DEBUG_SPEW, "Done");

    return rc;
}



int timer_event_init(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    callbacks = NULL;

    next_wake_time = time_ms() + TIMER_LONG_TIME;

    /* create the mutex. */
    rc = mutex_create(&callback_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Mutex creation failed with %s!", plc_tag_decode_error(rc));
        return rc;
    }

    /* create the condition var */
    rc = condition_var_create(&timer_signal);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Condition var creation failed with %s!", plc_tag_decode_error(rc));
        return rc;
    }

    /* start the tickler thread. */
    rc = thread_create(&timer_thread, timer_thread_handler, 32768, NULL);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Timer thread creation failed with %s!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_INFO, "Done");

    return PLCTAG_STATUS_OK;
}


void timer_event_teardown(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    /* first shut down the thread. */
    atomic_int_set(&library_terminating,1);

    /* make sure the thread is not waiting */
    if(timer_signal) {
        rc = condition_var_signal(timer_signal);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Got error %s signalling timer condition variable!", plc_tag_decode_error(rc));
        }
    }

    if(timer_thread) {
        thread_join(timer_thread);
        timer_thread = NULL;
    }

    /* reset to valid state. */
    atomic_int_set(&library_terminating, 0);

    /* destroy the condition var */
    if(timer_signal) {
        rc = condition_var_destroy(&timer_signal);
    }

    /* check the callbacks */
    if(callback_mutex) {
        int done = 1;

        do {
            timer_event_callback_p callback = NULL;

            critical_block(callback_mutex) {
                if(callbacks) {
                    /* unlink the head callback */
                    callback = callbacks;
                    callbacks = callback->next;
                }

                /* any left? if so, keep going. */
                done = !callbacks;
            }

            if(callback) {
                callback->timer_event_handler(TIMER_SHUTDOWN, 0, callback->context);
                callback = NULL;
            }
        } while(!done);

        /* destroy the mutex. */
        rc = mutex_destroy(&callback_mutex);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Mutex destruction failed with %s!", plc_tag_decode_error(rc));
        }
    } else {
        pdebug(DEBUG_WARN, "Found null callback mutex during clean up!");
    }

    pdebug(DEBUG_INFO, "Done");
}



THREAD_FUNC(timer_thread_handler)
{
    int rc = PLCTAG_STATUS_OK;

    (void)arg;

    pdebug(DEBUG_INFO, "Starting.");

    while(!atomic_int_get(&library_terminating)) {
        int64_t current_time = time_ms();

        /*
         * do we care why the condition wait returned?
         *
         * If it is PLCTAG_STATUS_OK or PLCTAG_ERR_TIMEOUT, we are fine.
         * If it is some other error, perhaps we should shut down?
         */
        rc = condition_var_wait(timer_signal, next_wake_time);
        if(rc == PLCTAG_STATUS_OK || rc == PLCTAG_ERR_TIMEOUT) {
                rc = timer_event_tickler(current_time);
                /* FIXME - check return status. */
        } else {
            pdebug(DEBUG_WARN, "Got unexpected error %s from condition_var_wait()!", plc_tag_decode_error(rc));
        }
    }

    pdebug(DEBUG_INFO, "Done");

    THREAD_RETURN((intptr_t)rc);
}