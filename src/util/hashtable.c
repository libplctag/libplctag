/***************************************************************************
 *   Copyright (C) 2017 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library/Lesser General Public License as*
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


#include <lib/libplctag.h>
#include <platform.h>
#include <util/debug.h>
#include <util/hash.h>
#include <util/hashtable.h>
#include <util/vector.h>

/*
 * This implements a simple linear probing hash table.
 *
 * Note that it will readjust its size if enough entries are made.
 */

//#define TABLE_INC       (57)
#define MAX_ITERATIONS  (10)

struct hashtable_entry_t {
    void *data;
    int64_t key;
};

struct hashtable_t {
    int increment;
    int total_entries;
    int used_entries;
    uint32_t hash_salt;
    struct hashtable_entry_t *entries;
};


typedef struct hashtable_entry_t *hashtable_entry_p;

//static int find_entry(hashtable_p table, void *key, int key_len, uint32_t *bucket_index, vector_p *bucket, uint32_t *index, hashtable_entry_p *entry_ref);
//static int entry_cmp(hashtable_entry_p entry_ref, void *key, int key_len);
//static hashtable_entry_p hashtable_entry_create(void *key, int key_len, void * data_ref);
//void hashtable_entry_destroy(int num_args, void **args);

static int find_key(hashtable_p table, int64_t key);
static int find_empty(hashtable_p table, int64_t key);
static int expand_table(hashtable_p table);


hashtable_p hashtable_create(int initial_size, int increment)
{
    hashtable_p tab = NULL;

    pdebug(DEBUG_INFO,"Starting");

    if(initial_size <= 0) {
        pdebug(DEBUG_WARN,"Size is less than or equal to zero!");
        return NULL;
    }

    tab = mem_alloc(sizeof(struct hashtable_t));
    if(!tab) {
        pdebug(DEBUG_ERROR,"Unable to allocate memory for hash table!");
        return NULL;
    }

    tab->increment = increment;
    tab->total_entries = initial_size;
    tab->used_entries = 0;
    tab->hash_salt = (uint32_t)(time_ms()) + (uint32_t)(intptr_t)(tab);

    tab->entries = mem_alloc(initial_size * (int)sizeof(struct hashtable_entry_t));
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

int find_key(hashtable_p table, int64_t key)
{
    /* get the index */
    uint32_t hash_val = hash((uint8_t*)&key, sizeof(key), table->hash_salt);
    int index = (int)(hash_val % (uint32_t)table->total_entries);
    int iteration;

    pdebug(DEBUG_SPEW, "Starting.");

    /* search for the hash value. */
    for(iteration=1; iteration < MAX_ITERATIONS; iteration++) {
        if(table->entries[index].key == key) {
            break;
        }

        index = (index + iteration) % table->total_entries;
    }

    if(iteration == MAX_ITERATIONS) {
        pdebug(DEBUG_SPEW, "Key not found.");
        return PLCTAG_ERR_NOT_FOUND;
    } else {
        pdebug(DEBUG_SPEW, "Key %d found at index %d.", (int)table->entries[index].key, index);
    }

    pdebug(DEBUG_SPEW, "Done.");

    return index;
}




int find_empty(hashtable_p table, int64_t key)
{
    uint32_t hash_val = hash((uint8_t*)&key, sizeof(key), table->hash_salt);
    int index = (int)(hash_val % (uint32_t)table->total_entries);

    /* search for the hash value. */
    for(int iteration=1; iteration < MAX_ITERATIONS; iteration++) {
        if(table->entries[index].data == NULL) {
            break;
        }

        index = (index + iteration) % table->total_entries;
    }

    if(table->entries[index].data) {
        pdebug(DEBUG_DETAIL,"No empty entry found in %d iterations!", MAX_ITERATIONS);
        return PLCTAG_ERR_NOT_FOUND;
    }

    return index;
}




int expand_table(hashtable_p table)
{
    struct hashtable_t new_table;
    int total_entries = table->total_entries;
    int index = PLCTAG_ERR_NOT_FOUND;

    pdebug(DEBUG_DETAIL, "Starting.");

    do {
        total_entries += table->increment;
        new_table.total_entries = total_entries;
        new_table.used_entries = 0;
        new_table.hash_salt = table->hash_salt;

        pdebug(DEBUG_DETAIL, "trying new size = %d", total_entries);

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

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}
