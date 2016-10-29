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


static lock_t thread_num_lock = LOCK_INIT;
static volatile uint32_t thread_num = 1;

static THREAD_LOCAL uint32_t this_thread_num = 0;


static uint32_t get_thread_id()
{
    if(!this_thread_num) {
        while(!lock_acquire(&thread_num_lock)) { } /* FIXME - this could hang! just keep trying */
            this_thread_num = thread_num;
            thread_num++;
        lock_release(&thread_num_lock);
    }

    return this_thread_num;
}


extern void pdebug_impl(const char *func, int line_num, const char *templ, ...)
{
    va_list va;
    struct tm t;
    time_t epoch;
    int64_t epoch_ms;
    int remainder_ms;
    char prefix[2048];

    /* build the prefix */

    /* get the time parts */
    epoch_ms = time_ms();
    epoch = epoch_ms/1000;
    remainder_ms = (int)(epoch_ms % 1000);

    /* FIXME - should capture error return! */
    localtime_r(&epoch,&t);

    /* create the prefix and format for the file entry. */
    snprintf(prefix, sizeof prefix,"thread(%04u) %04d-%02d-%02d %02d:%02d:%02d.%03d %s:%d %s\n",
             get_thread_id(),
             t.tm_year+1900,t.tm_mon,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec,remainder_ms,
             func, line_num, templ);

    /* print it out. */
    va_start(va,templ);
    vfprintf(stderr,prefix,va);
    va_end(va);
}




#define COLUMNS (10)

extern void pdebug_dump_bytes_impl(uint8_t *data,int count)
{
    int max_row, row, column, offset;

    /* determine the number of rows we will need to print. */
    max_row = (count  + (COLUMNS - 1))/COLUMNS;

    fprintf(stderr,"Dumping bytes:\n");

    for(row = 0; row < max_row; row++) {
        offset = (row * COLUMNS);

        fprintf(stderr,"%05d", offset);

        for(column = 0; column < COLUMNS && offset < count; column++) {
            offset = (row * COLUMNS) + column;
            fprintf(stderr, " %02x", data[offset]);
        }

        fprintf(stderr,"\n");
    }

    fflush(stderr);
}
