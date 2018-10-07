/***************************************************************************
 *   Copyright (C) 2015 by OmanTek                                         *
 *   Author Kyle Hayes  kylehayes@omantek.com                              *
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
#include <sys/time.h>
#include "../lib/libplctag.h"
#include "utils.h"

#define TAG_PATH "protocol=ab_eip&gateway=10.206.1.39&path=1,0&cpu=LGX&elem_size=4&elem_count=1&name=TestDINTArray[4]&debug=4"

#define DATA_TIMEOUT 1500



/*
 * This test program creates a lot of threads that read the same tag in
 * the plc.  They all hit the exact same underlying tag data structure.
 * This tests, to some extent, whether the library can handle multi-threaded
 * access.
 */


/* global to cheat on passing it to threads. */
volatile int done = 0;


volatile plc_tag tag = NULL;




static int open_tag(plc_tag *tag, const char *tag_str)
{
    int rc = PLCTAG_STATUS_OK;
    int64_t start_time;

    /* create the tag */
    start_time = util_time_ms();
    *tag = plc_tag_create(tag_str);

    /* everything OK? */
    if(! *tag) {
        fprintf(stderr,"ERROR: Could not create tag!\n");
        return PLCTAG_ERR_CREATE;
    } else {
        fprintf(stderr, "INFO: Tag created with status %s\n", plc_tag_decode_error(plc_tag_status(*tag)));
    }

    /* let the connect succeed we hope */
    while((start_time + 2000) > util_time_ms() && (rc = plc_tag_status(*tag)) == PLCTAG_STATUS_PENDING) {
        util_sleep_ms(10);
    }

    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr,"Error %s setting up tag internal state.\n", plc_tag_decode_error(rc));
        plc_tag_destroy(*tag);
        *tag = (plc_tag)0;
        return rc;
    }

    return rc;
}



void *test_tag(void *data)
{
    int tid = (int)(intptr_t)data;
    int32_t value;
    int64_t start;
    int64_t end;
    int rc = PLCTAG_STATUS_OK;
    int iteration = 1;

    while(!done) {
        /* capture the starting time */
        start = util_time_ms();

        /* read the tag */
        rc = plc_tag_read(tag, DATA_TIMEOUT);

        if(rc != PLCTAG_STATUS_OK) {
            fprintf(stderr,"Test %d, terminating test, read resulted in error %s\n", tid, plc_tag_decode_error(rc));
            done = 1;
        } else {
            value = plc_tag_get_int32(tag,0);

            /* increment the value, keep it in bounds of 0-499 */
            value = (value >= (int32_t)500 ? (int32_t)0 : value + (int32_t)1);

            /* yes, we should be checking this return value too... */
            plc_tag_set_int32(tag, 0, value);

            /* write the value */
            rc = plc_tag_write(tag, DATA_TIMEOUT);

            if(rc != PLCTAG_STATUS_OK) {
                fprintf(stderr,"Test %d, terminating test, write resulted in error %s\n", tid, plc_tag_decode_error(rc));
                done = 1;
            }
        }

        end = util_time_ms();

        fprintf(stderr,"Test %d, iteration %d, got result %d with return code %s in %dms\n",tid, iteration, value, plc_tag_decode_error(rc), (int)(end-start));

        iteration++;
    }

    fprintf(stderr, "Test %d terminating.\n", tid);

    return NULL;
}



#define MAX_THREADS (100)

int main(int argc, char **argv)
{
    pthread_t threads[MAX_THREADS];
    int64_t start_time;
    int64_t end_time;
    int64_t seconds = 30;  /* default 1 hour */
    int num_threads = 0;
    int rc = PLCTAG_STATUS_OK;

    if(argc==2) {
        num_threads = atoi(argv[1]);
    } else {
        fprintf(stderr,"Usage: stress_api_lock <num threads>\n");
        return 0;
    }

    rc = open_tag((plc_tag*)&tag,TAG_PATH);

    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr,"Unable to create tag! rc=(%d) %s\n", rc, plc_tag_decode_error(rc));
        return 1;
    }

    /* create the test threads */
    for(int tid=0; tid < num_threads  && tid < MAX_THREADS; tid++) {
        fprintf(stderr, "Creating serial test thread (Test #%d).\n", tid);
        pthread_create(&threads[tid], NULL, &test_tag, (void *)(intptr_t)tid);
    }

    start_time = util_time_ms();
    end_time = start_time + (seconds * 1000);

    while(!done && util_time_ms() < end_time) {
        util_sleep_ms(100);
    }

    if(done) {
        fprintf(stderr,"Test FAILED!\n");
    } else {
        fprintf(stderr,"Test SUCCEEDED!\n");
    }

    done = 1;

    for(int tid=0; tid < num_threads && tid < MAX_THREADS; tid++) {
        pthread_join(threads[tid], NULL);
    }

    fprintf(stderr, "All test threads terminated.\n");

    plc_tag_destroy(tag);


    return 0;
}

