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

#define TAG_PATH "protocol=ab_eip&gateway=10.206.1.39&path=1,0&cpu=LGX&elem_size=4&elem_count=1&name=TestDINTArray[%d]&debug=3"

#define DATA_TIMEOUT 500



/*
 * This test program creates a lot of threads that read the same tag in
 * the plc.  They all hit the exact same underlying tag data structure.
 * This tests, to some extent, whether the library can handle multi-threaded
 * access.
 */


/* global to cheat on passing it to threads. */
volatile int done = 0;


/*
 * time_ms
 *
 * Get current epoch time in ms.
 */

int64_t time_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv,NULL);

    return  ((int64_t)tv.tv_sec*1000)+ ((int64_t)tv.tv_usec/1000);
}



int sleep_ms(int ms)
{
    struct timeval tv;

    tv.tv_sec = ms/1000;
    tv.tv_usec = (ms % 1000)*1000;

    return select(0,NULL,NULL,NULL, &tv);
}



const char* decode_error(int rc)
{
    switch(rc) {
        case PLCTAG_STATUS_PENDING: return "PLCTAG_STATUS_PENDING"; break;
        case PLCTAG_STATUS_OK: return "PLCTAG_STATUS_OK"; break;
        case PLCTAG_ERR_NULL_PTR: return "PLCTAG_ERR_NULL_PTR"; break;
        case PLCTAG_ERR_OUT_OF_BOUNDS: return "PLCTAG_ERR_OUT_OF_BOUNDS"; break;
        case PLCTAG_ERR_NO_MEM: return "PLCTAG_ERR_NO_MEM"; break;
        case PLCTAG_ERR_LL_ADD: return "PLCTAG_ERR_LL_ADD"; break;
        case PLCTAG_ERR_BAD_PARAM: return "PLCTAG_ERR_BAD_PARAM"; break;
        case PLCTAG_ERR_CREATE: return "PLCTAG_ERR_CREATE"; break;
        case PLCTAG_ERR_NOT_EMPTY: return "PLCTAG_ERR_NOT_EMPTY"; break;
        case PLCTAG_ERR_OPEN: return "PLCTAG_ERR_OPEN"; break;
        case PLCTAG_ERR_SET: return "PLCTAG_ERR_SET"; break;
        case PLCTAG_ERR_WRITE: return "PLCTAG_ERR_WRITE"; break;
        case PLCTAG_ERR_TIMEOUT: return "PLCTAG_ERR_TIMEOUT"; break;
        case PLCTAG_ERR_TIMEOUT_ACK: return "PLCTAG_ERR_TIMEOUT_ACK"; break;
        case PLCTAG_ERR_RETRIES: return "PLCTAG_ERR_RETRIES"; break;
        case PLCTAG_ERR_READ: return "PLCTAG_ERR_READ"; break;
        case PLCTAG_ERR_BAD_DATA: return "PLCTAG_ERR_BAD_DATA"; break;
        case PLCTAG_ERR_ENCODE: return "PLCTAG_ERR_ENCODE"; break;
        case PLCTAG_ERR_DECODE: return "PLCTAG_ERR_DECODE"; break;
        case PLCTAG_ERR_UNSUPPORTED: return "PLCTAG_ERR_UNSUPPORTED"; break;
        case PLCTAG_ERR_TOO_LONG: return "PLCTAG_ERR_TOO_LONG"; break;
        case PLCTAG_ERR_CLOSE: return "PLCTAG_ERR_CLOSE"; break;
        case PLCTAG_ERR_NOT_ALLOWED: return "PLCTAG_ERR_NOT_ALLOWED"; break;
        case PLCTAG_ERR_THREAD: return "PLCTAG_ERR_THREAD"; break;
        case PLCTAG_ERR_NO_DATA: return "PLCTAG_ERR_NO_DATA"; break;
        case PLCTAG_ERR_THREAD_JOIN: return "PLCTAG_ERR_THREAD_JOIN"; break;
        case PLCTAG_ERR_THREAD_CREATE: return "PLCTAG_ERR_THREAD_CREATE"; break;
        case PLCTAG_ERR_MUTEX_DESTROY: return "PLCTAG_ERR_MUTEX_DESTROY"; break;
        case PLCTAG_ERR_MUTEX_UNLOCK: return "PLCTAG_ERR_MUTEX_UNLOCK"; break;
        case PLCTAG_ERR_MUTEX_INIT: return "PLCTAG_ERR_MUTEX_INIT"; break;
        case PLCTAG_ERR_MUTEX_LOCK: return "PLCTAG_ERR_MUTEX_LOCK"; break;
        case PLCTAG_ERR_NOT_IMPLEMENTED: return "PLCTAG_ERR_NOT_IMPLEMENTED"; break;
        case PLCTAG_ERR_BAD_DEVICE: return "PLCTAG_ERR_BAD_DEVICE"; break;
        case PLCTAG_ERR_BAD_GATEWAY: return "PLCTAG_ERR_BAD_GATEWAY"; break;
        case PLCTAG_ERR_REMOTE_ERR: return "PLCTAG_ERR_REMOTE_ERR"; break;
        case PLCTAG_ERR_NOT_FOUND: return "PLCTAG_ERR_NOT_FOUND"; break;
        case PLCTAG_ERR_ABORT: return "PLCTAG_ERR_ABORT"; break;
        case PLCTAG_ERR_WINSOCK: return "PLCTAG_ERR_WINSOCK"; break;

        default: return "Unknown error."; break;
    }

    return "Unknown error.";
}


static int open_tag(plc_tag *tag, const char *tag_str)
{
    int rc = PLCTAG_STATUS_OK;

    /* create the tag */
    *tag = plc_tag_create(tag_str);

    /* everything OK? */
    if(! *tag) {
        fprintf(stderr,"ERROR: Could not create tag!\n");
        return PLCTAG_ERR_CREATE;
    } else {
        fprintf(stderr, "INFO: Tag created with status %s\n", decode_error(plc_tag_status(*tag)));
    }

    /* let the connect succeed we hope */
    while(plc_tag_status(*tag) == PLCTAG_STATUS_PENDING) {
        sleep_ms(10);
    }

    if(plc_tag_status(*tag) != PLCTAG_STATUS_OK) {
        fprintf(stderr,"Error setting up tag internal state.\n");
        rc = plc_tag_status(*tag);
        plc_tag_destroy(*tag);
        return rc;
    }

    return rc;
}



/*
 * Thread function.  Just read until killed.
 */

void serial_test(void *data)
{
    int tid = (int)(intptr_t)data;
    static const char *tag_str = "protocol=ab_eip&gateway=10.206.1.39&path=1,0&cpu=LGX&elem_size=4&elem_count=1&name=TestDINTArray[0]&debug=3";
    int32_t value;
    uint64_t start;
    uint64_t end;
    plc_tag tag = PLC_TAG_NULL;
    int rc = PLCTAG_STATUS_OK;

    while(!done) {
        /* capture the starting time */
        start = time_ms();

        rc = open_tag(&tag, tag_str);

        if(rc != PLCTAG_STATUS_OK) {
            fprintf(stderr,"Test %d, Error creating tag!  Terminating test...\n", tid);
            done = 1;
            return;
        }

        rc = plc_tag_read(tag, DATA_TIMEOUT);

        if(rc != PLCTAG_STATUS_OK) {
            done = 1;
        } else {
            value = plc_tag_get_int32(tag,0);

            /* increment the value, keep it in bounds of 0-499 */
            value = (value >= (int32_t)500 ? (int32_t)0 : value + (int32_t)1);

            /* yes, we should be checking this return value too... */
            plc_tag_set_int32(tag, 0, value);

            /* write the value */
            rc = plc_tag_write(tag, DATA_TIMEOUT);
        }

        plc_tag_destroy(tag);

        end = time_ms();

        fprintf(stderr,"Test %d got result %d with return code %s in %dms\n",tid,value,decode_error(rc),(int)(end-start));

        //sleep_ms(1);
    }

    fprintf(stderr, "Test %d terminating.\n", tid);
}


int main(int argc, char **argv)
{
    pthread_t serial_test_thread;
    uint64_t start_time;
    uint64_t end_time;

    /* create the test threads */
    fprintf(stderr, "Creating serial test thread (Test #1).\n");
    pthread_create(&serial_test_thread, NULL, (void *) &serial_test, (void *)(intptr_t)1);

    start_time = time_ms();
    end_time = start_time + 5000;

    while(!done && time_ms() < end_time) {
        sleep_ms(100);
    }

    done = 1;

    pthread_join(serial_test_thread, NULL);

    fprintf(stderr, "All tests done.\n");

    return 0;
}

