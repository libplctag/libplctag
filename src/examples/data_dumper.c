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
#include <time.h>
#include <sys/time.h>
#include <sys/select.h>
#include <stdlib.h>
#include "../lib/libplctag.h"
#include "utils.h"

#define TAG_PATH "protocol=ab_eip&gateway=192.168.1.7&path=1,0&cpu=LGX&elem_size=4&elem_count=125&name=LogData"
#define ELEM_COUNT 125
#define ELEM_SIZE 4
#define DATA_TIMEOUT 5000




void log_data(plc_tag tag)
{
    static int log_year = 0;
    static int log_month = 0;
    static int log_day = 0;
    static FILE *log = NULL;
    int i = 0;
    char timestamp_buf[128];
    time_t t = time(NULL);
    struct tm *tm_struct;

    tm_struct = localtime(&t);

    /* do we need to open the file?*/
    if(log_year != tm_struct->tm_year
       || log_month != tm_struct->tm_mon
       || log_day != tm_struct->tm_mday) {

        char log_file_name[128];

        log_year = tm_struct->tm_year;
        log_month = tm_struct->tm_mon;
        log_day = tm_struct->tm_mday;

        snprintf(log_file_name, sizeof(log_file_name),"log-%04d-%02d-%02d.log", 1900+log_year, log_month, log_day);
        if(log) {
            fclose(log);
        }

        log = fopen(log_file_name,"a");
    }

    strftime(timestamp_buf, sizeof(timestamp_buf), "%Y/%m/%d %H:%M:%S", tm_struct);

    fprintf(log,"%s",timestamp_buf);

    for(i=0; i<ELEM_COUNT; i++) {
        fprintf(log,",%d",plc_tag_get_int32(tag,i*ELEM_SIZE));
    }

    fprintf(log,"\n");

    fflush(log);
}



int main(int argc, char **argv)
{
    plc_tag tag = PLC_TAG_NULL;
    int rc;
    int delay = 200; /* ms */

    if(argc>1) {
        delay = atoi(argv[1]);
    }

    if(delay < 30) {
        fprintf(stderr,"ERROR: delay too small!\n");
        return 1;
    }


    /* create the tag */
    tag = plc_tag_create(TAG_PATH);

    /* everything OK? */
    if(!tag) {
        fprintf(stderr,"ERROR: Could not create tag!\n");

        return 0;
    }

    /* let the connect succeed we hope */
    while(plc_tag_status(tag) == PLCTAG_STATUS_PENDING) {
        sleep(1);
    }

    if(plc_tag_status(tag) != PLCTAG_STATUS_OK) {
        fprintf(stderr,"Error setting up tag internal state.\n");
        return 0;
    }

    rc = plc_tag_read(tag, 1000);

    while(1) {
        /* get the data */
        rc = plc_tag_read(tag, 0);

        sleep_ms(delay);

        if(plc_tag_status(tag) != PLCTAG_STATUS_OK) {
            fprintf(stderr,"ERROR: Unable to read the data! Got error code %d\n",rc);

            return 0;
        }

        log_data(tag);
    }

    /* we are done */
    plc_tag_destroy(tag);

    return 0;
}


