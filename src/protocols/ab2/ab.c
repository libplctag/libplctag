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
#include <lib/tag.h>
#include <ab2/ab.h>
#include <ab2/df1.h>
#include <ab2/pccc_eip_plc.h>
#include <ab2/plc5_tag.h>
#include <ab2/slc_tag.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/mem.h>
#include <util/string.h>


//static plc_type_t ab2_get_plc_type(attr attribs);

static plc_tag_p pccc_tag_create(ab2_plc_type_t plc_type, attr attribs);
static void pccc_tag_destroy(void *tag_arg);

plc_tag_p ab2_tag_create(attr attribs)
{
    plc_tag_p result = NULL;
    ab2_plc_type_t plc_type;

    pdebug(DEBUG_INFO, "Starting.");

    plc_type = ab2_get_plc_type(attribs);

    switch(plc_type) {
        case AB2_PLC_PLC5:
            /* fall through */
        case AB2_PLC_SLC:
            /* fall through */
        case AB2_PLC_MLGX:
            result = pccc_tag_create(plc_type, attribs);
            break;

        default:
            pdebug(DEBUG_WARN, "Unknown PLC type!");
            result = NULL;
            break;
    }

    pdebug(DEBUG_INFO, "Done.");

    return result;
}




ab2_plc_type_t ab2_get_plc_type(attr attribs)
{
    const char *cpu_type = attr_get_str(attribs, "plc", attr_get_str(attribs, "cpu", "NONE"));

    if (!str_cmp_i(cpu_type, "plc") || !str_cmp_i(cpu_type, "plc5")) {
        pdebug(DEBUG_DETAIL,"Found PLC/5 PLC.");
        return AB2_PLC_PLC5;
    } else if ( !str_cmp_i(cpu_type, "slc") || !str_cmp_i(cpu_type, "slc500")) {
        pdebug(DEBUG_DETAIL,"Found SLC 500 PLC.");
        return AB2_PLC_SLC;
    } else if (!str_cmp_i(cpu_type, "lgxpccc") || !str_cmp_i(cpu_type, "logixpccc") || !str_cmp_i(cpu_type, "lgxplc5") || !str_cmp_i(cpu_type, "logixplc5") ||
               !str_cmp_i(cpu_type, "lgx-pccc") || !str_cmp_i(cpu_type, "logix-pccc") || !str_cmp_i(cpu_type, "lgx-plc5") || !str_cmp_i(cpu_type, "logix-plc5")) {
        pdebug(DEBUG_DETAIL,"Found Logix-class PLC using PCCC protocol.");
        return AB2_PLC_LGX_PCCC;
    } else if (!str_cmp_i(cpu_type, "micrologix800") || !str_cmp_i(cpu_type, "mlgx800") || !str_cmp_i(cpu_type, "micro800")) {
        pdebug(DEBUG_DETAIL,"Found Micro8xx PLC.");
        return AB2_PLC_MLGX800;
    } else if (!str_cmp_i(cpu_type, "micrologix") || !str_cmp_i(cpu_type, "mlgx")) {
        pdebug(DEBUG_DETAIL,"Found MicroLogix PLC.");
        return AB2_PLC_MLGX;
    } else if (!str_cmp_i(cpu_type, "compactlogix") || !str_cmp_i(cpu_type, "clgx") || !str_cmp_i(cpu_type, "lgx") ||
               !str_cmp_i(cpu_type, "controllogix") || !str_cmp_i(cpu_type, "contrologix") ||
               !str_cmp_i(cpu_type, "logix")) {
        pdebug(DEBUG_DETAIL,"Found ControlLogix/CompactLogix PLC.");
        return AB2_PLC_LGX;
    } else if (!str_cmp_i(cpu_type, "omron-njnx") || !str_cmp_i(cpu_type, "omron-nj") || !str_cmp_i(cpu_type, "omron-nx") || !str_cmp_i(cpu_type, "njnx")
               || !str_cmp_i(cpu_type, "nx1p2")) {
        pdebug(DEBUG_DETAIL,"Found OMRON NJ/NX Series PLC.");
        return AB2_PLC_OMRON_NJNX;
    } else {
        pdebug(DEBUG_WARN, "Unsupported device type: %s", cpu_type);

        return AB2_PLC_NONE;
    }
}



plc_tag_p pccc_tag_create(ab2_plc_type_t plc_type, attr attribs)
{
    int rc = PLCTAG_STATUS_OK;
    pccc_tag_p tag = NULL;
    const char *tag_name = NULL;
    int tmp = 0;
    bool is_bit = FALSE;
    uint8_t bit_num = 0;

    pdebug(DEBUG_INFO, "Starting.");

    tag = (pccc_tag_p)base_tag_create(sizeof(*tag), (void (*)(void*))pccc_tag_destroy);
    if(!tag) {
        pdebug(DEBUG_WARN, "Unable to allocate new PLC/5 tag!");
        return NULL;
    }

    /* parse the PLC-5 tag name */
    tag_name = attr_get_str(attribs, "name", NULL);
    if(!tag_name) {
        pdebug(DEBUG_WARN, "Data file name and offset missing!");
        rc_dec(tag);
        return NULL;
    }

    rc = df1_parse_logical_address(tag_name, &(tag->data_file_type),&(tag->data_file_num), &(tag->data_file_elem), &(tag->data_file_sub_elem), &is_bit, &bit_num);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Malformed data file name!");
        rc_dec(tag);
        return NULL;
    }

    /* if the tag is a bit tag, then fill in the parent tag structure. */
    if(is_bit == TRUE) {
        tag->base_tag.is_bit = 1;
        tag->base_tag.bit = bit_num;
    }

    /* set up the tag data buffer. This involves getting the element size and element count. */
    tmp = df1_element_size(tag->data_file_type);
    if(tmp < 0) {
        pdebug(DEBUG_WARN, "Unsupported or unknown data file type, got error %s converting data file type to element size!", plc_tag_decode_error(tag->elem_size));
        rc_dec(tag);
        return NULL;
    }

    /* see if the user overrode the element size */
    tag->elem_size = (uint16_t)attr_get_int(attribs, "elem_size", tmp);

    if(tag->elem_size == 0) {
        pdebug(DEBUG_WARN, "Data file type unsupported or unknown, unable to determine element size automatically!");
        rc_dec(tag);
        return NULL;
    }

    tmp = attr_get_int(attribs, "elem_count", 1);
    if(tmp < 1) {
        pdebug(DEBUG_WARN, "Element count must be greater than zero or missing and will default to one!");
        rc_dec(tag);
        return NULL;
    }

    tag->elem_count = (uint16_t)tmp;

    tag->base_tag.size = tag->elem_count * tag->elem_size;

    if(tag->base_tag.size <= 0) {
        pdebug(DEBUG_WARN, "Tag size must be a positive number of bytes!");
        rc_dec(tag);
        return NULL;
    }

    tag->base_tag.data = mem_alloc(tag->base_tag.size);
    if(!tag->base_tag.data) {
        pdebug(DEBUG_WARN, "Unable to allocate internal tag data buffer!");
        rc_dec(tag);
        return NULL;
    }

    tag->plc = pccc_eip_plc_get(attribs);
    if(!tag->plc) {
        pdebug(DEBUG_WARN, "Unable to get PLC!");
        rc_dec(tag);
        return NULL;
    }

    /* set the vtable for base functions. */
    switch(plc_type) {
        case AB2_PLC_PLC5:
            tag->base_tag.vtable = &plc5_tag_vtable;
            tag->base_tag.byte_order = &plc5_tag_byte_order;
            break;

        case AB2_PLC_SLC:
        case AB2_PLC_MLGX:
            tag->base_tag.vtable = &slc_tag_vtable;
            tag->base_tag.byte_order = &slc_tag_byte_order;
            break;

        default:
            pdebug(DEBUG_WARN, "Unsupported PLC type %d!", plc_type);
            rc_dec(tag);
            return NULL;
            break;
    }

    pdebug(DEBUG_INFO, "Done.");

    return (plc_tag_p)tag;
}


/* helper functions. */
void pccc_tag_destroy(void *tag_arg)
{
    pccc_tag_p tag = (pccc_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer passed to destructor!");
        return;
    }

    /* get rid of any outstanding timers and events. */

    /* unlink the protocol layers. */
    tag->plc = rc_dec(tag->plc);

    /* delete the base tag parts. */
    base_tag_destroy((plc_tag_p)tag);

    pdebug(DEBUG_INFO, "Done.");
}



int pccc_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value)
{
    int res = default_value;
    pccc_tag_p tag = (pccc_tag_p)raw_tag;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* assume we have a match. */
    tag->base_tag.status = PLCTAG_STATUS_OK;

    /* match the attribute. */
    if(str_cmp_i(attrib_name, "elem_size") == 0) {
        res = tag->elem_size;
    } else if(str_cmp_i(attrib_name, "elem_count") == 0) {
        res = tag->elem_count;
    } else if(str_cmp_i(attrib_name, "idle_timeout_ms") == 0) {
        res = plc_get_idle_timeout(tag->plc);
    } else {
        pdebug(DEBUG_WARN, "Unsupported attribute name \"%s\"!", attrib_name);
        tag->base_tag.status = PLCTAG_ERR_UNSUPPORTED;
        return default_value;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return res;
}


int pccc_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value)
{
    int rc = PLCTAG_STATUS_OK;
    pccc_tag_p tag = (pccc_tag_p)raw_tag;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(str_cmp_i(attrib_name, "idle_timeout_ms") == 0) {
        rc = plc_set_idle_timeout(tag->plc, new_value);
    } else {
        pdebug(DEBUG_WARN, "Unsupported attribute name \"%s\"!", attrib_name);
        rc = PLCTAG_ERR_UNSUPPORTED;
        tag->base_tag.status = (int8_t)rc;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


