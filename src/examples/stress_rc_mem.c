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


#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <signal.h>
#include "../lib/libplctag.h"
#include "utils.h"
#include <threads.h> 
#include <inttypes.h>  

 
/* 
    The purpose of this program is to stress the ref count system and make sure
    that there are no leaks and no use-after-free problems.
*/



static volatile int terminate = 0;

static void signal_handler(int signum);
static int thread_func(void *arg);



#define NUM_THREADS 10


int main(void)
{   
    thrd_t threads[NUM_THREADS] = {0};
    
    /* Set up the signal handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("Error setting up SIGINT signal handler");
        return 1;
    }

    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("Error setting up SIGTERM signal handler");
        return 1;
    }

    fprintf(stderr, "Signal handlers set up. Press Ctrl+C or send SIGTERM to terminate.\n");

    plc_tag_set_debug_level(PLCTAG_DEBUG_INFO);

    /* create 10 threads to run thread_func() */
    for(int task_id=0; task_id < NUM_THREADS; task_id++) {
        int rc = thrd_create(&threads[task_id], thread_func, (void *)(intptr_t)task_id);
        if(rc != thrd_success) {
            fprintf(stderr, "Error creating thread %d\n", task_id);
            return 1;
        }
    }

    /* wait while we test */
    while(!terminate){
        util_sleep_ms(100);
    }
    
    for(int task_id=0; task_id < NUM_THREADS; task_id++) {
        thrd_join(threads[task_id], NULL);
    }

    return 0;   
}



/* a signal handling function that sets terminate to 1. */
void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        terminate = 1;
        // fprintf(stderr, "Termination signal received. Exiting...\n");
    }
}


/*
    This function opens a tag to the test server and just reads it over and over.
    If any error occurs, it closes the tag handle and opens a new one.
*/

int thread_func(void *arg) {
    int task_id = (int)(intptr_t)arg;
    int rc;
    int32_t tag = 0;
    char tag_str[250] = {0};

    snprintf(tag_str, sizeof(tag_str), "protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=TestTag&connection_group_id=%d", task_id);

    while(!terminate) {
        fprintf(stderr, "Task %d creating tag\n", task_id);

        tag = plc_tag_create(tag_str, 5000);
        if(tag < 0) {
            fprintf(stderr, "Task %d tag creation failed with error %s\n", task_id, plc_tag_decode_error(tag));

            util_sleep_ms(100);
            continue;
        }

        fprintf(stderr, "Task %d reading tag %" PRId32 "\n", task_id, tag);
        
        do {
            rc = plc_tag_read(tag, 5000);
            if(rc == PLCTAG_STATUS_OK) {  
                util_sleep_ms(10);
            } else {
                fprintf(stderr, "Task %d read failed with error %s\n", task_id, plc_tag_decode_error(rc));
            }
        } while(rc == PLCTAG_STATUS_OK && !terminate);

        fprintf(stderr, "Task %d destroying tag %" PRId32 "\n", task_id, tag);
        plc_tag_destroy(tag);

        util_sleep_ms(10);
    }

    return 0;
}



