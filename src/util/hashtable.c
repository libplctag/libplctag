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



struct hashtable_t {
    int bucket_size;
    vector_p buckets;
};


struct hashtable_entry_t {
    void *data;
    int key_len;
    void *key;
};


typedef struct hashtable_entry_t *hashtable_entry_p;

static int find_entry(hashtable_p table, void *key, int key_len, uint32_t *bucket_index, vector_p *bucket, uint32_t *index, hashtable_entry_p *entry_ref);
static int entry_cmp(hashtable_entry_p entry_ref, void *key, int key_len);
static hashtable_entry_p hashtable_entry_create(void *key, int key_len, void * data_ref);
void hashtable_entry_destroy(int num_args, void **args);





hashtable_p hashtable_create(int size)
{
    hashtable_p tab = NULL;
    vector_p bucket = NULL;
    hashtable_p res = NULL;

    pdebug(DEBUG_INFO,"Starting");

    if(size <= 0) {
        pdebug(DEBUG_WARN,"Size is less than or equal to zero!");
        return res;
    }

    tab = mem_alloc(sizeof(struct hashtable_t));
    if(!tab) {
        pdebug(DEBUG_ERROR,"Unable to allocate memory for hash table!");
        return res;
    }

    tab->bucket_size = size;
    tab->buckets = vector_create(size, 1);
    if(!tab->buckets) {
        pdebug(DEBUG_ERROR,"Unable to allocate memory for bucket vector!");
        hashtable_destroy(tab);
        return NULL;
    }

    for(int i=0; i < size; i++) {
        bucket = vector_create(5, 5); /* FIXME - MAGIC */
        if(!bucket) {
            pdebug(DEBUG_ERROR,"Unable to allocate memory for bucket %d!",i);
            hashtable_destroy(tab);
            return NULL;
        } else {
            /* store the bucket. */
            vector_put(tab->buckets, i, bucket);
        }
    }

    pdebug(DEBUG_INFO,"Done");

    return tab;
}


void * hashtable_get(hashtable_p table, void *key, int key_len)
{
    uint32_t bucket_index = 0;
    vector_p bucket = NULL;
    uint32_t entry_index = 0;
    hashtable_entry_p entry = NULL;
    void *result = NULL;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW,"Starting");

    if(!table) {
        pdebug(DEBUG_WARN,"Hashtable pointer null or invalid.");
        return NULL;
    }

    if(!key || key_len <=0) {
        pdebug(DEBUG_WARN,"Key missing or of zero length.");
        return NULL;
    }

    rc = find_entry(table, key, key_len, &bucket_index, &bucket, &entry_index, &entry);
    if(rc == PLCTAG_STATUS_OK) {
        /* found */
        result = entry->data;
    }

    pdebug(DEBUG_SPEW,"Done");

    return result;
}




int hashtable_put(hashtable_p table, void *key, int key_len, void  *data)
{
    uint32_t bucket_index = 0;
    vector_p bucket = NULL;
    uint32_t entry_index = 0;
    hashtable_entry_p entry;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW,"Starting");

    if(!table) {
        pdebug(DEBUG_WARN,"Hashtable pointer null or invalid.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!key || key_len <=0) {
        pdebug(DEBUG_WARN,"Key missing or of zero length.");
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    rc = find_entry(table, key, key_len, &bucket_index, &bucket, &entry_index, &entry);
    if(rc == PLCTAG_ERR_NOT_FOUND) {
        /* not found, this is good. */

        /* create a new entry and insert it into the bucket. */
        entry = hashtable_entry_create(key, key_len, data);
        if(!entry) {
            pdebug(DEBUG_ERROR,"Unable to allocate new hashtable entry!");
            rc = PLCTAG_ERR_NO_MEM;
        } else {
            /* put the new entry at the end of the bucket vector */
            rc = vector_put(bucket, vector_length(bucket), entry);
        }
    } else {
        /* catch the case that the key is already in the hashtable. */
        if(rc == PLCTAG_STATUS_OK) {
            rc = PLCTAG_ERR_DUPLICATE;
        }
    }

    pdebug(DEBUG_SPEW,"Done");

    return rc;
}



int hashtable_on_each(hashtable_p table, int (*callback_func)(hashtable_p table, void *key, int key_len, void *data))
{
    int rc = PLCTAG_STATUS_OK;

    if(!table) {
        pdebug(DEBUG_WARN,"Hashtable pointer null or invalid");
    }

    for(int bucket_index=0; rc == PLCTAG_STATUS_OK && bucket_index < vector_length(table->buckets); bucket_index++) {
        vector_p bucket = vector_get(table->buckets, bucket_index);
        for(int entry_index=0; rc == PLCTAG_STATUS_OK && entry_index < vector_length(bucket); entry_index++) {
            hashtable_entry_p entry = vector_get(bucket, entry_index);

            if(entry) {
                rc = callback_func(table, entry->key, entry->key_len, entry->data);
            }
        }
    }

    return rc;
}



void * hashtable_remove(hashtable_p table, void *key, int key_len)
{
    uint32_t bucket_index = 0;
    vector_p bucket = NULL;
    uint32_t entry_index = 0;
    hashtable_entry_p entry = NULL;
    int rc = PLCTAG_STATUS_OK;
    void * result = NULL;

    pdebug(DEBUG_SPEW,"Starting");

    if(!table) {
        pdebug(DEBUG_WARN,"Hashtable pointer null or invalid.");
        return result;
    }

    if(!key || key_len <=0) {
        pdebug(DEBUG_WARN,"Key missing or of zero length.");
        return result;
    }

    rc = find_entry(table, key, key_len, &bucket_index, &bucket, &entry_index, &entry);
    if(rc == PLCTAG_STATUS_OK) {
        entry = vector_remove(bucket, entry_index);

        if(entry) {
            result = entry->data;

            /* clear out the data reference so that it does not get cleaned up. */
            entry->data = NULL;

            mem_free(entry);
        }
    }

    pdebug(DEBUG_SPEW,"Done");

    return result;
}




int hashtable_destroy(hashtable_p table)
{
    pdebug(DEBUG_INFO,"Starting");

    if(!table) {
        pdebug(DEBUG_WARN,"Called with null pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    for(int i=0; i < vector_length(table->buckets); i++) {
        vector_p bucket = vector_get(table->buckets,i);
        if(bucket) {
            /* clean out the bucket */
            for(int j=0; j < vector_length(bucket); j++) {
                hashtable_entry_p entry = vector_get(bucket, j);

                if(entry) {
                    mem_free(entry);
                }
            }

            /* free the bucket */
            vector_destroy(bucket);
        }
    }

    vector_destroy(table->buckets);

    mem_free(table);

    pdebug(DEBUG_INFO,"Done");

    return PLCTAG_STATUS_OK;
}





/***********************************************************************
 *************************** Helper Functions **************************
 **********************************************************************/




int find_entry(hashtable_p table, void *key, int key_len, uint32_t *bucket_index, vector_p *bucket, uint32_t *index, hashtable_entry_p *entry)
{
    int rc = PLCTAG_ERR_NOT_FOUND;

    pdebug(DEBUG_SPEW,"Starting");

    /* get the right bucket */
    *bucket_index = hash(key, key_len, table->bucket_size) % (uint32_t)table->bucket_size;
    *bucket = vector_get(table->buckets, *bucket_index);

    if(!*bucket) {
        pdebug(DEBUG_ERROR,"Bucket is NULL!");
        return PLCTAG_ERR_NO_DATA;
    }

    /* find the entry */
    for(*index=0; rc==PLCTAG_ERR_NOT_FOUND && (int)(*index) < vector_length(*bucket); *index = *index + 1) {
        *entry = vector_get(*bucket, *index);
        if(*entry) {
            if(entry_cmp(*entry, key, key_len) == 0) {
                rc = PLCTAG_STATUS_OK;
                break; /* punt out and do not increment index. */
            }
        }
    }

    pdebug(DEBUG_SPEW,"Done.");

    return rc;
}



int entry_cmp(hashtable_entry_p entry, void *key, int key_len)
{
    if(!entry) {
        pdebug(DEBUG_WARN,"Null entry");
        return -1;
    }

    if(!key || key_len <= 0) {
        pdebug(DEBUG_WARN,"Bad key");
        return -1;
    }

    return mem_cmp(entry->key, entry->key_len, key, key_len);
}



hashtable_entry_p hashtable_entry_create(void *key, int key_len, void *data)
{
    hashtable_entry_p new_entry = mem_alloc(sizeof(struct hashtable_entry_t) + key_len);

    if(!new_entry) {
        pdebug(DEBUG_ERROR,"Unable to allocate new hashtable entry!");
        return NULL;
    } else {
        new_entry->data = data;
        new_entry->key_len = key_len;
        new_entry->key = (void *)(new_entry + 1);
        mem_copy(new_entry->key, key, key_len);
        pdebug(DEBUG_INFO,"Done creating new hashtable entry.");
    }

    return new_entry;
}

