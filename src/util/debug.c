/***************************************************************************
 *   Copyright (C) 2018 by Kyle Hayes                                      *
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
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <util/debug.h>
#include <platform.h>
#include <lib/libplctag.h>
#include <lib/version.h>



/*
 * Debugging support.
 */


static int debug_level = DEBUG_NONE;
static lock_t thread_num_lock = LOCK_INIT;
static volatile uint32_t thread_num = 1;

/*
 * Keep the thread ID and the tag ID thread local.
 */

static THREAD_LOCAL uint32_t this_thread_num = 0;
static THREAD_LOCAL int tag_id = 0;


/* only output the version once */
static lock_t printed_version = LOCK_INIT;



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



void debug_set_tag_id(int t_id)
{
    tag_id = t_id;
}



static uint32_t get_thread_id()
{
    if(!this_thread_num) {
        spin_block(&thread_num_lock) {
            this_thread_num = thread_num;
            thread_num++;
        }
    }

    return this_thread_num;
}

static int make_prefix(char *prefix_buf, int prefix_buf_size)
{
    struct tm t;
    time_t epoch;
    int64_t epoch_ms;
    int remainder_ms;
    int rc = PLCTAG_STATUS_OK;

    /* make sure we have room, MAGIC */
    if(prefix_buf_size < 37) {
        return PLCTAG_ERR_TOO_SMALL;
    }

    /* build the prefix */

    /* get the time parts */
    epoch_ms = time_ms();
    epoch = (time_t)(epoch_ms/1000);
    remainder_ms = (int)(epoch_ms % 1000);

    /* FIXME - should capture error return! */
    localtime_r(&epoch,&t);

    /* create the prefix and format for the file entry. */
    rc = snprintf(prefix_buf, (size_t)prefix_buf_size,"%04d-%02d-%02d %02d:%02d:%02d.%03d thread(%u) tag(%d)",
                  t.tm_year+1900,t.tm_mon,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec,remainder_ms, get_thread_id(), tag_id);

    /* enforce zero string termination */
    if(rc > 1 && rc < prefix_buf_size) {
        prefix_buf[rc] = 0;
    } else {
        prefix_buf[prefix_buf_size - 1] = 0;
    }

    return rc;
}


static const char *debug_level_name[DEBUG_END] = {"NONE", "ERROR", "WARN", "INFO", "DETAIL", "SPEW"};

extern void pdebug_impl(const char *func, int line_num, int debug_level, const char *templ, ...)
{
    va_list va;
    char output[2048];
    char prefix[48]; /* MAGIC */
    int prefix_size;

    /* build the prefix */
    prefix_size = make_prefix(prefix,(int)sizeof(prefix));  /* don't exceed a size that int can express! */
    if(prefix_size <= 0) {
        return;
    }

    /* print only once */
    /* FIXME - this may not be safe. */
    if(!printed_version && debug_level >= DEBUG_INFO) {
        if(lock_acquire_try((lock_t*)&printed_version)) {
            /* create the output string template */
            fprintf(stderr,"%s INFO libplctag version %s\n",prefix, VERSION);
        }
    }

    /* create the output string template */
    snprintf(output, sizeof(output),"%s %s %s:%d %s\n",prefix, debug_level_name[debug_level], func, line_num, templ);

    /* make sure it is zero terminated */
    output[sizeof(output)-1] = 0;

    /* print it out. */
    va_start(va,templ);
    vfprintf(stderr,output,va);
    va_end(va);
}




#define COLUMNS (10)

extern void pdebug_dump_bytes_impl(const char *func, int line_num, int debug_level, uint8_t *data,int count)
{
    int max_row, row, column;
    char prefix[48]; /* MAGIC */
    int prefix_size;
    char row_buf[300]; /* MAGIC */

    /* build the prefix */
    prefix_size = make_prefix(prefix,(int)sizeof(prefix));

    if(prefix_size <= 0) {
        return;
    }

    /* determine the number of rows we will need to print. */
    max_row = (count  + (COLUMNS - 1))/COLUMNS;

    for(row = 0; row < max_row; row++) {
        int offset = (row * COLUMNS);
        int row_offset;

        /* print the prefix and address */
        row_offset = snprintf(&row_buf[0], sizeof(row_buf),"%s %s %s:%d %05d", prefix, debug_level_name[debug_level], func, line_num, offset);

        for(column = 0; column < COLUMNS && ((row * COLUMNS) + column) < count && row_offset < (int)sizeof(row_buf); column++) {
            offset = (row * COLUMNS) + column;
            row_offset += snprintf(&row_buf[row_offset], sizeof(row_buf) - (size_t)row_offset, " %02x", data[offset]);
        }

        /* terminate the row string*/
        row_buf[sizeof(row_buf)-1] = 0; /* just in case */

        /* output it, finally */
        fprintf(stderr,"%s\n",row_buf);
    }


    /*fflush(stderr);*/
}
