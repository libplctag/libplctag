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

/*
 * attr.c
 *
 *  Created on: Nov 27, 2012
 *      Author: Kyle Hayes
 */

#include <util/attr.h>
#include <platform.h>
#include <stdio.h>
#include <string.h>
#include <util/debug.h>




struct attr_entry_t {
    attr_entry next;
    char *name;
    char *val;
};

struct attr_t {
    attr_entry head;
};





/*
 * find_entry
 *
 * A helper function to find the attr_entry that has the
 * passed name.
 */

attr_entry find_entry(attr a, const char *name)
{
    attr_entry e;

    if(!a)
        return NULL;

    e = a->head;

    if(!e)
        return NULL;

    while(e) {
        if(str_cmp(e->name, name) == 0) {
            return e;
        }

        e = e->next;
    }

    return NULL;
}


/*
 * attr_create
 *
 * Create a new attr structure and return a pointer to it.
 */
extern attr attr_create()
{
    return (attr)mem_alloc(sizeof(struct attr_t));
}









/*
 * attr_create_from_str
 *
 * Parse the passed string into an attr structure and return a pointer to a list of
 * attr_entry structs.
 *
 * Attribute strings are formatted much like URL arguments:
 * foo=bar&blah=humbug&blorg=42&test=one
 * You cannot, currently, have an "=" or "&" character in the value for an
 * attribute.
 */
extern attr attr_create_from_str(const char *attr_str)
{
    char *tmp;
    char *cur;
    attr res = NULL;
    char **kv_pairs = NULL;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!str_length(attr_str)) {
        pdebug(DEBUG_WARN, "Attribute string needs to be longer than zero characters!");
        return NULL;
    }

    /* split the string on "&" */
    kv_pairs = str_split(attr_str, "&");
    if(!kv_pairs) {
        pdebug(DEBUG_WARN, "No key-value pairs!");
        return NULL;
    }

    /* set up the attribute list head */
    res = attr_create();
    if(!res) {
        pdebug(DEBUG_ERROR, "Unable to allocate memory for attribute list!");
        mem_free(kv_pairs);
        return NULL;
    }

    /* loop over each key-value pair */
    for(char **kv_pair = kv_pairs; *kv_pair; kv_pair++) {
        /* find the position of the '=' character */
        char *separator = strchr(*kv_pair, '=');
        char *key = *kv_pair;
        char *value = separator;

        pdebug(DEBUG_DETAIL, "Key-value pair \"%s\".", *kv_pair);

        if(separator == NULL) {
            pdebug(DEBUG_WARN, "Attribute string \"%s\" has invalid key-value pair near \"%s\"!", attr_str, *kv_pair);
            mem_free(kv_pairs);
            attr_destroy(res);
            return NULL;
        }

        /* value points to the '=' character.  Step past that for the value. */
        value++;

        /* cut the string at the separator. */
        *separator = (char)0;

        pdebug(DEBUG_DETAIL, "Key-value pair before trimming \"%s\":\"%s\".", key, value);

        /* skip leading spaces in the key */
        while(*key == ' ') {
            key++;
        }

        /* zero out all trailing spaces in the key */
        for(int i=str_length(key) - 1; i > 0 && key[i] == ' '; i--) {
            key[i] = (char)0;
        }

        pdebug(DEBUG_DETAIL, "Key-value pair after trimming \"%s\":\"%s\".", key, value);

        /* check the string lengths */

        if(str_length(key) <= 0) {
            pdebug(DEBUG_WARN, "Attribute string \"%s\" has invalid key-value pair near \"%s\"!  Key must not be zero length!", attr_str, *kv_pair);
            mem_free(kv_pairs);
            attr_destroy(res);
            return NULL;
        }

        if(str_length(value) <= 0) {
            pdebug(DEBUG_WARN, "Attribute string \"%s\" has invalid key-value pair near \"%s\"!  Value must not be zero length!", attr_str, *kv_pair);
            mem_free(kv_pairs);
            attr_destroy(res);
            return NULL;
        }

        /* add the key-value pair to the attribute list */
        if(attr_set_str(res, key, value)) {
            pdebug(DEBUG_WARN, "Unable to add key-value pair \"%s\":\"%s\" to attribute list!", key, value);
            mem_free(kv_pairs);
            attr_destroy(res);
            return NULL;
        }
    }

    if(kv_pairs) {
        mem_free(kv_pairs);
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return res;
}





/*
 * attr_set
 *
 * Set/create a new string attribute
 */
extern int attr_set_str(attr attrs, const char *name, const char *val)
{
    attr_entry e;

    if(!attrs) {
        return 1;
    }

    /* does the entry exist? */
    e = find_entry(attrs, name);

    /* if we had a match, then delete the existing value and add in the
     * new one.
     *
     * If we had no match, then e is NULL and we need to create a new one.
     */
    if(e) {
        /* we had a match, free any existing value */
        if(e->val) {
            mem_free(e->val);
        }

        /* set up the new value */
        e->val = str_dup(val);
        if(!e->val) {
            /* oops! */
            return 1;
        }
    } else {
        /* no match, need a new entry */
        e = (attr_entry)mem_alloc(sizeof(struct attr_entry_t));

        if(e) {
            e->name = str_dup(name);

            if(!e->name) {
                mem_free(e);
                return 1;
            }

            e->val = str_dup(val);

            if(!e->val) {
                mem_free(e->name);
                mem_free(e);
                return 1;
            }

            /* link it in the list */
            e->next = attrs->head;
            attrs->head = e;
        } else {
            /* allocation failed */
            return 1;
        }
    }

    return 0;
}



extern int attr_set_int(attr attrs, const char *name, int val)
{
    char buf[64];

    snprintf_platform(buf, sizeof buf, "%d", val);

    return attr_set_str(attrs, name, buf);
}



extern int attr_set_float(attr attrs, const char *name, float val)
{
    char buf[64];

    snprintf_platform(buf, sizeof buf, "%f", val);

    return attr_set_str(attrs, name, buf);
}





/*
 * attr_get
 *
 * Walk the list of attrs and return the value found with the passed name.
 * If the name is not found, return the passed default value.
 */
extern const char *attr_get_str(attr attrs, const char *name, const char *def)
{
    attr_entry e;

    if(!attrs) {
        return def;
    }

    e = find_entry(attrs, name);

    /* only return a value if there is one. */
    if(e) {
        return e->val;
    } else {
        return def;
    }
}


extern int attr_get_int(attr attrs, const char *name, int def)
{
    int res;
    int rc;

    const char *str_val = attr_get_str(attrs,name, NULL);

    if(!str_val) {
        return def;
    }

    rc = str_to_int(str_val, &res);

    if(rc) {
        /* format error? */
        return def;
    } else {
        return res;
    }
}


extern float attr_get_float(attr attrs, const char *name, float def)
{
    float res;
    int rc;

    const char *str_val = attr_get_str(attrs,name, NULL);

    if(!str_val) {
        return def;
    }

    rc = str_to_float(str_val, &res);

    if(rc) {
        /* format error? */
        return def;
    } else {
        return res;
    }
}


extern int attr_remove(attr attrs, const char *name)
{
    attr_entry e, p;

    if(!attrs)
        return 0;

    e = attrs->head;

    /* no such entry, return */
    if(!e)
        return 0;

    /* loop to find the entry */
    p = NULL;

    while(e) {
        if(str_cmp(e->name, name) == 0) {
            break;
        }

        p = e;
        e = e->next;
    }

    if(e) {
        /* unlink the node */
        if(!p) {
            attrs->head = e->next;
        } else {
            p->next = e->next;
        }

        if(e->name) {
            mem_free(e->name);
        }

        if(e->val) {
            mem_free(e->val);
        }

        mem_free(e);
    } /* else not found */

    return 0;
}


/*
 * attr_delete
 *
 * Destroy and free all memory for an attribute list.
 */
extern void attr_destroy(attr a)
{
    attr_entry e, p;

    if(!a)
        return;

    e = a->head;

    /* walk down the entry list and free as we go. */
    while(e) {
        if(e->name) {
            mem_free(e->name);
        }

        if(e->val) {
            mem_free(e->val);
        }

        p = e;
        e = e->next;

        mem_free(p);
    }

    mem_free(a);
}
