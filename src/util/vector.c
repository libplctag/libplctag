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
#include <util/mem.h>
#include <util/vector.h>

struct vector_t {
    int len;
    int capacity;
    int max_inc;
    void **data;
};



static int ensure_capacity(vector_p vec, int capacity);


vector_p vector_create(int capacity, int max_inc)
{
    vector_p vec = NULL;

    pdebug(DEBUG_SPEW,"Starting");

    if(capacity <= 0) {
        pdebug(DEBUG_WARN, "Called with negative capacity!");
        return NULL;
    }

    if(max_inc <= 0) {
        pdebug(DEBUG_WARN, "Called with negative maximum size increment!");
        return NULL;
    }

    vec = mem_alloc((int)sizeof(struct vector_t));
    if(!vec) {
        pdebug(DEBUG_ERROR,"Unable to allocate memory for vector!");
        return NULL;
    }

    vec->len = 0;
    vec->capacity = capacity;
    vec->max_inc = max_inc;

    vec->data = mem_alloc(capacity * (int)sizeof(void *));
    if(!vec->data) {
        pdebug(DEBUG_ERROR,"Unable to allocate memory for vector data!");
        vector_destroy(vec);
        return NULL;
    }

    pdebug(DEBUG_SPEW,"Done");

    return vec;
}



int vector_length(vector_p vec)
{
    pdebug(DEBUG_SPEW,"Starting");

    /* check to see if the vector ref is valid */
    if(!vec) {
        pdebug(DEBUG_WARN,"Null pointer or invalid pointer to vector passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_SPEW,"Done");

    return vec->len;
}



int vector_put(vector_p vec, int index, void * data)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW,"Starting");

    /* check to see if the vector ref is valid */
    if(!vec) {
       pdebug(DEBUG_WARN,"Null pointer or invalid pointer to vector passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(index < 0) {
        pdebug(DEBUG_WARN,"Index is negative!");
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    rc = ensure_capacity(vec, index+1);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"Unable to ensure capacity!");
        return rc;
    }

    /* reference the new data. */
    vec->data[index] = data;

    /* adjust the length, if needed */
    if(index >= vec->len) {
        vec->len = index+1;
    }

    pdebug(DEBUG_SPEW,"Done");

    return rc;
}


void * vector_get(vector_p vec, int index)
{
    pdebug(DEBUG_SPEW,"Starting");

    /* check to see if the vector ref is valid */
    if(!vec) {
        pdebug(DEBUG_WARN,"Null pointer or invalid pointer to vector passed!");
        return NULL;
    }

    if(index < 0 || index >= vec->len) {
        pdebug(DEBUG_WARN,"Index is out of bounds!");
        return NULL;
    }

    pdebug(DEBUG_SPEW,"Done");

    return vec->data[index];
}


void * vector_remove(vector_p vec, int index)
{
    void * result = NULL;

    pdebug(DEBUG_SPEW,"Starting");

    /* check to see if the vector ref is valid */
    if(!vec) {
        pdebug(DEBUG_WARN,"Null pointer or invalid pointer to vector passed!");
        return NULL;
    }

    if(index < 0 || index >= vec->len) {
        pdebug(DEBUG_WARN,"Index is out of bounds!");
        return NULL;
    }

    /* get the value in this slot before we overwrite it. */
    result = vec->data[index];

    /* move the rest of the data over this. */
    mem_move(&vec->data[index], &vec->data[index+1], (int)((sizeof(void *) * (size_t)(vec->len - index - 1))));

    /* make sure that we do not have old data hanging around. */
    vec->data[vec->len - 1] = NULL;

    /* adjust the length to the new size */
    vec->len--;

    pdebug(DEBUG_SPEW,"Done");

    return result;
}



int vector_destroy(vector_p vec)
{
    pdebug(DEBUG_SPEW,"Starting.");

    if(!vec) {
        pdebug(DEBUG_WARN,"Null pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    mem_free(vec->data);
    mem_free(vec);

    pdebug(DEBUG_SPEW,"Done.");

    return PLCTAG_STATUS_OK;
}



/***********************************************************************
 *************** Private Helper Functions ******************************
 **********************************************************************/



int ensure_capacity(vector_p vec, int capacity)
{
    int new_inc = 0;
    void * *new_data = NULL;

    if(!vec) {
        pdebug(DEBUG_WARN,"Null pointer or invalid pointer to vector passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* is there anything to do? */
    if(capacity <= vec->capacity) {
        /* release the reference */
        return PLCTAG_STATUS_OK;
    }

    /* calculate the new capacity
     *
     * Start by guessing 50% larger.  Clamp that against 1 at the
     * low end and the max increment passed when the vector was created.
     */
    new_inc = vec->capacity / 2;

    if(new_inc > vec->max_inc) {
        new_inc = vec->max_inc;
    }

    if(new_inc < 1) {
        new_inc = 1;
    }

    /* allocate the new data area */
    new_data = (void * *)mem_alloc((int)((sizeof(void *) * (size_t)(vec->capacity + new_inc))));
    if(!new_data) {
        pdebug(DEBUG_ERROR,"Unable to allocate new data area!");
        return PLCTAG_ERR_NO_MEM;
    }

    mem_copy(new_data, vec->data, (int)((size_t)(vec->capacity) * sizeof(void *)));

    mem_free(vec->data);

    vec->data = new_data;

    vec->capacity += new_inc;

    return PLCTAG_STATUS_OK;
}


