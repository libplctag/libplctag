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

#include <assert.h>
#include <stdio.h>
#include "../../lib/libplctag.h"
#include "../../util/hashtable.h"
#include "../../util/debug.h"

#define START_CAPACITY (10)
#define INSERT_ENTRIES (50)

int main(int argc, const char **argv)
{
    hashtable_p table = NULL;
    int size = START_CAPACITY;
    float best_utilization = 0.0;
    float tmp_utilization = 0.0;

    (void)argc;
    (void)argv;

    pdebug(DEBUG_INFO,"Starting hashtable tests.");

    set_debug_level(DEBUG_SPEW);

    /* create a hashtable */
    pdebug(DEBUG_INFO,"Creating hashtable with at least capacity %d.", START_CAPACITY);
    table = hashtable_create(START_CAPACITY);
    assert(table != NULL);
    assert(hashtable_capacity(table) >= START_CAPACITY);
    assert(hashtable_entries(table) == 0);

    size = hashtable_capacity(table);

    /* insert tests. */
    for(int i=1; i <= INSERT_ENTRIES; i++) {
        int rc = hashtable_put(table, i, (void*)(intptr_t)i);
        assert(rc == PLCTAG_STATUS_OK);

        if(hashtable_capacity(table) != size) {
            pdebug(DEBUG_INFO, "Hashtable expanded from %d entries to %d entries after inserting %d entries.", size, hashtable_capacity(table), hashtable_entries(table));
            size = hashtable_capacity(table);
        }
    }

    tmp_utilization = (float)(hashtable_entries(table))/(float)(hashtable_capacity(table));

    pdebug(DEBUG_INFO, "Current table utilization %f%%", tmp_utilization*100.0);

    if(tmp_utilization > best_utilization) {
        best_utilization = tmp_utilization;
    }

    assert(hashtable_entries(table) == INSERT_ENTRIES);
    pdebug(DEBUG_INFO, "Hash table has correct number of used entries, %d.", hashtable_entries(table));

    pdebug(DEBUG_INFO, "Hashtable using %d entries of %d entries capacity.", hashtable_entries(table), hashtable_capacity(table));

    /* retrieval tests. */
    pdebug(DEBUG_INFO, "Running retrieval tests.");
    for(int i=INSERT_ENTRIES; i > 0; i--) {
        void *res = hashtable_get(table, i);
        assert(res != NULL);

        assert(i == (int)(intptr_t)res);
    }

    /* insert + delete tests. */
    pdebug(DEBUG_INFO, "Running combined insert and delete tests.");
    for(int i=INSERT_ENTRIES+1; i < (INSERT_ENTRIES*2); i++) {
        int rc = hashtable_put(table, i, (void*)(intptr_t)i);
        void *res = NULL;

        assert(rc == PLCTAG_STATUS_OK);

        res = hashtable_remove(table, (i - INSERT_ENTRIES));
        assert((i - INSERT_ENTRIES) == (int)(intptr_t)res);
    }

    assert(hashtable_entries(table) == INSERT_ENTRIES);
    pdebug(DEBUG_INFO, "Hash table has correct number of used entries, %d.", hashtable_entries(table));

    tmp_utilization = (float)(hashtable_entries(table))/(float)(hashtable_capacity(table));

    pdebug(DEBUG_INFO, "Current table utilization %f%%", tmp_utilization*100.0);

    if(tmp_utilization > best_utilization) {
        best_utilization = tmp_utilization;
    }

    pdebug(DEBUG_INFO, "Best table utilization %f%%", best_utilization*100.0);

    hashtable_destroy(table);

    pdebug(DEBUG_INFO, "Done.");
}

