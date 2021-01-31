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

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <lib/tag.h>
#include <ab2/ab.h>
#include <ab2/df1.h>
#include <ab2/cip_plc.h>
#include <ab2/cip_tag.h>
#include <ab2/pccc_eip_plc.h>
#include <ab2/plc5_tag.h>
#include <ab2/slc_tag.h>
#include <util/attr.h>
#include <util/bool.h>
#include <util/debug.h>
#include <util/mem.h>
#include <util/string.h>


#define CIP_MAX_ENCODED_NAME_SIZE (256)

static plc_tag_p pccc_tag_create(ab2_plc_type_t plc_type, attr attribs);
static void pccc_tag_destroy(void *tag_arg);

static plc_tag_p cip_tag_create(ab2_plc_type_t plc_type, attr attribs);
static void cip_tag_destroy(void *tag_arg);

static int cip_encode_tag_name(const char *name, uint8_t **encoded_tag_name, int *encoded_name_length, bool *is_bit, uint8_t *bit_num);
static int skip_whitespace(const char *name, int *name_index);
static int parse_bit_segment(const char *name, int *name_index, bool *is_bit, uint8_t *bit_num);
static int parse_symbolic_segment(const char *name, uint8_t *buf, int *encoded_index, int *name_index);
static int parse_numeric_segment(const char *name, uint8_t *buf, int *encoded_index, int *name_index);




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

        case AB2_PLC_LGX:
            /* fall through */
        case AB2_PLC_MLGX800:
            /* fall through */
        case AB2_PLC_OMRON_NJNX:
            result = cip_tag_create(plc_type, attribs);
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
    SET_STATUS(tag->base_tag.status, PLCTAG_STATUS_OK);

    /* match the attribute. */
    if(str_cmp_i(attrib_name, "elem_size") == 0) {
        res = tag->elem_size;
    } else if(str_cmp_i(attrib_name, "elem_count") == 0) {
        res = tag->elem_count;
    } else if(str_cmp_i(attrib_name, "idle_timeout_ms") == 0) {
        res = plc_get_idle_timeout(tag->plc);
    } else {
        pdebug(DEBUG_WARN, "Unsupported attribute name \"%s\"!", attrib_name);
        SET_STATUS(tag->base_tag.status, PLCTAG_ERR_UNSUPPORTED);
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
        SET_STATUS(tag->base_tag.status, (int8_t)rc);
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}




plc_tag_p cip_tag_create(ab2_plc_type_t plc_type, attr attribs)
{
    int rc = PLCTAG_STATUS_OK;
    cip_tag_p tag = NULL;
    const char *tag_name = NULL;
    int tmp = 0;
    bool is_bit = FALSE;
    uint8_t bit_num = 0;

    pdebug(DEBUG_INFO, "Starting.");

    tag = (cip_tag_p)base_tag_create(sizeof(*tag), (void (*)(void*))cip_tag_destroy);
    if(!tag) {
        pdebug(DEBUG_WARN, "Unable to allocate new CIP tag!");
        return NULL;
    }

    /* parse the CIP tag name */
    tag_name = attr_get_str(attribs, "name", NULL);
    if(!tag_name) {
        pdebug(DEBUG_WARN, "Tag name missing!");
        rc_dec(tag);
        return NULL;
    }

    /* check for a raw tag. */
    if(str_cmp_i(tag_name, "@raw") == 0) {
        pdebug(DEBUG_INFO, "Raw CIP tag found.");
        tag->is_raw_tag = TRUE;
        tag->elem_size = 1;

        tag->encoded_name = NULL;
        tag->encoded_name_length = 0;

        tag->base_tag.is_bit = 0;
        tag->base_tag.bit = 0;
    } else {
        tag->is_raw_tag = FALSE;

        rc = cip_encode_tag_name(tag_name, &(tag->encoded_name), &(tag->encoded_name_length), &is_bit, &bit_num);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Malformed tag name!");
            rc_dec(tag);
            return NULL;
        }
    }

    /* if the tag is a bit tag, then fill in the parent tag structure. */
    if(is_bit == TRUE) {
        tag->base_tag.is_bit = 1;
        tag->base_tag.bit = bit_num;
    }

    /* get the element count */
    tmp = attr_get_int(attribs, "elem_count", 1);
    if(tmp < 1) {
        pdebug(DEBUG_WARN, "Element count must be greater than zero or missing and will default to one!");
        rc_dec(tag);
        return NULL;
    }

    tag->elem_count = tmp;

    /* see if the user overrode the element size */
    tmp = tag->elem_size;
    tag->elem_size = (uint16_t)attr_get_int(attribs, "elem_size", tmp);

    if(tag->elem_size == 0) {
        pdebug(DEBUG_INFO, "Tag element size not explicitly given, will attempt to infer it from a read.");

        tag->base_tag.data = NULL;
        tag->base_tag.size = 0;
        tag->elem_size = 0;
    } else {
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
    }

    /* get the PLC */

    /* TODO - add any PLC-specific attributes here. */
    switch(plc_type) {
        case AB2_PLC_LGX:
            tag->base_tag.vtable = &cip_tag_vtable;
            tag->base_tag.byte_order = &cip_tag_byte_order;
            tag->plc = cip_plc_get(attribs);
            break;

        case AB2_PLC_MLGX800:
            tag->base_tag.vtable = &cip_tag_vtable;
            tag->base_tag.byte_order = &cip_tag_byte_order;
            tag->plc = cip_plc_get(attribs);
            break;

        case AB2_PLC_OMRON_NJNX:
            tag->base_tag.vtable = &cip_tag_vtable;
            tag->base_tag.byte_order = &cip_tag_byte_order;
            tag->plc = cip_plc_get(attribs);
            break;

        default:
            pdebug(DEBUG_WARN, "Unsupported PLC type %d!");
            tag->plc = NULL;
            break;
    }

    if(!tag->plc) {
        pdebug(DEBUG_WARN, "Unable to get PLC!");
        rc_dec(tag);
        return NULL;
    }

    pdebug(DEBUG_INFO, "Done.");

    return (plc_tag_p)tag;
}


/* helper functions. */
void cip_tag_destroy(void *tag_arg)
{
    cip_tag_p tag = (cip_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer passed to destructor!");
        return;
    }

    /* get rid of any outstanding requests, timers and events. */
    if(tag->plc) {
        plc_stop_request(tag->plc, &(tag->request));

        /* unlink the protocol layers. */
        tag->plc = rc_dec(tag->plc);
    }

    /* remove internal data. */
    if(tag->encoded_name) {
        mem_free(tag->encoded_name);
        tag->encoded_name = NULL;
        tag->encoded_name_length = 0;
    }

    /* delete the base tag parts. */
    base_tag_destroy((plc_tag_p)tag);

    pdebug(DEBUG_INFO, "Done.");
}





int cip_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value)
{
    int res = default_value;
    cip_tag_p tag = (cip_tag_p)raw_tag;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* assume we have a match. */
    SET_STATUS(tag->base_tag.status, PLCTAG_STATUS_OK);

    /* match the attribute. Raw tags only have size.*/
    if((tag->is_raw_tag == FALSE) && (str_cmp_i(attrib_name, "elem_size") == 0)) {
        res = tag->elem_size;
    } else if((tag->is_raw_tag == FALSE) && (str_cmp_i(attrib_name, "elem_count") == 0)) {
        res = tag->elem_count;
    } else if(str_cmp_i(attrib_name, "idle_timeout_ms") == 0) {
        res = plc_get_idle_timeout(tag->plc);
    } else if((tag->is_raw_tag == TRUE) && (str_cmp_i(attrib_name, "payload_size") == 0)) {
        res = (int)(int32_t)tag->trans_offset;
    } else {
        pdebug(DEBUG_WARN, "Unsupported attribute name \"%s\"!", attrib_name);
        SET_STATUS(tag->base_tag.status, PLCTAG_ERR_UNSUPPORTED);
        return default_value;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return res;
}


int cip_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value)
{
    int rc = PLCTAG_STATUS_OK;
    cip_tag_p tag = (cip_tag_p)raw_tag;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(str_cmp_i(attrib_name, "idle_timeout_ms") == 0) {
        rc = plc_set_idle_timeout(tag->plc, new_value);
    } else if((tag->is_raw_tag == TRUE) && (str_cmp_i(attrib_name, "payload_size") == 0)) {
        if(new_value > 0) {
            rc = base_tag_resize_data((plc_tag_p)tag, new_value);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unable to resize tag data buffer, error %s!", plc_tag_decode_error(rc));
                return rc;
            }

            // REMOVE
            // /* do we need to allocate a bigger buffer? */
            // if(new_value > tag->base_tag.size) {
            //     uint8_t *new_data = NULL;

            //     tag->elem_count = new_value;
            //     new_data = mem_realloc(tag->base_tag.data, new_value);

            //     if(new_data) {
            //         pdebug(DEBUG_DETAIL, "Updating tag buffer size to %d from %" PRId32 ".", new_value, tag->base_tag.size);
            //         tag->base_tag.size = tag->elem_count;
            //         tag->base_tag.data = new_data;
            //     } else {
            //         pdebug(DEBUG_WARN, "Unable to reallocate tag data buffer to new size!");
            //         rc = PLCTAG_ERR_NO_MEM;
            //         return rc;
            //     }
            // }

            /* store the payload size in the trans_offset field */
            tag->trans_offset = (uint32_t)(int32_t)new_value;
        } else {
            pdebug(DEBUG_WARN, "The payload size must be greater than zero!");
            rc = PLCTAG_ERR_TOO_SMALL;
            return rc;
        }
    } else {
        pdebug(DEBUG_WARN, "Unsupported attribute name \"%s\"!", attrib_name);
        rc = PLCTAG_ERR_UNSUPPORTED;
        SET_STATUS(tag->base_tag.status, rc);
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}



/*
 * The EBNF is:
 *
 * tag ::= SYMBOLIC_SEG ( tag_seg )* ( bit_seg )?
 *
 * tag_seg ::= '.' SYMBOLIC_SEG
 *             '[' array_seg ']'
 *
 * bit_seg ::= '.' [0-9]+
 *
 * array_seg ::= NUMERIC_SEG ( ',' NUMERIC_SEG )*
 *
 * SYMBOLIC_SEG ::= [a-zA-Z]([a-zA-Z0-9_]*)
 *
 * NUMERIC_SEG ::= [0-9]+
 *
 */



int cip_encode_tag_name(const char *name, uint8_t **encoded_tag_name, int *encoded_name_length, bool *is_bit, uint8_t *bit_num)
{
    int rc = PLCTAG_STATUS_OK;
    int name_index = 0;
    int name_len = str_length(name);
    uint8_t buf[CIP_MAX_ENCODED_NAME_SIZE] = {0};

    pdebug(DEBUG_INFO, "Starting for name \"%s\".", name);

    /* zero out the CIP encoded name size. Byte zero in the encoded name. */
    *encoded_name_length = 0;
    buf[0] = 0;
    (*encoded_name_length)++;

    /* names must start with a symbolic segment. */
    if(parse_symbolic_segment(name, &buf[0], encoded_name_length, &name_index) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"Unable to parse initial symbolic segment in tag name %s!", name);
        return PLCTAG_ERR_BAD_PARAM;
    }

    while(name_index < name_len && (*encoded_name_length) < CIP_MAX_ENCODED_NAME_SIZE) {
        /* try to parse the different parts of the name. */
        if(name[name_index] == '.') {
            name_index++;
            /* could be a name segment or could be a bit identifier. */
            if(parse_symbolic_segment(name, &buf[0], encoded_name_length, &name_index) != PLCTAG_STATUS_OK) {
                /* try a bit identifier. */
                if(parse_bit_segment(name, &name_index, is_bit, bit_num) == PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_DETAIL, "Found bit identifier %" PRIu8 ".", *bit_num);
                    break;
                } else {
                    pdebug(DEBUG_WARN, "Expected a symbolic segment or a bit identifier at position %d in tag name %s", name_index, name);
                    return PLCTAG_ERR_BAD_PARAM;
                }
            } else {
                pdebug(DEBUG_DETAIL, "Found symbolic segment ending at %d", name_index);
            }
        } else if (name[name_index] == '[') {
            int num_dimensions = 0;
            /* must be an array so look for comma separated numeric segments. */
            do {
                name_index++;
                num_dimensions++;

                skip_whitespace(name, &name_index);
                rc = parse_numeric_segment(name, &buf[0], encoded_name_length, &name_index);
                skip_whitespace(name, &name_index);
            } while(rc == PLCTAG_STATUS_OK && name[name_index] == ',' && num_dimensions < 3);

            /* must terminate with a closing ']' */
            if(name[name_index] != ']') {
                pdebug(DEBUG_WARN, "Bad tag name format, expected closing array bracket at %d in tag name %s!", name_index, name);
                return PLCTAG_ERR_BAD_PARAM;
            }

            /* step past the closing bracket. */
            name_index++;
        } else {
            pdebug(DEBUG_WARN,"Unexpected character at position %d in name string %s!", name_index, name);
            break;
        }
    }

    if(name_index != name_len) {
        pdebug(DEBUG_WARN, "Bad tag name format.  Tag must end with a bit identifier if one is present.");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* set the word count. */
    buf[0] = (uint8_t)(((*encoded_name_length) -1)/2);

    /* try to allocate memory */
    *encoded_tag_name = mem_alloc(*encoded_name_length);
    if(! (*encoded_tag_name)) {
        pdebug(DEBUG_WARN, "Unable to allocate memory for encoded tag name!");
        return PLCTAG_ERR_NO_MEM;
    }

    /* copy the data. */
    mem_copy(*encoded_tag_name, &buf[0], *encoded_name_length);

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}

int skip_whitespace(const char *name, int *name_index)
{
    while(name[*name_index] == ' ') {
        (*name_index)++;
    }

    return PLCTAG_STATUS_OK;
}


/*
 * A bit segment is simply an integer from 0 to 63 (inclusive).
 */
int parse_bit_segment(const char *name, int *name_index, bool *is_bit, uint8_t *bit_num)
{
    const char *p, *q;
    long val;

    pdebug(DEBUG_DETAIL, "Starting with name index=%d.", *name_index);

    p = &name[*name_index];
    q = p;

    val = strtol((char *)p, (char **)&q, 10);

    /* sanity checks. */
    if(p == q) {
        /* no number. */
        pdebug(DEBUG_WARN,"Expected bit identifier or symbolic segment at position %d in tag name %s!", *name_index, name);
        return PLCTAG_ERR_BAD_PARAM;
    }

    if((val < 0) || (val >= 256)) {
        pdebug(DEBUG_WARN,"Bit identifier must be between 0 and 255, inclusive, was %d!", (int)val);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* TODO - move this check into the tag creation function. */
    // if(tag->elem_count != 1) {
    //     pdebug(DEBUG_WARN, "Bit tags must have only one element!");
    //     return PLCTAG_ERR_BAD_PARAM;
    // }

    /* bump name_index. */
    *name_index += (int)(q-p);
    *is_bit = TRUE;
    *bit_num = (uint8_t)val;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


int parse_symbolic_segment(const char *name, uint8_t *buf, int *encoded_index, int *name_index)
{
    int encoded_i = *encoded_index;
    int name_i = *name_index;
    int name_start = name_i;
    int seg_len_index = 0;
    int seg_len = 0;

    pdebug(DEBUG_DETAIL, "Starting with name index=%d and encoded name index=%d.", name_i, encoded_i);

    /* a symbolic segment must start with an alphabetic character or @, then can have digits or underscores. */
    if(!isalpha(name[name_i]) && name[name_i] != ':' && name[name_i] != '_' && name[name_i] != '@') {
        pdebug(DEBUG_DETAIL, "tag name at position %d is not the start of a symbolic segment.", name_i);
        return PLCTAG_ERR_NO_MATCH;
    }

    if(encoded_i >= (CIP_MAX_ENCODED_NAME_SIZE - 3)) {
        pdebug(DEBUG_WARN, "Encoded name too long!");
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* start building the encoded symbolic segment. */
    buf[encoded_i] = 0x91; /* start of symbolic segment. */
    encoded_i++;
    seg_len_index = encoded_i;
    buf[seg_len_index]++;
    encoded_i++;

    /* store the first character of the name. */
    buf[encoded_i] = (uint8_t)name[name_i];
    encoded_i++;
    name_i++;

    /* get the rest of the name. */
    while((encoded_i < CIP_MAX_ENCODED_NAME_SIZE) && (isalnum(name[name_i]) || name[name_i] == ':' || name[name_i] == '_')) {
        buf[encoded_i] = (uint8_t)name[name_i];
        encoded_i++;
        buf[seg_len_index]++;
        name_i++;
    }

    seg_len = buf[seg_len_index];

    /* finish up the encoded name.   Space for the name must be a multiple of two bytes long. */
    if(buf[seg_len_index] & 0x01) {
        buf[encoded_i] = 0;
        encoded_i++;
    }

    *encoded_index = encoded_i;
    *name_index = name_i;

    pdebug(DEBUG_DETAIL, "Parsed symbolic segment \"%.*s\" in tag name.", seg_len, &name[name_start]);

    return PLCTAG_STATUS_OK;
}


int parse_numeric_segment(const char *name, uint8_t *buf, int *encoded_index, int *name_index)
{
    const char *p, *q;
    long val;

    pdebug(DEBUG_DETAIL, "Starting with name index=%d and encoded name index=%d.", *name_index, *encoded_index);

    p = &name[*name_index];
    q = p;

    val = strtol((char *)p, (char **)&q, 10);

    /* sanity checks. */
    if(p == q) {
        /* no number. */
        pdebug(DEBUG_WARN,"Expected numeric segment at position %d in tag name %s!", *name_index, name);
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(val < 0) {
        pdebug(DEBUG_WARN,"Numeric segment must be greater than or equal to zero, was %d!", (int)val);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* bump name_index. */
    *name_index += (int)(q-p);

    /* encode the segment. */
    if(val > 0xFFFF) {
        buf[*encoded_index] = (uint8_t)0x2A; /* 4-byte segment value. */
        (*encoded_index)++;

        buf[*encoded_index] = (uint8_t)0; /* padding. */
        (*encoded_index)++;

        buf[*encoded_index] = (uint8_t)val & 0xFF;
        (*encoded_index)++;
        buf[*encoded_index] = (uint8_t)((val >> 8) & 0xFF);
        (*encoded_index)++;
        buf[*encoded_index] = (uint8_t)((val >> 16) & 0xFF);
        (*encoded_index)++;
        buf[*encoded_index] = (uint8_t)((val >> 24) & 0xFF);
        (*encoded_index)++;

        pdebug(DEBUG_DETAIL, "Parsed 4-byte numeric segment of value %u.", (uint32_t)val);
    } else if(val > 0xFF) {
        buf[*encoded_index] = (uint8_t)0x29; /* 2-byte segment value. */
        (*encoded_index)++;

        buf[*encoded_index] = (uint8_t)0; /* padding. */
        (*encoded_index)++;

        buf[*encoded_index] = (uint8_t)val & 0xFF;
        (*encoded_index)++;
        buf[*encoded_index] = (uint8_t)((val >> 8) & 0xFF);
        (*encoded_index)++;

        pdebug(DEBUG_DETAIL, "Parsed 2-byte numeric segment of value %u.", (uint32_t)val);
    } else {
        buf[*encoded_index] = (uint8_t)0x28; /* 1-byte segment value. */
        (*encoded_index)++;

        buf[*encoded_index] = (uint8_t)val & 0xFF;
        (*encoded_index)++;

        pdebug(DEBUG_DETAIL, "Parsed 1-byte numeric segment of value %u.", (uint32_t)val);
    }

    pdebug(DEBUG_DETAIL, "Done with name index=%d and encoded name index=%d.", *name_index, *encoded_index);

    return PLCTAG_STATUS_OK;
}








