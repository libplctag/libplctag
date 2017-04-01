/***************************************************************************
 *   Copyright (C) 2017 by Kyle Hayes                                      *
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

#include <platform.h>
#include <util/debug.h>
#include <util/attr.h>
#include <lib/libplctag_tag.h>
#include <lib/libplctag.h>
#include <system/tag.h>
#include <lib/init.h>


/* we'll need to set these per protocol type.
struct tag_vtable_t {
    tag_abort_func          abort;
    tag_destroy_func        destroy;
    tag_read_func           read;
    tag_status_func         status;
    tag_write_func          write;
};
*/

static int system_tag_abort(plc_tag_p tag);
static int system_tag_destroy(plc_tag_p tag);
static int system_tag_read(plc_tag_p tag);
static int system_tag_status(plc_tag_p tag);
static int system_tag_write(plc_tag_p tag);

struct tag_vtable_t system_tag_vtable = { system_tag_abort, system_tag_destroy, system_tag_read, system_tag_status, system_tag_write};


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

    tag = (system_tag_p)mem_alloc(sizeof(struct system_tag_t));

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
    str_copy(tag->name, name, (int)(sizeof(tag->name)));

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



static int system_tag_destroy(plc_tag_p ptag)
{
    system_tag_p tag = (system_tag_p)ptag;

    if(!tag) {
        return PLCTAG_ERR_NULL_PTR;
    }

    mem_free(tag);

    return PLCTAG_STATUS_OK;
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
        str_copy((char *)(&tag->data[0]), VERSION, str_length(VERSION));
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

