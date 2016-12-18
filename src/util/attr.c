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
 *   GNU Library General Public License for more details.                  *
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
    char *name;
    char *val;
    attr res = NULL;

    if(!str_length(attr_str)) {
        return NULL;
    }

    /* make a copy for a destructive read. */
    tmp = str_dup(attr_str);
    if(!tmp) {
        return NULL;
    }

    res = attr_create();

    if(!res) {
        mem_free(tmp);
        return NULL;
    }

    /*
     * walk the pointer along the input and copy the
     * names and values along the way.
     */
    cur = tmp;
    while(*cur) {
        /* read the name */
        name = cur;
        while(*cur && *cur != '=')
            cur++;

        /* did we run off the end of the string?
         * That is an error because we need to have a value.
         */
        if(*cur == 0) {
            if(res) attr_destroy(res);
            mem_free(tmp);
            return NULL;
        }

        /* terminate the name string */
        *cur = 0;

        /* read the value */
        cur++;
        val = cur;

        while(*cur && *cur != '&')
            cur++;

        /* we do not care if we ran off the end, much. */
        if(*cur) {
            *cur = 0;
            cur++;
        }

        if(attr_set_str(res, name, val)) {
            if(res) attr_destroy(res);
            mem_free(tmp);
            return NULL;
        }
    }

    mem_free(tmp);

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

