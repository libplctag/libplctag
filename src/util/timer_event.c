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

#include <stddef.h>
#include <lib/libplctag.h>
#include <util/debug.h>
#include <util/event_loop.h>
#include <util/mem.h>
#include <util/mutex.h>
#include <util/timer_event.h>


struct timer_s {
    struct timer_s *next;
    void *context;
    void (*callback)(timer_p timer,
                     int64_t wake_time,
                     int64_t current_time,
                     void *context);
    int64_t wake_time;
};


static timer_p timer_list = NULL;
static mutex_p timer_mutex = NULL;


// static void timer_rc_destroy(void *timer_arg);

int timer_event_create(timer_p *timer)
{
    pdebug(DEBUG_DETAIL, "Starting.");

    if(!timer) {
        pdebug(DEBUG_WARN, "null timer pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    *timer = (timer_p)mem_alloc(sizeof(**timer));
    if(! *timer) {
        pdebug(DEBUG_ERROR, "Failed to allocate memory for timer.");
        return PLCTAG_ERR_NO_MEM;
    }

    (*timer)->wake_time = INT64_MAX;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


int timer_event_wake_at(timer_p timer,
                  int64_t wake_time,
                  void (*callback)(timer_p timer,
                                   int64_t wake_time,
                                   int64_t current_time,
                                   void *context),
                  void *context)
{
    int rc = PLCTAG_STATUS_OK;
    bool need_wake_up = FALSE;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!timer) {
        pdebug(DEBUG_WARN, "null timer pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!callback) {
        pdebug(DEBUG_WARN, "Callback pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* add to the list of active timers. */
    critical_block(timer_mutex) {
        timer_p *timer_walker = &timer_list;

        timer->wake_time = wake_time;
        timer->context = context;
        timer->callback = callback;

        while(*timer_walker && (*timer_walker)->wake_time < wake_time) {
            timer_walker = &((*timer_walker)->next);
        }

        timer->next = *timer_walker;
        *timer_walker = timer;

        /* do we have a new wake up time? */
        if(timer_list == timer) {
            need_wake_up = TRUE;
        }
    }

    if(need_wake_up) {
        event_loop_wake();
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int timer_event_snooze(timer_p timer)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!timer) {
        pdebug(DEBUG_WARN, "null timer pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* remove from the list of active timers. */
    critical_block(timer_mutex) {
        timer_p *timer_walker = &timer_list;

        while(*timer_walker && *timer_walker != timer) {
            timer_walker = &((*timer_walker)->next);
        }

        if(*timer_walker && *timer_walker == timer) {
            *timer_walker = timer->next;
            timer->next = NULL;
        } else {
            rc = PLCTAG_ERR_NOT_FOUND;
        }
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


/* timer helpers */

int timer_destroy(timer_p *timer)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    if(!timer) {
        pdebug(DEBUG_WARN, "Pointer to timer location is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(! *timer) {
        pdebug(DEBUG_WARN, "Pointer to timer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* find and remove the timer. */
    critical_block(timer_mutex) {
        timer_p *timer_walker = &timer_list;

        while(*timer_walker && *timer_walker != *timer) {
            timer_walker = &((*timer_walker)->next);
        }

        /* unlink from the list. */
        if(*timer_walker && *timer_walker == *timer) {
            *timer_walker = (*timer)->next;
            *timer = NULL;
        } else {
            rc = PLCTAG_ERR_NOT_FOUND;
        }
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}





int64_t timer_event_tickler(int64_t current_time)
{
    int64_t next_wake_time = INT64_MAX;
    int done = 1;

    pdebug(DEBUG_DETAIL, "Starting.");

    do {
        timer_p timer = NULL;

        critical_block(timer_mutex) {
            if(timer_list && timer_list->wake_time <= current_time) {
                timer = rc_inc(timer_list);

                /* pop off the list head. */
                timer_list = timer_list->next;

                next_wake_time = (timer_list ? timer_list->wake_time : INT64_MAX);

                /* keep trying */
                done = 0;
            } else {
                timer = NULL;
                done = 1;
            }
        }

        if(timer) {
            /* call the callback.  This may re-enable the timer. */
            timer->callback(timer, timer->wake_time, current_time, timer->context);

            /* release our reference */
            timer = rc_dec(timer);
        }
    } while(!done);

    pdebug(DEBUG_SPEW, "Done.");

    return next_wake_time;
}




int timer_event_init(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    timer_list = NULL;

    rc = mutex_create(&timer_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create timer mutex, got error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


void timer_event_teardown(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* FIXME leak away! */
    timer_list = NULL;

    rc = mutex_destroy(&timer_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to destroy timer mutex, got error %s!", plc_tag_decode_error(rc));
    }

    pdebug(DEBUG_DETAIL, "Done.");
}


