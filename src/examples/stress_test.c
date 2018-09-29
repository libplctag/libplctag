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

#define TAG_PATH "protocol=ab_eip&gateway=10.206.1.39&path=1,0&cpu=LGX&elem_size=4&elem_count=1&name=TestDINTArray[%d]&debug=3"

#define DATA_TIMEOUT 1500
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


static int open_tag(plc_tag *tag, FILE *log, int tid, int num_elems)
{
    int rc = PLCTAG_STATUS_OK;
    static const char *tag_str = "protocol=ab_eip&gateway=10.206.1.39&path=1,5&cpu=lgx&elem_size=4&elem_count=%d&name=TestBigArray[%d]&debug=4";
    char buf[250] = {0,};

    snprintf(buf, sizeof(buf), tag_str, num_elems, tid*num_elems);

    fprintf(log,"--- Test %d, Creating tag (%d, %d) with string %s\n", tid, tid, num_elems, buf);

    /* create the tag */
    start_time = util_time_ms();
    *tag = plc_tag_create_sync(tag_str, TAG_CREATE_TIMEOUT);

    /* everything OK? */
    if(! *tag) {
        fprintf(log,"!!! Test %d, could not create tag!\n", tid);
        return PLCTAG_ERR_CREATE;
    }

    if(rc != PLCTAG_STATUS_OK) {
        fprintf(log,"!!! Test %d, error %s setting up tag internal state.\n", tid, plc_tag_decode_error(rc));
        plc_tag_destroy(*tag);
        *tag = (plc_tag)0;
        return rc;
    }

    return rc;
}


static void *test_cip(void *data)
{
    thread_args *args = (thread_args *)data;
    int tid = args->tid;
    int num_elems = args->num_elems;
    int32_t value = 0;
    int64_t start = 0;
    int64_t end = 0;
    int64_t total_io_time = 0;
    int64_t timeout = 0;
    plc_tag tag = PLC_TAG_NULL;
    int rc = PLCTAG_STATUS_OK;
    int iteration = 1;
    int first_time = 1;
    FILE *log = NULL;

    while(!done) {
        /* capture the starting time */
        start = util_time_ms();

        rc = open_tag(&tag, tag_str);

        if(rc != PLCTAG_STATUS_OK) {
            fprintf(stderr,"Test %d, Error creating tag!  Terminating test...\n", tid);
            done = 1;
            return NULL;
        }

        rc = plc_tag_read(tag, DATA_TIMEOUT);

        if(rc != PLCTAG_STATUS_OK) {
            done = 1;
        } else {
            value = plc_tag_get_int16(tag,0);

            /* increment the value, keep it in bounds of 0-499 */
            value = (value >= (int16_t)500 ? (int16_t)0 : value + (int16_t)1);

            /* yes, we should be checking this return value too... */
            plc_tag_set_int16(tag, 0, value);

            /* write the value */
            rc = plc_tag_write(tag, DATA_TIMEOUT);
        }

        plc_tag_destroy(tag);

        end = util_time_ms();

        fprintf(stderr,"Thread %d, iteration %d, got result %d with return code %s in %dms\n",tid, iteration, value, plc_tag_decode_error(rc), (int)(end-start));

/*        if(iteration >= 100) {
            iteration = 1;
            util_sleep_ms(5000);
        }
*/
        iteration++;
    }

    fprintf(stderr, "Test %d terminating.\n", tid);

    return NULL;
}

    log = open_log(tid);

    fprintf(stderr, "--- Test %d updating %d elements starting at index %d.\n", tid, num_elems, tid*num_elems);
    fprintf(log, "--- Test %d updating %d elements starting at index %d.\n", tid, num_elems, tid*num_elems);

void *test_cip(void *data)
{
    int tid = (int)(intptr_t)data;
    static const char *tag_str = "protocol=ab_eip&gateway=10.206.1.39&path=1,0&cpu=lgx&elem_size=4&elem_count=1&name=TestDINTArray[0]&debug=5";
    int32_t value = 0;
    uint64_t start = 0;
    uint64_t end = 0;
    int64_t timeout = 0;
    plc_tag tag = PLC_TAG_NULL;
    int rc = PLCTAG_STATUS_OK;
    int iteration = 1;

    while(!done) {
        if(!tag) {
            rc = open_tag(&tag, tag_str);
            if(rc != PLCTAG_STATUS_OK) {
                fprintf(stderr,"Test %d, iteration %d, Error (%s) creating tag!  Terminating test...\n", tid, iteration, plc_tag_decode_error(rc));
                done = 1;
                return NULL;
            }
        }

        /* capture the starting time */
        start = util_time_ms();

        do {
            rc = plc_tag_read(tag, DATA_TIMEOUT);
            if(rc != PLCTAG_STATUS_OK) {
                fprintf(stderr, "Test %d, iteration %d, read failed with error %s\n", tid, iteration, plc_tag_decode_error(rc));
                break;
            }

            value = plc_tag_get_int32(tag,0);

            /* increment the value, keep it in bounds of 0-499 */
            value = (value >= (int32_t)500 ? (int32_t)0 : value + (int32_t)1);

            /* yes, we should be checking this return value too... */
            plc_tag_set_int32(tag, 0, value);

            /* write the value */
            rc = plc_tag_write(tag, DATA_TIMEOUT);
            if(rc != PLCTAG_STATUS_OK) {
                fprintf("Test %d, iteration %d, write failed with error %s\n", tid, iteration, plc_tag_decode_error(rc));
                break;
            }
        } while(0);

        end = util_time_ms();

        fprintf(stderr,"Test %d, iteration %d, got result %d with return code %s in %dms\n",tid, iteration, value, plc_tag_decode_error(rc), (int)(end-start));

        if(rc != PLCTAG_STATUS_OK) {
            fprintf(stderr,"Test %d, iteration %d, closing tag due to error %s, will retry in 2 seconds.\n", tid, iteration, plc_tag_decode_error(rc));

            plc_tag_destroy(tag);
            tag = PLC_TAG_NULL;

            /* retry later */
            timeout = util_time_ms() + TAG_CREATE_TIMEOUT;
            while(timeout < util_time_ms()) {
                util_sleep_ms(10);
            }
        }

        iteration++;
    }

    plc_tag_destroy(tag);

    fprintf(stderr, "Test %d terminating after %d iterations.\n", tid, iteration);

    return NULL;
}



void *test_cip_old(void *data)
{
    int tid = (int)(intptr_t)data;
    static const char *tag_str_no_connect = "protocol=ab_eip&gateway=127.0.0.1&path=1,0&cpu=lgx&use_connected_msg=0&elem_size=4&elem_count=1&name=TestDINTArray[0]&debug=4";
    static const char *tag_str_connect_shared = "protocol=ab_eip&gateway=127.0.0.1&path=1,0&cpu=lgx&elem_size=4&elem_count=1&name=TestDINTArray[0]&debug=4";
    static const char *tag_str_connect_not_shared = "protocol=ab_eip&gateway=127.0.0.1&path=1,0&cpu=lgx&share_session=0&elem_size=4&elem_count=1&name=TestDINTArray[0]&debug=4";
    const char * tag_str = NULL;
    int32_t value = 0;
    uint64_t start = 0;
    uint64_t end = 0;
    plc_tag tag = PLC_TAG_NULL;
    int rc = PLCTAG_STATUS_OK;
    int iteration = 1;
    int no_destroy = 0;

/*    switch(test_flags) {
        case 0:
            tag_str = tag_str_no_connect;
            no_destroy = 1;
            fprintf(stderr,"Test %d, testing unconnected CIP, do not destroy tag each cycle.\n", tid);
            break;
        case 1:
            tag_str = tag_str_connect_shared;
            fprintf(stderr,"Test %d, testing connected CIP, shared connections, do not destroy tag each cycle.\n", tid);
            no_destroy = 1;
            break;
        case 2:
            tag_str = tag_str_connect_not_shared;
            fprintf(stderr,"Test %d, testing connected CIP, do not share connections, do not destroy tag each cycle.\n", tid);
            no_destroy = 1;
            break;
        case 10:
            tag_str = tag_str_no_connect;
            no_destroy = 0;
            fprintf(stderr,"Test %d, testing unconnected CIP, recreate and destroy tag each cycle.\n", tid);
            break;
        case 11:
            tag_str = tag_str_connect_shared;
            fprintf(stderr,"Test %d, testing connected CIP, shared connections, recreate and destroy tag each cycle.\n", tid);
            no_destroy = 0;
            break;
        case 12:
            tag_str = tag_str_connect_not_shared;
            fprintf(stderr,"Test %d, testing connected CIP, do not share connections, recreate and destroy tag each cycle.\n", tid);
            no_destroy = 0;
            break;
        default:
            fprintf(stderr,"Illegal test type, pick 0-2\n");
            done = 1;
            return NULL;
            break;
    }
*/

            first_time = 0;

            rc = open_tag(&tag, log, tid, num_elems);
            if(rc != PLCTAG_STATUS_OK) {
                fprintf(log,"!!! Test %d, iteration %d, Error (%s) creating tag!  Retrying in %dms.", tid, iteration, plc_tag_decode_error(rc), TAG_CREATE_TIMEOUT);
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

/*        if(iteration >= 100) {
            iteration = 1;
            util_sleep_ms(5000);
        }

        iteration++;
    }

    plc_tag_destroy(tag);

    fprintf(log, "*** Test %d terminating after %d iterations and an average of %dms per iteration.\n", tid, iteration, (int)(total_io_time/iteration));

    close_log(log);

    return NULL;
}



#define MAX_THREADS (100)

int main(int argc, char **argv)
{
    pthread_t threads[MAX_THREADS];
    int64_t start_time;
    int64_t end_time;
    int num_threads = 0;
    int64_t seconds = 0;

    if(argc>2) {
        int tmp_seconds = 0;
        num_threads = atoi(argv[1]);
        seconds = atoi(argv[2]);
/*        test_flags = atoi(argv[2]);*/
    } else {
        fprintf(stderr,"Usage: stress_test <num threads> <seconds to run>\n");
        return 0;
    }

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
