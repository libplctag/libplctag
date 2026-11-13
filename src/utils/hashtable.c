/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
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

/*
 * Open addressed hash table with linear probing, keyed by non-zero int64_t.
 *
 * A key's home entry is its own low bits, so the table size is always a power of two.  Keys
 * must already be well mixed, as hash64() values are: nothing is hashed again.
 *
 * Key 0 marks an empty entry, so callers never use it.  A key lives within MAX_PROBES entries
 * of its home.  Remove leaves a hole, so a lookup cannot stop at an empty entry and checks all
 * MAX_PROBES.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <utils/hashtable.h>
#include <libplctag/lib/libplctag.h>
#include <platform.h>
#include <utils/debug.h>


#define MAX_PROBES (10)


struct hashtable_entry_t {
    void *data;
    int64_t key;
};

struct hashtable_t {
    struct hashtable_entry_t *entries;
    int32_t total_entries;
    int32_t used_entries;
};


static int32_t find_key(hashtable_p table, int64_t key);
static int32_t find_empty(struct hashtable_entry_t *entries, int32_t total_entries, int64_t key);
static int32_t expand_table(hashtable_p table);


extern hashtable_p hashtable_create(int32_t initial_capacity) {
    hashtable_p tab = NULL;
    int32_t size = 1;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, 0, "Starting");

    if(initial_capacity <= 0 || initial_capacity > (INT32_MAX / (int32_t)sizeof(struct hashtable_entry_t))) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Initial capacity %" PRId32 " is out of range!", initial_capacity);
        return NULL;
    }

    while(size < initial_capacity) { size <<= 1; }

    tab = mem_alloc(sizeof(struct hashtable_t));
    if(!tab) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_ERROR, 0, "Unable to allocate memory for hash table!");
        return NULL;
    }

    tab->entries = mem_alloc(size * (int)sizeof(struct hashtable_entry_t));
    if(!tab->entries) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_ERROR, 0, "Unable to allocate entry array!");
        mem_free(tab);
        return NULL;
    }

    tab->total_entries = size;
    tab->used_entries = 0;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, 0, "Done with %" PRId32 " entries.", size);

    return tab;
}


extern void *hashtable_get(hashtable_p table, int64_t key) {
    int32_t index = PLCTAG_ERR_NOT_FOUND;

    if(!table) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Hashtable pointer null or invalid.");
        return NULL;
    }

    index = find_key(table, key);

    return (index < 0 ? NULL : table->entries[index].data);
}


/* The caller makes sure the key is not already present. */
extern int32_t hashtable_put(hashtable_p table, int64_t key, void *data) {
    int32_t index = PLCTAG_ERR_NOT_FOUND;

    if(!table || !data || key == 0) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Null table, null data or zero key!");
        return PLCTAG_ERR_NULL_PTR;
    }

    while((index = find_empty(table->entries, table->total_entries, key)) < 0) {
        int32_t rc = expand_table(table);

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Unable to expand table, error %s!", plc_tag_decode_error(rc));
            return rc;
        }
    }

    table->entries[index].key = key;
    table->entries[index].data = data;
    table->used_entries++;

    return PLCTAG_STATUS_OK;
}


extern void *hashtable_remove(hashtable_p table, int64_t key) {
    int32_t index = PLCTAG_ERR_NOT_FOUND;
    void *result = NULL;

    if(!table) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Hashtable pointer null or invalid.");
        return NULL;
    }

    index = find_key(table, key);
    if(index < 0) { return NULL; }

    result = table->entries[index].data;
    table->entries[index].key = 0;
    table->entries[index].data = NULL;
    table->used_entries--;

    return result;
}


extern int32_t hashtable_capacity(hashtable_p table) {
    if(!table) { return PLCTAG_ERR_NULL_PTR; }

    return table->total_entries;
}


extern void *hashtable_get_index(hashtable_p table, int32_t index) {
    if(!table || index < 0 || index >= table->total_entries) { return NULL; }

    return table->entries[index].data;
}


extern int32_t hashtable_destroy(hashtable_p table) {
    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, 0, "Starting");

    if(!table) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Called with null pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    mem_free(table->entries);
    mem_free(table);

    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, 0, "Done");

    return PLCTAG_STATUS_OK;
}


/* key 0 would match an empty entry, so it is never looked up: put refuses it. */
static int32_t find_key(hashtable_p table, int64_t key) {
    int32_t mask = table->total_entries - 1;
    int32_t home = (int32_t)((uint64_t)key & (uint64_t)mask);

    if(key == 0) { return PLCTAG_ERR_NOT_FOUND; }

    for(int32_t i = 0; i < MAX_PROBES; i++) {
        int32_t index = (home + i) & mask;

        if(table->entries[index].key == key) { return index; }
    }

    return PLCTAG_ERR_NOT_FOUND;
}


static int32_t find_empty(struct hashtable_entry_t *entries, int32_t total_entries, int64_t key) {
    int32_t mask = total_entries - 1;
    int32_t home = (int32_t)((uint64_t)key & (uint64_t)mask);

    for(int32_t i = 0; i < MAX_PROBES; i++) {
        int32_t index = (home + i) & mask;

        if(entries[index].data == NULL) { return index; }
    }

    return PLCTAG_ERR_NOT_FOUND;
}


/* Double until every entry fits within MAX_PROBES of its home. */
static int32_t expand_table(hashtable_p table) {
    int32_t total_entries = table->total_entries;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Table using %" PRId32 " entries of %" PRId32 ".", table->used_entries,
           table->total_entries);

    for(;;) {
        struct hashtable_entry_t *entries = NULL;
        bool all_fit = true;

        if(total_entries > (INT32_MAX / (int32_t)sizeof(struct hashtable_entry_t)) / 2) {
            pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Table cannot grow past %" PRId32 " entries!", total_entries);
            return PLCTAG_ERR_TOO_LARGE;
        }

        total_entries *= 2;

        entries = mem_alloc(total_entries * (int)sizeof(struct hashtable_entry_t));
        if(!entries) {
            pdebug(DEBUG_MODULE_UTILS, DEBUG_ERROR, 0, "Unable to allocate new entry array!");
            return PLCTAG_ERR_NO_MEM;
        }

        for(int32_t i = 0; i < table->total_entries && all_fit; i++) {
            if(table->entries[i].data) {
                int32_t index = find_empty(entries, total_entries, table->entries[i].key);

                if(index < 0) {
                    all_fit = false;
                } else {
                    entries[index] = table->entries[i];
                }
            }
        }

        if(all_fit) {
            mem_free(table->entries);
            table->entries = entries;
            table->total_entries = total_entries;

            pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Table grew to %" PRId32 " entries.", total_entries);

            return PLCTAG_STATUS_OK;
        }

        mem_free(entries);
    }
}
