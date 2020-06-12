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

static uint32_t get_uint32(plc_tag_p ptag, int offset);
static int set_uint32(plc_tag_p ptag, int offset, uint32_t val);
static uint8_t get_uint8(plc_tag_p ptag, int offset);

struct tag_vtable_t system_tag_vtable = {
    /* abort */     system_tag_abort,
    /* read */      system_tag_read,
    /* status */    system_tag_status,
    /* tickler */   NULL,
    /* write */     system_tag_write,

    /* data accessors */

    /* get_int_attrib */ NULL,
    /* set_int_attrib */ NULL,

    /* get_bit */ NULL,
    /* set_bit */ NULL,

    /* get_uint64 */ NULL,
    /* set_uint64 */ NULL,

    /* get_int64 */ NULL,
    /* set_int64 */ NULL,

    /* get_uint32 */ get_uint32,
    /* set_uint32 */ set_uint32,

    /* get_int32 */ NULL,
    /* set_int32 */ NULL,

    /* get_uint16 */ NULL,
    /* set_uint16 */ NULL,

    /* get_int16 */ NULL,
    /* set_int16 */ NULL,

    /* get_uint8 */ get_uint8,
    /* set_uint8 */ NULL,

    /* get_int8 */ NULL,
    /* set_int8 */ NULL,

    /* get_float64 */ NULL,
    /* set_float64 */ NULL,

    /* get_float32 */ NULL,
    /* set_float32 */ NULL
};


plc_tag_p system_tag_create(attr attribs)
{
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

    /* get the name and copy it */
    str_copy(tag->name, MAX_SYSTEM_TAG_NAME, name);

    /* point data at the backing store. */
    tag->data = &tag->backing_data[0];
    tag->size = (int)sizeof(tag->backing_data);

    /* set the endian-ness */
    tag->endian = PLCTAG_DATA_LITTLE_ENDIAN;

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

    //mem_free(tag);

    return;
}


static int system_tag_read(plc_tag_p ptag)
{
    system_tag_p tag = (system_tag_p)ptag;

    pdebug(DEBUG_INFO,"Starting.");

    if(!tag) {
        return PLCTAG_ERR_NULL_PTR;
    }

    if(str_cmp_i(&tag->name[0],"version") == 0) {
        pdebug(DEBUG_DETAIL,"Version is %s",VERSION);
        str_copy((char *)(&tag->data[0]), MAX_SYSTEM_TAG_SIZE , VERSION);
        tag->data[str_length(VERSION)] = 0;
        return PLCTAG_STATUS_OK;
    }

    if(str_cmp_i(&tag->name[0],"debug") == 0) {
        int debug_level = get_debug_level();
        tag->data[0] = (uint8_t)(debug_level & 0xFF);
        tag->data[1] = (uint8_t)((debug_level >> 8) & 0xFF);
        tag->data[2] = (uint8_t)((debug_level >> 16) & 0xFF);
        tag->data[3] = (uint8_t)((debug_level >> 24) & 0xFF);
        return PLCTAG_STATUS_OK;
    }

    pdebug(DEBUG_WARN,"Unknown system tag %s", tag->name);
    return PLCTAG_ERR_UNSUPPORTED;
}


static int system_tag_status(plc_tag_p tag)
{
    tag->status = PLCTAG_STATUS_OK;
    return PLCTAG_STATUS_OK;
}


static int system_tag_write(plc_tag_p ptag)
{
    system_tag_p tag = (system_tag_p)ptag;

    if(!tag) {
        return PLCTAG_ERR_NULL_PTR;
    }

    /* the version is static */
    if(str_cmp_i(&tag->name[0],"version") == 0) {
        return PLCTAG_ERR_NOT_IMPLEMENTED;
    }

    if(str_cmp_i(&tag->name[0],"debug") == 0) {
        int res = 0;
        res = (int32_t)(((uint32_t)(tag->data[0])) +
                        ((uint32_t)(tag->data[1]) << 8) +
                        ((uint32_t)(tag->data[2]) << 16) +
                        ((uint32_t)(tag->data[3]) << 24));
        set_debug_level(res);
        return PLCTAG_STATUS_OK;
    }

    pdebug(DEBUG_WARN,"Unknown system tag %s", tag->name);
    return PLCTAG_ERR_NOT_IMPLEMENTED;
}




uint32_t get_uint32(plc_tag_p raw_tag, int offset)
{
    uint32_t res = UINT32_MAX;
    system_tag_p tag = (system_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there enough data */
    if((offset < 0) || (offset + ((int)sizeof(uint32_t)) > tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return res;
    }

    res = ((uint32_t)(tag->data[offset])) +
          ((uint32_t)(tag->data[offset+1]) << 8) +
          ((uint32_t)(tag->data[offset+2]) << 16) +
          ((uint32_t)(tag->data[offset+3]) << 24);

    return res;
}



int set_uint32(plc_tag_p raw_tag, int offset, uint32_t val)
{
    int rc = PLCTAG_STATUS_OK;
    system_tag_p tag = (system_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there enough data */
    if((offset < 0) || (offset + ((int)sizeof(uint32_t)) > tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    /* write the data. */
    tag->data[offset]   = (uint8_t)(val & 0xFF);
    tag->data[offset+1] = (uint8_t)((val >> 8) & 0xFF);
    tag->data[offset+2] = (uint8_t)((val >> 16) & 0xFF);
    tag->data[offset+3] = (uint8_t)((val >> 24) & 0xFF);

    return rc;
}





uint8_t get_uint8(plc_tag_p raw_tag, int offset)
{
    uint8_t res = UINT8_MAX;
    system_tag_p tag = (system_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there enough data */
    if((offset < 0) || (offset + ((int)sizeof(uint8_t)) > tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return res;
    }

    res = tag->data[offset];

    return res;
}


