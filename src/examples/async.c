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


/*
 * This example reads from a large DINT array.  It creates 150 tags that each read from one element of the
 * array. It fires off all the tags at once and waits for them to complete the reads. In this case, it waits
 * a fixed amount of time and then tries to read the tags.
 */


#include <stdio.h>
#include "../lib/libplctag.h"
#include "utils.h"


#define TAG_PATH "protocol=ab_eip&gateway=10.206.1.39&path=1,5&cpu=LGX&elem_size=4&elem_count=1&name=TestBigArray[%d]&debug=4"
#define NUM_TAGS 100
#define DATA_TIMEOUT 5000

int main()
{
    plc_tag tag[NUM_TAGS];
    int rc;
    int i;
    int64_t timeout = DATA_TIMEOUT + util_time_ms();
    int done = 0;
    int64_t start = 0;
    int64_t end = 0;

    /* create the tags */
    for(i=0; i< NUM_TAGS; i++) {
        char tmp_tag_path[256] = {0,};
        snprintf_platform(tmp_tag_path, sizeof tmp_tag_path,TAG_PATH,i);

        fprintf(stderr, "Attempting to create tag with attribute string '%s'\n",tmp_tag_path);

        tag[i]  = plc_tag_create(tmp_tag_path);

        if(!tag[i]) {
            fprintf(stderr,"Error: could not create tag %d\n",i);
        }
    }


    do {
        done = 1;

        for(i=0; i < NUM_TAGS; i++) {
            rc = plc_tag_status(tag[i]);
            if(rc != PLCTAG_STATUS_OK) {
                done = 0;
            }
        }

        if(!done) {
            util_sleep_ms(1);
        }
    } while(timeout > util_time_ms() && !done) ;

    if(!done) {
        fprintf(stderr, "Timeout waiting for tags to be ready!\n");

        for(i=0; i < NUM_TAGS; i++) {
            plc_tag_destroy(tag[i]);
        }

        return 1;
    }

    start = util_time_ms();

    /* get the data */
    for(i=0; i < NUM_TAGS; i++) {
        rc = plc_tag_read(tag[i], 0);

        if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
            fprintf(stderr,"ERROR: Unable to read the data! Got error code %d: %s\n",rc, plc_tag_decode_error(rc));

            return 0;
        }
    }

    /* wait for all to finish */
    do {
        done = 1;

        for(i=0; i < NUM_TAGS; i++) {
            rc = plc_tag_status(tag[i]);
            if(rc != PLCTAG_STATUS_OK) {
                done = 0;
            }
        }

        if(!done) {
            util_sleep_ms(1);
        }
    } while(timeout > util_time_ms() && !done) ;

    if(!done) {
        fprintf(stderr, "Timeout waiting for tags to finish reading!\n");

        for(i=0; i < NUM_TAGS; i++) {
            plc_tag_destroy(tag[i]);
        }

        return 1;
    }

    end = util_time_ms();

    /* get any data we can */
    for(i=0; i < NUM_TAGS; i++) {
        /* read complete! */
        fprintf(stderr,"Tag %d data[0]=%d\n",i,plc_tag_get_int32(tag[i],0));
    }


    /* we are done */
    for(i=0; i < NUM_TAGS; i++) {
        plc_tag_destroy(tag[i]);
    }

    fprintf(stderr, "Read %d tags in %dms\n", NUM_TAGS, (int)(end - start));

    return 0;
}
