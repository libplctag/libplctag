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

#include <platform.h>
#include <util/debug.h>
#include <util/attr.h>
#include <lib/tag.h>
#include <lib/libplctag.h>
#include <lib/version.h>
#include <system/tag.h>
#include <lib/init.h>
#include <util/rc.h>



static void system_tag_destroy(plc_tag_p tag);


static int system_tag_abort(plc_tag_p tag);
static int system_tag_read(plc_tag_p tag);
static int system_tag_status(plc_tag_p tag);
static int system_tag_write(plc_tag_p tag);

struct tag_vtable_t system_tag_vtable = {
    /* abort */     system_tag_abort,
    /* read */      system_tag_read,
    /* status */    system_tag_status,
    /* tickler */   NULL,
    /* write */     system_tag_write,
    /* wake_plc */  (tag_vtable_func)NULL,

    /* data accessors */

    /* get_int_attrib */ NULL,
    /* set_int_attrib */ NULL

};

tag_byte_order_t system_tag_byte_order = {
    .is_allocated = 0,

    .int16_order = {0,1},
    .int32_order = {0,1,2,3},
    .int64_order = {0,1,2,3,4,5,6,7},
    .float32_order = {0,1,2,3},
    .float64_order = {0,1,2,3,4,5,6,7},

    .str_is_defined = 1,
    .str_is_counted = 0,
    .str_is_fixed_length = 0,
    .str_is_zero_terminated = 1, /* C-style string. */
    .str_is_byte_swapped = 0,

    .str_count_word_bytes = 0,
    .str_max_capacity = 0,
    .str_total_length = 0,
    .str_pad_bytes = 0
};



plc_tag_p system_tag_create(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata), void *userdata)
{
    int rc = PLCTAG_STATUS_OK;
    system_tag_p tag = NULL;
    const char *name = attr_get_str(attribs, "name", NULL);

    pdebug(DEBUG_INFO,"Starting.");

    /* check the name, if none given, punt. */
    if(!name || str_length(name) < 1) {
        pdebug(DEBUG_ERROR, "System tag name is empty or missing!");
        return PLC_TAG_P_NULL;
    }

    pdebug(DEBUG_DETAIL,"Creating special tag %s", name);

    /*
     * allocate memory for the new tag.  Do this first so that
     * we have a vehicle for returning status.
     */

    tag = (system_tag_p)rc_alloc(sizeof(struct system_tag_t), (rc_cleanup_func)system_tag_destroy);

    if(!tag) {
        pdebug(DEBUG_ERROR,"Unable to allocate memory for system tag!");
        return PLC_TAG_P_NULL;
    }

    /*
     * we got far enough to allocate memory, set the default vtable up
     * in case we need to abort later.
     */
    tag->vtable = &system_tag_vtable;

    /* set up the generic parts. */
    rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to initialize generic tag parts!");
        rc_dec(tag);
        return (plc_tag_p)NULL;
    }

    /* set the byte order. */
    tag->byte_order = &system_tag_byte_order;

    /* get the name and copy it */
    str_copy(tag->name, MAX_SYSTEM_TAG_NAME - 1, name);
    tag->name[MAX_SYSTEM_TAG_NAME - 1] = '\0';

    /* point data at the backing store. */
    tag->data = &tag->backing_data[0];
    tag->size = (int)sizeof(tag->backing_data);

    pdebug(DEBUG_INFO,"Done");

    return (plc_tag_p)tag;
}


static int system_tag_abort(plc_tag_p tag)
{
    /* there are no outstanding operations, so everything is OK. */
    tag->status = PLCTAG_STATUS_OK;
    return PLCTAG_STATUS_OK;
}



static void system_tag_destroy(plc_tag_p ptag)
{
    system_tag_p tag = (system_tag_p)ptag;

    if(!tag) {
        return;
    }

    if(ptag->ext_mutex) {
        mutex_destroy(&ptag->ext_mutex);
    }

    if(ptag->api_mutex) {
        mutex_destroy(&ptag->api_mutex);
    }

    if(ptag->tag_cond_wait) {
        cond_destroy(&ptag->tag_cond_wait);
    }

    if(tag->byte_order && tag->byte_order->is_allocated) {
        mem_free(tag->byte_order);
        tag->byte_order = NULL;
    }

    return;
}


static int system_tag_read(plc_tag_p ptag)
{
    system_tag_p tag = (system_tag_p)ptag;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO,"Starting.");

    if(!tag) {
        return PLCTAG_ERR_NULL_PTR;
    }

    if(str_cmp_i(&tag->name[0],"version") == 0) {
        pdebug(DEBUG_DETAIL,"Version is %s",VERSION);
        str_copy((char *)(&tag->data[0]), MAX_SYSTEM_TAG_SIZE , VERSION);
        tag->data[str_length(VERSION)] = 0;
        rc = PLCTAG_STATUS_OK;
    } else if(str_cmp_i(&tag->name[0],"debug") == 0) {
        int debug_level = get_debug_level();
        tag->data[0] = (uint8_t)(debug_level & 0xFF);
        tag->data[1] = (uint8_t)((debug_level >> 8) & 0xFF);
        tag->data[2] = (uint8_t)((debug_level >> 16) & 0xFF);
        tag->data[3] = (uint8_t)((debug_level >> 24) & 0xFF);
        rc = PLCTAG_STATUS_OK;
    } else {
        pdebug(DEBUG_WARN, "Unsupported system tag %s!", tag->name);
        rc = PLCTAG_ERR_UNSUPPORTED;
    }

    /* safe here because we are still within the API mutex. */
    tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_READ_STARTED, PLCTAG_STATUS_OK);
    tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_READ_COMPLETED, PLCTAG_STATUS_OK);
    plc_tag_generic_handle_event_callbacks((plc_tag_p)tag);

    pdebug(DEBUG_INFO,"Done.");

    return rc;
}


static int system_tag_status(plc_tag_p tag)
{
    tag->status = PLCTAG_STATUS_OK;
    return PLCTAG_STATUS_OK;
}


static int system_tag_write(plc_tag_p ptag)
{
    int rc = PLCTAG_STATUS_OK;
    system_tag_p tag = (system_tag_p)ptag;

    if(!tag) {
        return PLCTAG_ERR_NULL_PTR;
    }

    /* raise this here so that the callback can update the tag buffer. */
    tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_WRITE_STARTED, PLCTAG_STATUS_PENDING);
    plc_tag_generic_handle_event_callbacks((plc_tag_p)tag);

    /* the version is static */
    if(str_cmp_i(&tag->name[0],"debug") == 0) {
        int res = 0;
        res = (int32_t)(((uint32_t)(tag->data[0])) +
                        ((uint32_t)(tag->data[1]) << 8) +
                        ((uint32_t)(tag->data[2]) << 16) +
                        ((uint32_t)(tag->data[3]) << 24));
        set_debug_level(res);
        rc = PLCTAG_STATUS_OK;
    } else if(str_cmp_i(&tag->name[0],"version") == 0) {
        rc = PLCTAG_ERR_NOT_IMPLEMENTED;
    } else {
        pdebug(DEBUG_WARN, "Unsupported system tag %s!", tag->name);
        rc = PLCTAG_ERR_UNSUPPORTED;
    }

    tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_WRITE_COMPLETED, PLCTAG_STATUS_OK);
    plc_tag_generic_handle_event_callbacks((plc_tag_p)tag);

    pdebug(DEBUG_INFO,"Done.");

    return rc;
}

