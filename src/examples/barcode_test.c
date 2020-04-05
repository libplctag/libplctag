/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
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

#include <inttypes.h>
#include <stdint.h>
#include "../lib/libplctag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utils.h"

#define REQUIRED_VERSION 2,1,0

#define TIMEOUT_MS (15000) /* a loooooong timeout */

#define NEW_BARCODE "protocol=ab_eip&gateway=10.206.1.40&path=1,4&cpu=lgx&elem_size=1&elem_count=1&name=new_barcode"
#define BARCODE_PROCESSED "protocol=ab_eip&gateway=10.206.1.40&path=1,4&cpu=lgx&elem_size=1&elem_count=1&name=barcode_processed"
#define BARCODE "protocol=ab_eip&gateway=10.206.1.40&path=1,4&cpu=lgx&elem_size=88&elem_count=1&name=barcode"

#define TRY(f) if((rc = (f)) != PLCTAG_STATUS_OK) { printf("ERROR: " #f " failed with error %s!\n", plc_tag_decode_error(rc)); break; }

static int wait_for_new_barcode(void);
static int read_barcode(void);
static int mark_barcode_processed(void);

int main(int argc, const char **argv)
{
    int rc = PLCTAG_STATUS_OK;
    int64_t last_read = util_time_ms();
    int64_t first_read = last_read;

    (void)argc;
    (void)argv;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }


    while(1) {
        int64_t new_time;
        int64_t diff_time;
        int64_t total_time;

        TRY(wait_for_new_barcode())

        TRY(read_barcode())

        TRY(mark_barcode_processed())

        new_time = util_time_ms();

        diff_time = new_time - last_read;
        total_time = new_time - first_read;

        printf("Iteration took %" PRId64 "ms, total elapsed time is %" PRId64 "ms.\n", diff_time, total_time);

        last_read = new_time;
    }

    return rc;
}


int wait_for_new_barcode(void)
{
    static int32_t new_barcode_tag = 0;
    int rc = PLCTAG_STATUS_PENDING;

    if(new_barcode_tag <= 0) {
        new_barcode_tag = plc_tag_create(NEW_BARCODE, TIMEOUT_MS);

        if(new_barcode_tag < 0) {
            printf("ERROR: error creating tag for new_barcode!\n");
            return new_barcode_tag;
        }
    }

    rc = PLCTAG_STATUS_PENDING;
    do {
        uint8_t flag_val = 0;

        /* read the tag. */
        TRY(plc_tag_read(new_barcode_tag, TIMEOUT_MS))

        /* get the flag value */
        if((flag_val = plc_tag_get_uint8(new_barcode_tag, 0)) != UINT8_MAX) {
            if(flag_val) {
                rc = PLCTAG_STATUS_OK;
            } else {
                rc = PLCTAG_STATUS_PENDING;
                util_sleep_ms(4000);
            }
        }
    } while(rc == PLCTAG_STATUS_PENDING);

    return rc;
}



int read_barcode(void)
{
    static int32_t barcode_tag = 0;
    int rc = PLCTAG_STATUS_OK;
    char barcode_buf[85] = {0,};
    int32_t barcode_len = 0;

    if(barcode_tag <= 0) {
        barcode_tag = plc_tag_create(BARCODE, TIMEOUT_MS);

        if(barcode_tag < 0) {
            printf("ERROR: error creating tag for barcode!\n");
            return barcode_tag;
        }
    }

    /* read the tag. */
    rc = plc_tag_read(barcode_tag, TIMEOUT_MS);
    if(rc != PLCTAG_STATUS_OK) {
        printf("ERROR: error reading barcode tag!\n");
        return rc;
    }

    /* zero this out so that there is a terminating zero character. */
    memset(barcode_buf, 0, sizeof(barcode_buf));

    /* get the length of the string. */
    barcode_len = plc_tag_get_int32(barcode_tag, 0);

    /* copy the barcode characters. */
    for(int i=0; i < barcode_len && i < (int)sizeof(barcode_buf); i++) {
        barcode_buf[i] = (char)plc_tag_get_uint8(barcode_tag, 4 + i); /* offset for the string length */
    }

    /* print out the barcode */
    printf("Got barcode: %s\n", barcode_buf);

    return PLCTAG_STATUS_OK;
}



int mark_barcode_processed(void)
{
    static int32_t barcode_processed_tag = 0;
    int rc = PLCTAG_STATUS_OK;

    if(barcode_processed_tag <= 0) {
        barcode_processed_tag = plc_tag_create(BARCODE_PROCESSED, TIMEOUT_MS);

        if(barcode_processed_tag < 0) {
            printf("ERROR: error creating tag for barcode_processed!\n");
            return barcode_processed_tag;
        }
    }

    /* set up the value */
    rc = plc_tag_set_uint8(barcode_processed_tag, 0, 0xFF);
    if(rc != PLCTAG_STATUS_OK) {
        printf("ERROR: error setting processed flag value!\n");
        return rc;
    }

    /* write the tag. */
    rc = plc_tag_write(barcode_processed_tag, TIMEOUT_MS);
    if(rc != PLCTAG_STATUS_OK) {
        printf("ERROR: error writing out barcode_processed tag!\n");
        return rc;
    }

    return PLCTAG_STATUS_OK;
}
