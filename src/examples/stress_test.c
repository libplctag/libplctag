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
#include <signal.h>
#include "../lib/libplctag.h"
#include "utils.h"

#define TAG_PATH "protocol=ab_eip&gateway=10.206.1.39&path=1,0&cpu=LGX&elem_size=4&elem_count=1&name=TestDINTArray[%d]&debug=3"

#define DATA_TIMEOUT 2000
#define TAG_CREATE_TIMEOUT 5000



/*
 * This test program creates a lot of threads that read the same tag in
 * the plc.  They all hit the exact same underlying tag data structure.
 * This tests, to some extent, whether the library can handle multi-threaded
 * access.
 */



typedef struct {
    int tid;
    int num_elems;
} thread_args;


/* global to cheat on passing it to threads. */
volatile int done = 0;


static FILE *open_log(int tid)
{
    char buf[60] = {0,};

    snprintf(buf, sizeof(buf), "test-%d.log", tid);

    return fopen(buf,"w");
}

static void close_log(FILE *log)
{
    fflush(log);
    fclose(log);
}


static int32_t open_tag(FILE *log, int tid, int num_elems)
{
    int rc = PLCTAG_STATUS_OK;
    static const char *tag_str = "protocol=ab_eip&gateway=10.206.1.40&path=1,4&cpu=lgx&elem_size=4&elem_count=%d&name=TestBigArray[%d]&debug=4";
    char buf[250] = {0,};
    int32_t tag = 0;

    snprintf(buf, sizeof(buf), tag_str, num_elems, (tid-1)*num_elems);

    fprintf(log,"--- Test %d, Creating tag (%d, %d) with string %s\n", tid, tid, num_elems, buf);

    /* create the tag */
    tag = plc_tag_create(buf, TAG_CREATE_TIMEOUT);

    /* everything OK? */
    if(tag < 0) {
        fprintf(log,"!!! Test %d, could not create tag. error %s!\n", tid, plc_tag_decode_error(tag));
        return tag;
    }

    if((rc = plc_tag_status(tag)) != PLCTAG_STATUS_OK) {
        fprintf(log,"!!! Test %d, error %s setting up tag internal state.\n", tid, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return rc;
    }

    return tag;
}


static void *test_cip(void *data)
{
    thread_args *args = (thread_args *)data;
    int tid = args->tid;
    int num_elems = args->num_elems;
    int32_t value = 0;
    int64_t total_io_time = 0;
    int32_t tag = 0;
    int rc = PLCTAG_STATUS_OK;
    int iteration = 1;
    int first_time = 1;
    FILE *log = NULL;
    int start_index = (tid-1)*num_elems;

    /* a hack to allow threads to start. */
    util_sleep_ms(tid);

    log = open_log(tid);

    fprintf(stderr, "--- Test %d updating %d elements starting at index %d.\n", tid, num_elems, start_index);
    fprintf(log, "--- Test %d updating %d elements starting at index %d.\n", tid, num_elems, start_index);

    while(!done) {
        int64_t start = 0;
        int64_t end = 0;

        while(!tag && !done) {
            if(!first_time) {
                int64_t timeout = 0;

                /* retry later */
                timeout = util_time_ms() + TAG_CREATE_TIMEOUT;
                while(!done && timeout > util_time_ms()) {
                    util_sleep_ms(5);
                }
            }

            first_time = 0;

            tag = open_tag(log, tid, num_elems);
            if(tag < 0) {
                fprintf(log,"!!! Test %d, iteration %d, Error (%s) creating tag!  Retrying in %dms.", tid, iteration, plc_tag_decode_error(tag), TAG_CREATE_TIMEOUT);
            }
        }

        /* capture the starting time */
        start = util_time_ms();

        do {
            rc = plc_tag_read(tag, DATA_TIMEOUT);
            if(rc != PLCTAG_STATUS_OK) {
                fprintf(log, "!!! Test %d, iteration %d, read failed with error %s\n", tid, iteration, plc_tag_decode_error(rc));
                break;
            }

            for(int i=0; i < num_elems; i++) {
                value = plc_tag_get_int32(tag,i*4);

                /* increment the value, keep it in bounds of 0-499 */
                value = (value >= (int32_t)500 ? (int32_t)0 : value + (int32_t)1);

                /* yes, we should be checking this return value too... */
                plc_tag_set_int32(tag, i*4, value);
            }

            /* write the value */
            rc = plc_tag_write(tag, DATA_TIMEOUT);
            if(rc != PLCTAG_STATUS_OK) {
                fprintf(log, "!!! Test %d, iteration %d, write failed with error %s\n", tid, iteration, plc_tag_decode_error(rc));
                break;
            }
        } while(0);

        end = util_time_ms();

        total_io_time += (end - start);

        if(rc != PLCTAG_STATUS_OK) {
            fprintf(log,"!!! Test %d, iteration %d, closing tag due to error %s, will retry in %dms.\n", tid, iteration, plc_tag_decode_error(rc), TAG_CREATE_TIMEOUT);
            plc_tag_destroy(tag);
            tag = 0;
        } else {
            fprintf(log, "*** Test %d, iteration %d updated %d elements in %dms.\n", tid, iteration, num_elems, (int)(end-start));
            util_sleep_ms(1);
        }

        iteration++;
    }

    plc_tag_destroy(tag);

    fprintf(log, "*** Test %d terminating after %d iterations and an average of %dms per iteration.\n", tid, iteration, (int)(total_io_time/iteration));

    close_log(log);

    return NULL;
}



void sigpipe_handler(int unused)
{
    (void)unused;
    done = 1;
}



#define MAX_THREADS (100)

int main(int argc, char **argv)
{
    pthread_t threads[MAX_THREADS];
    int64_t start_time;
    int64_t end_time;
    int num_threads = 0;
    int64_t seconds = 0;
    int num_elems = 0;
    int success = 0;
    thread_args args[MAX_THREADS];
    struct sigaction sigpipe = {0,};

    sigpipe.sa_handler = sigpipe_handler;

    /* catch broken pipe signals */
    sigaction(SIGPIPE, &sigpipe, NULL);

    if(argc == 4) {
        num_threads = atoi(argv[1]);
        num_elems = atoi(argv[2]);
        seconds = atoi(argv[3]);
    } else {
        fprintf(stderr,"Usage: stress_test <num threads> <elements per thread> <seconds to run>\n");
        return 1;
    }

    if(num_threads > MAX_THREADS) {
        fprintf(stderr, "Too many threads.  A maximum of %d threads are supported.\n", MAX_THREADS);
        return 1;
    }

    if(num_threads * num_elems > 1000) {
        fprintf(stderr, "#threads * #elems must be less than 1000.\n");
        return 1;
    }

    fprintf(stderr, "--- starting run with %d threads each handling %d elements running for %ld seconds\n", num_threads, num_elems, seconds);

    /* create the test threads */
    for(int tid=0; tid < num_threads  && tid < MAX_THREADS; tid++) {
        args[tid].tid = num_threads - tid;
        args[tid].num_elems = num_elems;

        fprintf(stderr, "--- Creating serial test thread %d with %d elements.\n", args[tid].tid, args[tid].num_elems);

        pthread_create(&threads[tid], NULL, &test_cip, &args[tid]);
    }

    start_time = util_time_ms();
    end_time = start_time + (seconds * 1000);

    while(!done && util_time_ms() < end_time) {
        util_sleep_ms(100);
    }

    success = !done;

    done = 1;

    for(int tid=0; tid < num_threads && tid < MAX_THREADS; tid++) {
        pthread_join(threads[tid], NULL);
    }

    fprintf(stderr, "--- All test threads terminated.\n");

    if(!success) {
        fprintf(stderr,"*** Test FAILED!\n");
    } else {
        fprintf(stderr,"*** Test SUCCEEDED!\n");
    }

    return 0;
}
