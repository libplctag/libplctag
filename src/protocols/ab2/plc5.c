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
#include <ab2/plc5.h>
#include <ab2/common_defs.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/mem.h>
#include <util/string.h>

typedef struct {
    struct plc_tag_t base_tag;

    uint16_t elem_size;
    uint16_t elem_count;
} ab2_plc5_tag_t;
typedef ab2_plc5_tag_t *ab2_plc5_tag_p;


static void plc5_tag_destroy(void *tag_arg);

static int plc5_tag_abort(plc_tag_p tag);
static int plc5_tag_read(plc_tag_p tag);
static int plc5_tag_status(plc_tag_p tag);
static int plc5_tag_tickler(plc_tag_p tag);
static int plc5_tag_write(plc_tag_p tag);
static int plc5_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value);
static int plc5_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value);


/* vtables for different kinds of tags */
static struct tag_vtable_t plc5_vtable = {
    plc5_tag_abort,
    plc5_tag_read,
    plc5_tag_status,
    plc5_tag_tickler,
    plc5_tag_write,

    /* attribute accessors */
    plc5_get_int_attrib,
    plc5_set_int_attrib
};


plc_tag_p ab2_plc5_tag_create(attr attribs)
{
    ab2_plc5_tag_p tag = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    tag = (ab2_plc5_tag_p)base_tag_create(sizeof(*tag), (void (*)(void*))plc5_tag_destroy);
    if(!tag) {
        pdebug(DEBUG_WARN, "Unable to allocate new PLC/5 tag!");
        return NULL;
    }

    /* set the vtable for base functions. */
    tag->base_tag.vtable = &plc5_vtable;

    pdebug(DEBUG_INFO, "Done.");

    return (plc_tag_p)tag;
}


/* helper functions. */
void plc5_tag_destroy(void *tag_arg)
{
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer passed to destructor!");
        return;
    }

    /* delete the base tag parts. */
    base_tag_destroy((plc_tag_p)tag);

    /* get rid of any outstanding timers and events. */


    pdebug(DEBUG_INFO, "Done.");
}


int plc5_tag_abort(plc_tag_p tag_arg)
{
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(tag->base_tag.read_in_flight || tag->base_tag.write_in_flight) {
        /* abort outstanding request. */
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


int plc5_tag_read(plc_tag_p tag_arg)
{
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    //protocol_queue_request(tag->request, tag, )

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}


int plc5_tag_status(plc_tag_p tag)
{
    pdebug(DEBUG_INFO, "Starting.");

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_ERR_UNSUPPORTED;
}


int plc5_tag_tickler(plc_tag_p tag)
{
    pdebug(DEBUG_INFO, "Starting.");

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_ERR_UNSUPPORTED;
}


int plc5_tag_write(plc_tag_p tag)
{
    pdebug(DEBUG_INFO, "Starting.");

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_ERR_UNSUPPORTED;
}


int plc5_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value)
{
    int res = default_value;
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)raw_tag;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* assume we have a match. */
    tag->base_tag.status = PLCTAG_STATUS_OK;

    /* match the attribute. */
    if(str_cmp_i(attrib_name, "elem_size") == 0) {
        res = tag->elem_size;
    } else if(str_cmp_i(attrib_name, "elem_count") == 0) {
        res = tag->elem_count;
    } else {
        pdebug(DEBUG_WARN, "Unsupported attribute name \"%s\"!", attrib_name);
        tag->base_tag.status = PLCTAG_ERR_UNSUPPORTED;
        return default_value;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return res;
}


int plc5_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value)
{
    (void)attrib_name;
    (void)new_value;

    pdebug(DEBUG_WARN, "Unsupported attribute \"%s\"!", attrib_name);

    raw_tag->status  = PLCTAG_ERR_UNSUPPORTED;

    return PLCTAG_ERR_UNSUPPORTED;
}


