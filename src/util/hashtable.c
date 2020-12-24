/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever     *
 * you choose.                                                             *
 *                                                                         *
 * MPL 2.0:                                                                *
 *                                                                         *
 *   This Source Code Form is subject to the terms of the Mozilla Public   *
 *   License, v. 2.0. If a copy of the MPL was not distributed with this   *
 *   file, You can obtain one at http://mozilla.org/MPL/2.0/.              *
 *                                                                         *
 *                                                                         *
 * LGPL 2:                                                                 *
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

#include <stddef.h>
#include <lib/libplctag.h>
#include <util/debug.h>
#include <util/hash.h>
#include <util/hashtable.h>
#include <util/mem.h>
#include <util/time.h>
#include <util/vector.h>

/*
 * This implements a simple linear probing hash table.
 *
 * Note that it will readjust its size if enough entries are made.
 */

#define MAX_ITERATIONS  (10)
#define MAX_INCREMENT (10000)

struct hashtable_entry_t {
    void *data;
    int64_t key;
};

struct hashtable_t {
    int total_entries;
    int used_entries;
    uint32_t hash_salt;
    struct hashtable_entry_t *entries;
};


typedef struct hashtable_entry_t *hashtable_entry_p;

//static int next_highest_prime(int x);
static int find_key(hashtable_p table, int64_t key);
static int find_empty(hashtable_p table, int64_t key);
static int expand_table(hashtable_p table);


hashtable_p hashtable_create(int initial_capacity)
{
    hashtable_p tab = NULL;

    pdebug(DEBUG_INFO,"Starting");

    if(initial_capacity <= 0) {
        pdebug(DEBUG_WARN,"Size is less than or equal to zero!");
        return NULL;
    }

    tab = mem_alloc(sizeof(struct hashtable_t));
    if(!tab) {
        pdebug(DEBUG_ERROR,"Unable to allocate memory for hash table!");
        return NULL;
    }

    tab->total_entries = initial_capacity;
    tab->used_entries = 0;
    tab->hash_salt = (uint32_t)(time_ms()) + (uint32_t)(intptr_t)(tab);

    tab->entries = mem_alloc(initial_capacity * (int)sizeof(struct hashtable_entry_t));
    if(!tab->entries) {
        pdebug(DEBUG_ERROR,"Unable to allocate entry array!");
        hashtable_destroy(tab);
        return NULL;
    }

    pdebug(DEBUG_INFO,"Done");

    return tab;
}


void *hashtable_get(hashtable_p table, int64_t key)
{
    int index = 0;
    void *result = NULL;

    pdebug(DEBUG_SPEW,"Starting");

    if(!table) {
        pdebug(DEBUG_WARN,"Hashtable pointer null or invalid.");
        return NULL;
    }

    index = find_key(table, key);
    if(index != PLCTAG_ERR_NOT_FOUND) {
        result = table->entries[index].data;
        pdebug(DEBUG_SPEW,"found data %p", result);
    } else {
        pdebug(DEBUG_SPEW, "key not found!");
    }

    pdebug(DEBUG_SPEW,"Done");

    return result;
}


int hashtable_put(hashtable_p table, int64_t key, void  *data)
{
    int rc = PLCTAG_STATUS_OK;
    int index = 0;

    pdebug(DEBUG_SPEW,"Starting");

    if(!table) {
        pdebug(DEBUG_WARN,"Hashtable pointer null or invalid.");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* try to find a slot to put the new entry */
    index = find_empty(table, key);
    while(index == PLCTAG_ERR_NOT_FOUND) {
        rc = expand_table(table);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to expand table to make entry unique!");
            return rc;
        }

        index = find_empty(table, key);
    }

    pdebug(DEBUG_SPEW, "Putting value at index %d", index);

    table->entries[index].key = key;
    table->entries[index].data = data;
    table->used_entries++;

    pdebug(DEBUG_SPEW, "Done.");

    return PLCTAG_STATUS_OK;
}


void *hashtable_get_index(hashtable_p table, int index)
{
    if(!table) {
        pdebug(DEBUG_WARN,"Hashtable pointer null or invalid");
        return NULL;
    }

    if(index < 0 || index >= table->total_entries) {
        pdebug(DEBUG_WARN, "Out of bounds index!");
        return NULL;
    }

    return table->entries[index].data;
}



int hashtable_capacity(hashtable_p table)
{
    if(!table) {
        pdebug(DEBUG_WARN,"Hashtable pointer null or invalid");
        return PLCTAG_ERR_NULL_PTR;
    }

    return table->total_entries;
}



int hashtable_entries(hashtable_p table)
{
    if(!table) {
        pdebug(DEBUG_WARN,"Hashtable pointer null or invalid");
        return PLCTAG_ERR_NULL_PTR;
    }

    return table->used_entries;
}



int hashtable_on_each(hashtable_p table, int (*callback_func)(hashtable_p table, int64_t key, void *data, void *context), void *context_arg)
{
    int rc = PLCTAG_STATUS_OK;

    if(!table) {
        pdebug(DEBUG_WARN,"Hashtable pointer null or invalid");
    }

    for(int i=0; i < table->total_entries && rc == PLCTAG_STATUS_OK; i++) {
        if(table->entries[i].data) {
            rc = callback_func(table, table->entries[i].key, table->entries[i].data, context_arg);
        }
    }

    return rc;
}



void *hashtable_remove(hashtable_p table, int64_t key)
{
    int index = 0;
    void *result = NULL;

    pdebug(DEBUG_DETAIL,"Starting");

    if(!table) {
        pdebug(DEBUG_WARN,"Hashtable pointer null or invalid.");
        return result;
    }

    index = find_key(table, key);
    if(index == PLCTAG_ERR_NOT_FOUND) {
        pdebug(DEBUG_SPEW,"Not found.");
        return result;
    }

    result = table->entries[index].data;
    table->entries[index].key = 0;
    table->entries[index].data = NULL;
    table->used_entries--;

    pdebug(DEBUG_DETAIL,"Done");

    return result;
}




int hashtable_destroy(hashtable_p table)
{
    pdebug(DEBUG_INFO,"Starting");

    if(!table) {
        pdebug(DEBUG_WARN,"Called with null pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    mem_free(table->entries);
    table->entries = NULL;

    mem_free(table);

    pdebug(DEBUG_INFO,"Done");

    return PLCTAG_STATUS_OK;
}





/***********************************************************************
 *************************** Helper Functions **************************
 **********************************************************************/


#define KEY_TO_INDEX(t, k) (uint32_t)((hash((uint8_t*)&k, sizeof(k), t->hash_salt)) % (uint32_t)(t->total_entries))


int find_key(hashtable_p table, int64_t key)
{
    uint32_t initial_index = KEY_TO_INDEX(table, key);
    int index = 0;
    int iteration = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    /*
     * search for the hash value.
     *
     * Note: do NOT stop if we find a NULL entry.  That might be the result
     * of a removed entry and there could still be entries past that point
     * that are for the initial slot.
     */
    for(iteration=0; iteration < MAX_ITERATIONS; iteration++) {
        index = ((int)initial_index + iteration) % table->total_entries;

        if(table->entries[index].key == key) {
            break;
        }
    }

    if(iteration >= MAX_ITERATIONS) {
        /* FIXME - does not work on Windows. */
        //pdebug(DEBUG_SPEW, "Key %ld not found.", key);
        return PLCTAG_ERR_NOT_FOUND;
    } else {
        //pdebug(DEBUG_SPEW, "Key %d found at index %d.", (int)table->entries[index].key, index);
    }

    pdebug(DEBUG_SPEW, "Done.");

    return index;
}




int find_empty(hashtable_p table, int64_t key)
{
    uint32_t initial_index = KEY_TO_INDEX(table, key);
    int index = 0;
    int iteration = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    /* search for the hash value. */
    for(iteration=0; iteration < MAX_ITERATIONS; iteration++) {
        index = ((int)initial_index + iteration) % table->total_entries;

        pdebug(DEBUG_SPEW, "Trying index %d for key %ld.", index, key);
        if(table->entries[index].data == NULL) {
            break;
        }
    }

    if(iteration >= MAX_ITERATIONS) {
        pdebug(DEBUG_SPEW,"No empty entry found in %d iterations!", MAX_ITERATIONS);
        return PLCTAG_ERR_NOT_FOUND;
    }

    pdebug(DEBUG_SPEW, "Done.");

    return index;
}




int expand_table(hashtable_p table)
{
    struct hashtable_t new_table;
    int total_entries = table->total_entries;
    int index = PLCTAG_ERR_NOT_FOUND;

    pdebug(DEBUG_SPEW, "Starting.");

    pdebug(DEBUG_SPEW, "Table using %d entries of %d.", table->used_entries, table->total_entries);

    do {
        /* double entries unless already at max doubling, then increment. */
        total_entries += (total_entries < MAX_INCREMENT ? total_entries : MAX_INCREMENT);

        new_table.total_entries = total_entries;
        new_table.used_entries = 0;
        new_table.hash_salt = table->hash_salt;

        pdebug(DEBUG_SPEW, "trying new size = %d", total_entries);

        new_table.entries = mem_alloc(total_entries * (int)sizeof(struct hashtable_entry_t));
        if(!new_table.entries) {
            pdebug(DEBUG_ERROR, "Unable to allocate new entry array!");
            return PLCTAG_ERR_NO_MEM;
        }

        /* copy the old entries.  Only copy ones that are used. */
        for(int i=0; i < table->total_entries; i++) {
            if(table->entries[i].data) {
                index = find_empty(&new_table, table->entries[i].key);
                if(index == PLCTAG_ERR_NOT_FOUND) {
                    /* oops, still cannot insert all the entries! Try again. */
                    pdebug(DEBUG_WARN, "Unable to insert existing entry into expanded table!");
                    mem_free(new_table.entries);
                    break;
                } else {
                    /* store the entry into the new table. */
                    new_table.entries[index] = table->entries[i];
                    new_table.used_entries++;
                }
            }
        }
    } while(index == PLCTAG_ERR_NOT_FOUND);

    /* success! */
    mem_free(table->entries);
    table->entries = new_table.entries;
    table->total_entries = new_table.total_entries;
    table->used_entries = new_table.used_entries;

    pdebug(DEBUG_SPEW, "Done.");

    return PLCTAG_STATUS_OK;
}
