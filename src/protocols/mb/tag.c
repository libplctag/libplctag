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


#include <lib/libplctag.h>
#include <lib/tag.h>
#include <platform.h>
#include <mb/tag.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/rc.h>


/* local functions. */
static size_t calc_size(attr attribs);
static void mb_tag_destroy(void *mb_tag);



plc_tag_p mb_tag_create(attr attribs)
{
    int rc = PLCTAG_STATUS_OK;
    size_t tag_size = 0;
    mb_tag_p tag = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    /* calculate tag size */
    tag_size = calc_size(attribs);
    if(tag_size == 0) {
        pdebug(DEBUG_WARN, "Tag size is zero, check tag string!");
        return NULL;
    }

    /* allocate the memory for the new tag. */
    tag = (mb_tag_p)rc_alloc((int)(unsigned int)(sizeof(struct mb_tag_t) + tag_size), mb_tag_destroy);
    if(!tag) {
        pdebug(DEBUG_ERROR, "Unable to allocate memory for new Modbus tag!");
        return NULL;
    }

    /* tag data is immediately past the end of the tag struct. */
    tag->data = (uint8_t *)(tag + 1);

    pdebug(DEBUG_INFO, "Done.");

    return (plc_tag_p)tag;
}


/************ Helper Functions *************/

size_t calc_size(attr attribs)
{
    size_t elem_count, elem_size;

    pdebug(DEBUG_DETAIL, "Starting.");

    elem_count = (size_t)(unsigned int)attr_get_int(attribs, "elem_count", 1);
    elem_size = (size_t)(unsigned int)attr_get_int(attribs, "elem_size", 0);
    if(elem_size == 0) {
        pdebug(DEBUG_WARN, "Element size must be set with elem_size!");
        return 0;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return elem_count * elem_size;
}



void mb_tag_destroy(void *tag_arg)
{
    mb_tag_p tag = (mb_tag_p)tag_arg;

    (void)tag;

    pdebug(DEBUG_INFO, "Starting.");

    pdebug(DEBUG_INFO, "Done.");
}
