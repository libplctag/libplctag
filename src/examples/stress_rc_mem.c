/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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


#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
    The purpose of this program is to stress the ref count system and make sure
    that there are no leaks and no use-after-free problems.
*/


static volatile int terminate = 0;

static void interrupt_handler(void);
static void *thread_func(void *arg);


#define NUM_THREADS 10


int main(void) {
    compat_thread_t threads[NUM_THREADS] = {0};
    int64_t end_time = compat_time_ms() + (10 * 1000);

    /* Set up the signal handler */
    compat_set_interrupt_handler(interrupt_handler);

    // NOLINTNEXTLINE
    fprintf(stderr, "Interrupt handlers set up.\n");
    // NOLINTNEXTLINE
    fprintf(stderr, "Waiting until time %" PRId64 "ms.\n", end_time);
    // NOLINTNEXTLINE
    fprintf(stderr, "Use ^C or send a SIGTERM to terminate early.\n");

    plc_tag_set_debug_level(PLCTAG_DEBUG_WARN);

    /* create 10 threads to run thread_func() */
    for(int task_id = 0; task_id < NUM_THREADS; task_id++) {
        int rc = compat_thread_create(&threads[task_id], thread_func, (void *)(intptr_t)task_id);
        if(rc != 0) {
            // NOLINTNEXTLINE
            fprintf(stderr, "Error creating thread %d\n", task_id);
            return -1;
        }
    }

    /* wait while we test */
    while(!terminate && (end_time > compat_time_ms())) {
        compat_sleep_ms(1000, NULL);
        // NOLINTNEXTLINE
        fprintf(stderr, "Milliseconds remaining %" PRId64 "ms.\n", (end_time - compat_time_ms()));
    }

    terminate = 1;

    for(int task_id = 0; task_id < NUM_THREADS; task_id++) { compat_thread_join(threads[task_id], NULL); }

    return 0;
}


/* a signal handling function that sets terminate to 1. */
void interrupt_handler(void) { terminate = 1; }


/*
    This function opens a tag to the test server and just reads it over and over.
    If any error occurs, it closes the tag handle and opens a new one.
*/

void *thread_func(void *arg) {
    int task_id = (int)(intptr_t)arg;
    int rc;
    int32_t tag = 0;
    char tag_str[250] = {0};

    // NOLINTNEXTLINE
    snprintf(tag_str, sizeof(tag_str),
             "protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=TestBigArray&connection_group_id=%d", task_id);

    while(!terminate) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Task %d creating tag\n", task_id);

        tag = plc_tag_create(tag_str, 5000);
        if(tag < 0) {
            // NOLINTNEXTLINE
            fprintf(stderr, "Task %d tag creation failed with error %s\n", task_id, plc_tag_decode_error(tag));

            compat_sleep_ms(100, NULL);
            continue;
        }

        // NOLINTNEXTLINE
        fprintf(stderr, "Task %d reading tag %" PRId32 "\n", task_id, tag);

        do {
            rc = plc_tag_read(tag, 5000);
            if(rc != PLCTAG_STATUS_OK) {
                // NOLINTNEXTLINE
                fprintf(stderr, "Task %d read failed with error %s\n", task_id, plc_tag_decode_error(rc));
            }
        } while(rc == PLCTAG_STATUS_OK && !terminate);

        // NOLINTNEXTLINE
        fprintf(stderr, "Task %d destroying tag %" PRId32 "\n", task_id, tag);

        plc_tag_destroy(tag);
    }

    return 0;
}
