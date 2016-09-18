/***************************************************************************
 *   Copyright (C) 2016 by OmanTek                                         *
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
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

/*
 * debug.c
 *
 *  Created on: August 1, 2016
 *      Author: Kyle Hayes
 */

#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <util/debug.h>
#include <platform.h>




/*
 * Debugging support.
 */


static int debug_level = DEBUG_NONE;

extern int set_debug_level(int level)
{
    int old_level = debug_level;

    debug_level = level;

    return old_level;
}


extern int get_debug_level(void)
{
    return debug_level;
}

extern void pdebug_impl(const char *func, int line_num, const char *templ, ...)
{
    va_list va;
    struct tm t;
    time_t epoch;
    char prefix[2048];

    /* build the prefix */
    /* get the time parts */
    epoch = time(0);

    /* FIXME - should capture error return! */
    localtime_r(&epoch,&t);

    /* create the prefix and format for the file entry. */
    snprintf(prefix, sizeof prefix,"%04d-%02d-%02d %02d:%02d:%02d %s:%d %s\n",
             t.tm_year+1900,t.tm_mon,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec,
             func,line_num,templ);

    /* print it out. */
    va_start(va,templ);
    vfprintf(stderr,prefix,va);
    va_end(va);
}






extern void pdebug_dump_bytes_impl(uint8_t *data,int count)
{
    int i;
    int end;
    char buf[2048];

    snprintf(buf,sizeof buf,"Dumping bytes:\n");

    end = str_length(buf);

    for(i=0; i<count; i++) {
        if((i%10) == 0) {
            snprintf(buf+end,sizeof(buf)-end,"%05d",i);

            end = strlen(buf);
        }

        snprintf(buf+end,sizeof(buf)-end," %02x",data[i]);

        end = strlen(buf);

        if((i%10) == 9) {
            snprintf(buf+end,sizeof(buf)-end,"\n");

            end = strlen(buf);
        }
    }

    /*if( ((i%10)!=9) || (i>=count && (i%10)==9))
        snprintf(buf+end,sizeof(buf)-end,"\n");*/

    fprintf(stderr,"%s",buf);

    fflush(stderr);
}
