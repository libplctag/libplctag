/***************************************************************************
 *   Copyright (C) 2021 by Kyle Hayes                                      *
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
#include <lib/libplctag.h>
#include <lib/tag.h>
#include <ab2/ab.h>
#include <ab2/cip.h>
#include <ab2/cip_tag.h>
#include <ab2/cip_plc.h>
#include <ab2/magic_detect_change_tag.h>
#include <ab2/magic_list_tags_tag.h>
#include <ab2/magic_udt_tag.h>
#include <ab2/raw_cip_tag.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/mem.h>
#include <util/plc.h>
#include <util/string.h>

#define CIP_MAX_ENCODED_NAME_SIZE (256)



static void cip_tag_destroy(void *tag_arg);

// static int ab2_cip_encode_tag_name(const char *name, uint8_t **encoded_tag_name, int *encoded_name_length, bool *is_bit, uint8_t *bit_num);
static int skip_whitespace(const char *name, int *name_index);
static int parse_bit_segment(const char *name, int *name_index, bool *is_bit, uint8_t *bit_num);
static int parse_symbolic_segment(const char *name, uint8_t *buf, int *encoded_index, int *name_index);
static int parse_numeric_segment(const char *name, uint8_t *buf, int *encoded_index, int *name_index);

static int encode_bridge_segment(uint8_t *data, int data_capacity, int *data_offset, char **path_segments, int *segment_index);
static int encode_dhp_addr_segment(uint8_t *data, int data_capacity, int *data_offset, char **path_segments, int *segment_index, bool *is_dhp, uint8_t *target_dhp_port, uint8_t *target_dhp_id);
static int encode_numeric_segment(uint8_t *data, int data_capacity, int *data_offset, char **path_segments, int *segment_index);




plc_tag_p cip_tag_create(ab2_plc_type_t plc_type, attr attribs)
{
    int rc = PLCTAG_STATUS_OK;
    cip_tag_p tag = NULL;
    const char *tag_name = NULL;
    int tmp = 0;
    bool is_bit = false;
    uint8_t bit_num = 0;

    pdebug(DEBUG_INFO, "Starting.");

    tag_name = attr_get_str(attribs, "name", NULL);
    if(tag_name == NULL || str_length(tag_name) == 0) {
        pdebug(DEBUG_WARN, "Tag name is missing or empty, you must have a tag name!");
        return NULL;
    }

    /* handle magic tags. */
    if(str_cmp_i("@raw", tag_name) == 0) {
        return raw_cip_tag_create(plc_type, attribs);
    }

    /* bare tag listing tag. */
    if(str_cmp_i("@tags", tag_name) == 0) {
        pdebug(DEBUG_DETAIL, "Creating controller level tag listing tag.");
        return magic_list_tags_tag_create(plc_type, attribs);
    }

    /* program tag listing tag. */
    if(str_cmp_i_n(tag_name, "Program:", str_length("Program:")) == 0) {
        /* the tag name has a prefix of "Program:" */
        if(str_cmp_i(tag_name + str_length(tag_name) - str_length("@tags"), "@tags") == 0) {
            /* and it ends with "@tags" */
            pdebug(DEBUG_DETAIL, "Creating program level tag listing tag.");
            return magic_list_tags_tag_create(plc_type, attribs);
        }
    }

    if(str_cmp_i_n(tag_name, "@udt/", str_length("@udt/")) == 0) {
        pdebug(DEBUG_DETAIL, "Creating UDT information tag.");
        return magic_udt_tag_create(plc_type, attribs);
    }

    if(str_cmp_i(tag_name, "@change") == 0) {
        pdebug(DEBUG_DETAIL, "Creating tag mapping change detection tag.");
        return magic_detect_change_tag_create(plc_type, attribs);
    }

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
        tag->elem_size = 1;

        tag->encoded_name = NULL;
        tag->encoded_name_length = 0;

        tag->base_tag.is_bit = 0;
        tag->base_tag.bit = 0;
    } else {
        rc = ab2_cip_encode_tag_name(tag_name, &(tag->encoded_name), &(tag->encoded_name_length), &is_bit, &bit_num);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Malformed tag name!");
            rc_dec(tag);
            return NULL;
        }
    }

    /* if the tag is a bit tag, then fill in the parent tag structure. */
    if(is_bit == true) {
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
            /* Set the max payload size up high to enable Forward Open Extended use and negotiation. */
            attr_set_int(attribs, "cip_payload", attr_get_int(attribs, "cip_payload", CIP_MAX_PAYLOAD));

            tag->base_tag.vtable = &cip_tag_vtable;
            tag->base_tag.byte_order = &cip_tag_byte_order;
            tag->plc = cip_plc_get(attribs);
            break;

        case AB2_PLC_MLGX800:
            attr_set_int(attribs, "cip_payload", attr_get_int(attribs, "cip_payload", CIP_STD_PAYLOAD));
            tag->base_tag.vtable = &cip_tag_vtable;
            tag->base_tag.byte_order = &cip_tag_byte_order;
            tag->plc = cip_plc_get(attribs);
            break;

        case AB2_PLC_OMRON_NJNX:
            attr_set_int(attribs, "cip_payload", attr_get_int(attribs, "cip_payload", CIP_STD_PAYLOAD));
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

    /* if this is a normal tag, trigger a read. */
    if(tag->is_raw_tag != true) {
        pdebug(DEBUG_DETAIL, "Trigger initial read.");
        tag->base_tag.read_in_flight = 1;
        rc = tag->base_tag.vtable->read((plc_tag_p)tag);
        if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
            pdebug(DEBUG_WARN, "Error %s while trying to trigger initial read!", plc_tag_decode_error(rc));
            rc_dec(tag);
            return NULL;
        }
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

    pdebug(DEBUG_DETAIL, "Starting for attribute \"%s\".", attrib_name);

    /* assume we have a match. */
    SET_STATUS(tag->base_tag.status, PLCTAG_STATUS_OK);

    /* match the attribute. Raw tags only have size.*/
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

    pdebug(DEBUG_DETAIL, "Done for attribute \"%s\".", attrib_name);

    return res;
}


int cip_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value)
{
    int rc = PLCTAG_STATUS_OK;
    cip_tag_p tag = (cip_tag_p)raw_tag;

    pdebug(DEBUG_DETAIL, "Starting for attribute \"%s\".", attrib_name);

    if(str_cmp_i(attrib_name, "idle_timeout_ms") == 0) {
        rc = plc_set_idle_timeout(tag->plc, new_value);
    } else {
        pdebug(DEBUG_WARN, "Unsupported attribute name \"%s\"!", attrib_name);
        rc = PLCTAG_ERR_UNSUPPORTED;
        SET_STATUS(tag->base_tag.status, rc);
    }

    pdebug(DEBUG_DETAIL, "Done for attribute \"%s\".", attrib_name);

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



int ab2_cip_encode_tag_name(const char *name, uint8_t **encoded_tag_name, int *encoded_name_length, bool *is_bit, uint8_t *bit_num)
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

    /* bump name_index. */
    *name_index += (int)(q-p);
    *is_bit = true;
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





int ab2_cip_encode_path(uint8_t *data, int data_capacity, int *data_offset, const char *path, bool *is_dhp, uint8_t *target_dhp_port, uint8_t *target_dhp_id)
{
    int rc = PLCTAG_STATUS_OK;
    int segment_index = 0;
    char **path_segments = NULL;
    int path_len = 0;
    int path_len_offset = 0;

    pdebug(DEBUG_INFO, "Starting with path \"%s\".", path);

    do {
        if(!path) {
            pdebug(DEBUG_WARN, "Path string pointer is null!");
            rc = PLCTAG_ERR_NULL_PTR;
            break;
        }

        /* split the path and then encode the parts. */
        path_segments = str_split(path, ",");
        if(!path_segments) {
            pdebug(DEBUG_WARN, "Unable to split path string!");
            rc = PLCTAG_ERR_NO_MEM;
            break;
        }

        path_len_offset = *data_offset;
        TRY_SET_BYTE(data, data_capacity, *data_offset, 0); /* filler */

        while(path_segments[segment_index] != NULL) {
            if(str_length(path_segments[segment_index]) == 0) {
                pdebug(DEBUG_WARN, "Path segment %d is zero length!", segment_index+1);
                rc = PLCTAG_ERR_BAD_PARAM;
                break;
            }

            /* try the different types. */
            do {
                rc = encode_bridge_segment(data, data_capacity, data_offset, path_segments, &segment_index);
                if(rc != PLCTAG_ERR_NO_MATCH) break;

                rc = encode_dhp_addr_segment(data, data_capacity, data_offset, path_segments, &segment_index, is_dhp, target_dhp_port, target_dhp_id);
                if(rc != PLCTAG_ERR_NO_MATCH) break;
                if(rc == PLCTAG_STATUS_OK) { *is_dhp = true; break; } /* DH+ must be last */

                rc = encode_numeric_segment(data, data_capacity, data_offset, path_segments, &segment_index);
                if(rc != PLCTAG_ERR_NO_MATCH) break;
            } while(0);

            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unable to match segment %d \"%s\", error %s.", segment_index, path_segments[segment_index], plc_tag_decode_error(rc));
                break;
            }

            if(rc == PLCTAG_STATUS_OK && path_segments[segment_index] != NULL && (*is_dhp) == true) {
                pdebug(DEBUG_WARN, "DH+ path segment must be the last one in the path!");
                rc = PLCTAG_ERR_BAD_PARAM;
                break;
            }
        }

        if(rc != PLCTAG_STATUS_OK) {
            break;
        }

        /* TODO
         *
         * Move all this to the PLC-specific part and out of this layer completely.
         * The PLC definition should set up either additional path elements ("path_suffix") or
         * modify the path for the specific type of PLC and connection.
         */

        /* build the routing part. */
        if(is_dhp && (*is_dhp == true)) {
            /* build the DH+ encoding. */
            uint8_t dhp_port = (target_dhp_port ? 0 : *target_dhp_port);

            TRY_SET_BYTE(data, data_capacity, *data_offset, 0x20); /* class, 8-bit id */
            TRY_SET_BYTE(data, data_capacity, *data_offset, 0xA6); /* DH+ Router class */
            TRY_SET_BYTE(data, data_capacity, *data_offset, 0x24); /* instance, 8-bit id */
            TRY_SET_BYTE(data, data_capacity, *data_offset, dhp_port); /* port as 8-bit value */
            TRY_SET_BYTE(data, data_capacity, *data_offset, 0x2C); /* ?? 8-bit id */
            TRY_SET_BYTE(data, data_capacity, *data_offset, 0x01); /* maybe an 8-bit instance id? */
        } else {
            TRY_SET_BYTE(data, data_capacity, *data_offset, 0x20); /* class, 8-bit id */
            TRY_SET_BYTE(data, data_capacity, *data_offset, 0x02); /* Message Router class */
            TRY_SET_BYTE(data, data_capacity, *data_offset, 0x24); /* instance, 8-bit id */
            TRY_SET_BYTE(data, data_capacity, *data_offset, 0x01); /* Message Router instance */
        }

        /*
         * zero pad the path to a multiple of 16-bit
         * words.
         */
        path_len = *data_offset - (path_len_offset + 1);
        pdebug(DEBUG_DETAIL,"Path length before %d", path_len);
        if(path_len & 0x01) {
            TRY_SET_BYTE(data, data_capacity, *data_offset, 0x00); /* pad with zero bytes */
            path_len++;
        }

        /* back fill the path length, in 16-bit words. */
        TRY_SET_BYTE(data, data_capacity, path_len_offset, path_len/2);
    } while(0);

    if(path_segments) {
        mem_free(path_segments);
    }

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO, "Done with path \"%s\".", path);
    } else {
        pdebug(DEBUG_WARN, "Unable to encode CIP path, error %s!", plc_tag_decode_error(rc));
    }

    return rc;
}




int encode_bridge_segment(uint8_t *data, int data_capacity, int *data_offset, char **path_segments, int *segment_index)
{
    int rc = PLCTAG_STATUS_OK;
    int local_seg_index = *segment_index;
    const char *port_seg = NULL;
    int port = 0;
    const char *addr = NULL;
    int addr_len = 0;
    int num_dots = 0;

    pdebug(DEBUG_DETAIL, "Starting with path segment \"%s\"", path_segments[*segment_index]);

    /* the pattern we want is a port specifier, 18/A, or 19/B, followed by a dotted quad IP address. */

    do {
        if(path_segments[*segment_index] == NULL) {
            pdebug(DEBUG_WARN, "Segment is NULL!");
            rc = PLCTAG_ERR_NULL_PTR;
            break;
        }

        /* is there a next element? */
        if(path_segments[local_seg_index + 1] == NULL) {
            pdebug(DEBUG_DETAIL, "Need two segments to match a bridge, but there is only one left.  Not a bridge segment.");
            rc = PLCTAG_ERR_NO_MATCH;
            break;
        }

        /* match the port. */
        port_seg = path_segments[local_seg_index];
        if(str_cmp_i("a",port_seg) == 0 || str_cmp("18", port_seg) == 0) {
            pdebug(DEBUG_DETAIL, "Matched first bridge segment with port A.");
            port = 18;
        } else if(str_cmp_i("b",port_seg) == 0 || str_cmp("19", port_seg) == 0) {
            pdebug(DEBUG_DETAIL, "Matched first bridge segment with port B.");
            port = 19;
        } else {
            pdebug(DEBUG_DETAIL, "Segment \"%s\" is not a matching port for a bridge segment.", port_seg);
            rc = PLCTAG_ERR_NO_MATCH;
            break;
        }

        /* match the IP address.
         *
         * We want a dotted quad.   Each octet must be 0-255.
         */

        addr = path_segments[local_seg_index + 1];

        /* do initial sanity checks */
        addr_len = str_length(addr);
        if(addr_len < 7) {
            pdebug(DEBUG_DETAIL, "Checked for an IP address, but found \"%s\".", addr);
            rc = PLCTAG_ERR_NO_MATCH;
            break;
        }

        if(addr_len > 15) {
            pdebug(DEBUG_DETAIL, "Possible address segment, \"%s\", is too long to be a valid IP address.", addr);
            rc = PLCTAG_ERR_NO_MATCH;
            break;
        }

        /* is the addr part only digits and dots? */
        for(int index = 0; index < addr_len; index++) {
            if(!isdigit(addr[index]) && addr[index] != '.') {
                pdebug(DEBUG_DETAIL, "The possible address string, \"%s\", contains characters other than digits and dots.  Not an IP address.", addr);
                rc = PLCTAG_ERR_NO_MATCH;
                break;
            }

            if(addr[index] == '.') {
                num_dots++;
            }
        }
        if(rc != PLCTAG_STATUS_OK) {
            break;
        }

        if(num_dots != 3) {
            pdebug(DEBUG_DETAIL, "The possible address string \"%s\" is not a valid dotted quad.", addr);
        }

        /* TODO add checks to make sure each octet is in 0-255 */

        /* build the encoded segment. */
        TRY_SET_BYTE(data, data_capacity, *data_offset, port);

        /* address length */
        TRY_SET_BYTE(data, data_capacity, *data_offset, addr_len);

        /* copy the address string data. */
        for(int index = 0; index < addr_len; index++) {
            TRY_SET_BYTE(data, data_capacity, *data_offset, addr[index]);
        }
    } while(0);

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Matched bridge segment.");
        *segment_index += 2;
    } else {
        pdebug(DEBUG_DETAIL, "Did not match bridge segment.");
    }

    return rc;
}


int encode_dhp_addr_segment(uint8_t *data, int data_capacity, int *data_offset, char **path_segments, int *segment_index, bool *is_dhp, uint8_t *target_dhp_port, uint8_t *target_dhp_id)
{
    int rc = PLCTAG_STATUS_OK;
    int port = 0;
    const char *addr = NULL;
    int addr_len = 0;
    int addr_index = 0;
    int val = 0;

    (void)data;
    (void)data_capacity;
    (void)data_offset;

    pdebug(DEBUG_DETAIL, "Starting with path segment \"%s\"", path_segments[*segment_index]);

    /* the pattern we want is port:src:dest where src can be ignored and dest is 0-255.
     * Port needs to be 2/A, or 3/B. */

    do {
        /* sanity checks. */
        addr = path_segments[*segment_index];

        if(addr == NULL) {
            pdebug(DEBUG_WARN, "Segment is NULL!");
            rc = PLCTAG_ERR_NULL_PTR;
            break;
        }

        addr_len = str_length(addr);

        if(addr_len < 5) {
            pdebug(DEBUG_DETAIL, "Possible DH+ address segment, \"%s\", is too short to be a DH+ element.", addr);
            rc = PLCTAG_ERR_NO_MATCH;
            break;
        }

        if(addr_len > 9) {
            pdebug(DEBUG_DETAIL, "Possible DH+ address segment, \"%s\", is too long to be a DH+ element.", addr);
            rc = PLCTAG_ERR_NO_MATCH;
            break;
        }

        /* scan the DH+ address */

        /* Get the port part. */
        addr_index = 0;
        switch(addr[addr_index]) {
            case 'A':
                /* fall through */
            case 'a':
                /* fall through */
            case '2':
                port = 1;
                break;

            case 'B':
                /* fall through */
            case 'b':
                /* fall through */
            case '3':
                port = 2;
                break;

            default:
                pdebug(DEBUG_DETAIL, "Possible DH+ address segment, \"%s\", does not have a valid port identifier.", addr);
                return PLCTAG_ERR_NO_MATCH;
                break;
        }

        addr_index++;

        /* is the next character a colon? */
        if(addr[addr_index] != ':') {
            pdebug(DEBUG_DETAIL, "Possible DH+ address segment, \"%s\", does not have a colon after the port.", addr);
            rc = PLCTAG_ERR_NO_MATCH;
            break;
        }

        addr_index++;

        /* get the source node */
        val = 0;
        while(isdigit(addr[addr_index])) {
            val = (val * 10) + (addr[addr_index] - '0');
            addr_index++;
        }

        /* is the source node a valid number? */
        if(val < 0 || val > 255) {
            pdebug(DEBUG_WARN, "Source node DH+ address part is out of bounds (0 <= %d < 256).", val);
            rc = PLCTAG_ERR_BAD_PARAM;
            break;
        }

        /* we ignore the source node. */

        addr_index++;

        /* is the next character a colon? */
        if(addr[addr_index] != ':') {
            pdebug(DEBUG_DETAIL, "Possible DH+ address segment, \"%s\", does not have a colon after the port.", addr);
            rc = PLCTAG_ERR_NO_MATCH;
            break;
        }

        addr_index++;

        /* get the destination node */
        val = 0;
        while(isdigit(addr[addr_index])) {
            val = (val * 10) + (addr[addr_index] - '0');
            addr_index++;
        }

        /* is the destination node a valid number? */
        if(val < 0 || val > 255) {
            pdebug(DEBUG_WARN, "Destination node DH+ address part is out of bounds (0 <= %d < 256).", val);
            rc = PLCTAG_ERR_BAD_PARAM;
            break;
        }

        /* store the destination node for later */

        /* we might be called before the state is allocated for size calculation. */
        if(is_dhp && target_dhp_port && target_dhp_id) {
            *is_dhp = true;
            *target_dhp_port = (uint8_t)(unsigned int)port;
            *target_dhp_id = (uint8_t)(unsigned int)val;
        }
    } while(0);

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Matched DH+ segment.");
        (*segment_index)++;
    } else {
        pdebug(DEBUG_DETAIL, "Did not match DH+ segment.");
    }

    return rc;
}



int encode_numeric_segment(uint8_t *data, int data_capacity, int *data_offset, char **path_segments, int *segment_index)
{
    int rc = PLCTAG_STATUS_OK;
    const char *segment = path_segments[*segment_index];
    int seg_len = 0;
    int val = 0;
    int seg_index = 0;

    pdebug(DEBUG_DETAIL, "Starting with segment \"%s\".", segment);

    do {
        if(segment == NULL) {
            pdebug(DEBUG_WARN, "Segment is NULL!");
            rc = PLCTAG_ERR_NULL_PTR;
            break;
        }

        seg_len = str_length(segment);

        if(seg_len > 3) {
            pdebug(DEBUG_DETAIL, "Possible numeric address segment, \"%s\", is too long to be valid.", segment);
            rc = PLCTAG_ERR_NO_MATCH;
            break;
        }

        seg_index = 0;
        while(isdigit(segment[seg_index])) {
            val = (val * 10) + (segment[seg_index] - '0');
            seg_index++;
        }

        if(!isdigit(segment[seg_index]) && segment[seg_index] != 0) {
            pdebug(DEBUG_DETAIL, "Possible numeric address segment, \"%s\", contains non-numeric characters.", segment);
            rc = PLCTAG_ERR_NO_MATCH;
            break;
        }

        if(val < 0 || val > 255) {
            pdebug(DEBUG_WARN, "Numeric segment must be between 0 and 255, inclusive!");
            rc = PLCTAG_ERR_BAD_PARAM;
            break;
        }

        TRY_SET_BYTE(data, data_capacity, *data_offset, val);
    } while(0);

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Matched numeric segment.");
        (*segment_index)++;
    } else {
        pdebug(DEBUG_DETAIL, "Did not match numeric segment.");
    }

    return rc;
}





struct error_code_entry {
    int primary_code;
    int secondary_code;
    int translated_code;
    const char *short_desc;
    const char *long_desc;
};


/*
 * This information was constructed after finding a few online resources.  Most of it comes from
 * publically published manuals for other products.  Sources include:
 *  - Allen-Bradley/Rockwell public documents
 *  - aboutplcs.com
 * 	- Kepware
 *  - and others I have long since lost track of.
 *
 * Most probably comes from Allen-Bradley/Rockwell and aboutplcs.com.
 *
 * The copyright on these entries that of their respective owners.  Used here under assumption of Fair Use.
 */


static struct error_code_entry error_code_table[] = {
    {0x01, 0x0100, PLCTAG_ERR_DUPLICATE, "Connection In Use/Duplicate Forward Open", "A connection is already established from the target device sending a Forward Open request or the target device has sent multiple forward open request. This could be caused by poor network traffic. Check the cabling, switches and connections."},
    {0x01, 0x0103, PLCTAG_ERR_UNSUPPORTED, "Transport Class/Trigger Combination not supported", "The Transport class and trigger combination is not supported. The Productivity Suite CPU only supports Class 1 and Class 3 transports and triggers: Change of State and Cyclic."},
    {0x01, 0x0106, PLCTAG_ERR_NOT_ALLOWED, "Owner Conflict", "An existing exclusive owner has already configured a connection to this Connection Point. Check to see if other Scanner devices are connected to this adapter or verify that Multicast is supported by adapter device if Multicast is selected for Forward Open. This could be caused by poor network traffic. Check the cabling, switches and connections."},
    {0x01, 0x0107, PLCTAG_ERR_NOT_FOUND, "Target Connection Not Found", "This occurs if a device sends a Forward Close on a connection and the device can't find this connection. This could occur if one of these devices has powered down or if the connection timed out on a bad connection. This could be caused by poor network traffic. Check the cabling, switches and connections."},
    {0x01, 0x0108, PLCTAG_ERR_BAD_PARAM, "Invalid Network Connection Parameter", "This error occurs when one of the parameters specified in the Forward Open message is not supported such as Connection Point, Connection type, Connection priority, redundant owner or exclusive owner. The Productivity Suite CPU does not return this error and will instead use errors 0x0120, 0x0121, 0x0122, 0x0123, 0x0124, 0x0125 or 0x0132 instead."},
    {0x01, 0x0109, PLCTAG_ERR_BAD_PARAM, "Invalid Connection Size", "This error occurs when the target device doesn't support the requested connection size. Check the documentation of the manufacturer's device to verify the correct Connection size required by the device. Note that most devices specify this value in terms of bytes. The Productivity Suite CPU does not return this error and will instead use errors 0x0126, 0x0127 and 0x0128."},
    {0x01, 0x0110, PLCTAG_ERR_NOT_FOUND, "Target for Connection Not Configured", "This error occurs when a message is received with a connection number that does not exist in the target device. This could occur if the target device has powered down or if the connection timed out. This could be caused by poor network traffic. Check the cabling, switches and connections."},
    {0x01, 0x0111, PLCTAG_ERR_UNSUPPORTED, "RPI Not Supported", "This error occurs if the Originator is specifying an RPI that is not supported. The Productivity Suite CPU will accept a minimum value of 10ms on a CIP Forward Open request. However, the CPU will produce at the specified rate up to the scan time of the installed project. The CPU cannot product any faster than the scan time of the running project."},
    {0x01, 0x0112, PLCTAG_ERR_BAD_PARAM, "RPI Value not acceptable", "This error can be returned if the Originator is specifying an RPI value that is not acceptable. There may be six additional values following the extended error code with the acceptable values. An array can be defined for this field in order to view the extended error code attributes. If the Target device supports extended status, the format of the values will be as shown below:\nUnsigned Integer 16, Value = 0x0112, Explanation: Extended Status code,\nUnsigned Integer 8, Value = variable, Explanation: Acceptable Originator to Target RPI type, values: 0 = The RPI specified in the forward open was acceptable (O -> T value is ignored), 1 = unspecified (use a different RPI), 2 = minimum acceptable RPI (too fast), 3 = maximum acceptable RPI (too slow), 4 = required RPI to corrected mismatch (data is already being consumed at a different RPI), 5 to 255 = reserved.\nUnsigned Integer 32, Value = variable, Explanation: Value of O -> T RPI that is within the acceptable range for the application.\nUnsigned Integer 32, Value = variable, Explanation: Value of T -> O RPI that is within the acceptable range for the application."},
    {0x01, 0x0113, PLCTAG_ERR_NO_RESOURCES, "Out of Connections", "The Productivity Suite EtherNet/IP Adapter connection limit of 4 when doing Class 3 connections has been reached. An existing connection must be dropped in order for a new one to be generated."},
    {0x01, 0x0114, PLCTAG_ERR_NOT_FOUND, "Vendor ID or Product Code Mismatch", "The compatibility bit was set in the Forward Open message but the Vendor ID or Product Code did not match."},
    {0x01, 0x0115, PLCTAG_ERR_NOT_FOUND, "Device Type Mismatch", "The compatibility bit was set in the Forward Open message but the Device Type did not match."},
    {0x01, 0x0116, PLCTAG_ERR_NO_MATCH, "Revision Mismatch", "The compatibility bit was set in the Forward Open message but the major and minor revision numbers were not a valid revision."},
    {0x01, 0x0117, PLCTAG_ERR_BAD_PARAM, "Invalid Produced or Consumed Application Path", "This error is returned from the Target device when the Connection Point parameters specified for the O -> T (Output) or T -> O (Input) connection is incorrect or not supported. The Productivity Suite CPU does not return this error and uses the following error codes instead: 0x012A, 0x012B or 0x012F."},
    {0x01, 0x0118, PLCTAG_ERR_BAD_PARAM, "Invalid or Inconsistent Configuration Application Path", "This error is returned from the Target device when the Connection Point parameter specified for the Configuration data is incorrect or not supported. The Productivity Suite CPU does not return this error and uses the following error codes instead: 0x0129 or 0x012F."},
    {0x01, 0x0119, PLCTAG_ERR_OPEN, "Non-listen Only Connection Not Opened", "This error code is returned when an Originator device attempts to establish a listen only connection and there is no non-listen only connection established. The Productivity Suite CPU does not support listen only connections as Scanner or Adapter."},
    {0x01, 0x011A, PLCTAG_ERR_NO_RESOURCES, "Target Object Out of Connections", "The maximum number of connections supported by this instance of the object has been exceeded."},
    {0x01, 0x011B, PLCTAG_ERR_TOO_SMALL, "RPI is smaller than the Production Inhibit Time", "The Target to Originator RPI is smaller than the Target to Originator Production Inhibit Time. Consult the manufacturer's documentation as to the minimum rate that data can be produced and adjust the RPI to greater than this value."},
    {0x01, 0x011C, PLCTAG_ERR_UNSUPPORTED, "Transport Class Not Supported", "The Transport Class requested in the Forward Open is not supported. Only Class 1 and Class 3 classes are supported in the Productivity Suite CPU."},
    {0x01, 0x011D, PLCTAG_ERR_UNSUPPORTED, "Production Trigger Not Supported", "The Production Trigger requested in the Forward Open is not supported. In Class 1, only Cyclic and Change of state are supported in the Productivity Suite CPU. In Class 3, Application object is supported."},
    {0x01, 0x011E, PLCTAG_ERR_UNSUPPORTED, "Direction Not Supported", "The Direction requested in the Forward Open is not supported."},
    {0x01, 0x011F, PLCTAG_ERR_BAD_PARAM, "Invalid Originator to Target Network Connection Fixed/Variable Flag", "The Originator to Target fixed/variable flag specified in the Forward Open is not supported . Only Fixed is supported in the Productivity Suite CPU."},
    {0x01, 0x0120, PLCTAG_ERR_BAD_PARAM, "Invalid Target to Originator Network Connection Fixed/Variable Flag", "The Target to Originator fixed/variable flag specified in the Forward Open is not supported. Only Fixed is supported in the Productivity Suite CPU."},
    {0x01, 0x0121, PLCTAG_ERR_BAD_PARAM, "Invalid Originator to Target Network Connection Priority", "The Originator to Target Network Connection Priority specified in the Forward Open is not supported. Low, High, Scheduled and Urgent are supported in the Productivity Suite CPU."},
    {0x01, 0x0122, PLCTAG_ERR_BAD_PARAM, "Invalid Target to Originator Network Connection Priority", "The Target to Originator Network Connection Priority specified in the Forward Open is not supported. Low, High, Scheduled and Urgent are supported in the Productivity Suite CPU."},
    {0x01, 0x0123, PLCTAG_ERR_BAD_PARAM, "Invalid Originator to Target Network Connection Type", "The Originator to Target Network Connection Type specified in the Forward Open is not supported. Only Unicast is supported for O -> T (Output) data in the Productivity Suite CPU."},
    {0x01, 0x0124, PLCTAG_ERR_BAD_PARAM, "Invalid Target to Originator Network Connection Type", "The Target to Originator Network Connection Type specified in the Forward Open is not supported. Multicast and Unicast is supported in the Productivity Suite CPU. Some devices may not support one or the other so if this error is encountered try the other method."},
    {0x01, 0x0125, PLCTAG_ERR_BAD_PARAM, "Invalid Originator to Target Network Connection Redundant_Owner", "The Originator to Target Network Connection Redundant_Owner flag specified in the Forward Open is not supported. Only Exclusive owner connections are supported in the Productivity Suite CPU."},
    {0x01, 0x0126, PLCTAG_ERR_BAD_PARAM, "Invalid Configuration Size", "This error is returned when the Configuration data sent in the Forward Open does not match the size specified or is not supported by the Adapter. The Target device may return an additional Unsigned Integer 16 value that specifies the maximum size allowed for this data. An array can be defined for this field in order to view the extended error code attributes."},
    {0x01, 0x0127, PLCTAG_ERR_BAD_PARAM, "Invalid Originator to Target Size", "This error is returned when the Originator to Target (Output data) size specified in the Forward Open does not match what is in the Target. Consult the documentation of the Adapter device to verify the required size. Note that if the Run/Idle header is requested, it will add 4 additional bytes and must be accounted for in the Forward Open calculation. The Productivity Suite CPU always requires the Run/Idle header so if the option doesn't exist in the Scanner device, you must add an additional 4 bytes to the O -> T (Output) setup. Some devices may publish the size that they are looking for as an additional attribute (Unsigned Integer 16 value) of the Extended Error Code. An array can be defined for this field in order to view the extended error code attributes.\nNote: This error may also be generated when a Connection Point value that is invalid for IO Messaging (but valid for other cases such as Explicit Messaging) is specified, such as 0. Please verify if the Connection Point value is valid for IO Messaging in the target device."},
    {0x01, 0x0128, PLCTAG_ERR_BAD_PARAM, "Invalid Target to Originator Size", "This error is returned when the Target to Originator (Input data) size specified in the Forward Open does not match what is in Target. Consult the documentation of the Adapter device to verify the required size. Note that if the Run/Idle header is requested, it will add 4 additional bytes and must be accounted for in the Forward Open calculation. The Productivity Suite CPU does not support a Run/Idle header for the T -> O (Input) data. Some devices may publish the size that they are looking for as an additional attribute (Unsigned Integer 16 value) of the Extended Error Code. An array can be defined for this field in order to view the extended error code attributes.\nNote: This error may also be generated when a Connection Point value that is invalid for IO Messaging (but valid for other cases such as Explicit Messaging) is specified, such as 0. Please verify if the Connection Point value is valid for IO Messaging in the target device."},
    {0x01, 0x0129, PLCTAG_ERR_BAD_PARAM, "Invalid Configuration Application Path", "This error will be returned by the Productivity Suite CPU if a Configuration Connection with a size other than 0 is sent to the CPU. The Configuration Connection size must always be zero if it this path is present in the Forward Open message coming from the Scanner device."},
    {0x01, 0x012A, PLCTAG_ERR_BAD_PARAM, "Invalid Consuming Application Path", "This error will be returned by the Productivity Suite CPU if the Consuming (O -> T) Application Path is not present in the Forward Open message coming from the Scanner device or if the specified Connection Point is incorrect."},
    {0x01, 0x012B, PLCTAG_ERR_BAD_PARAM, "Invalid Producing Application Path", "This error will be returned by the Productivity Suite CPU if the Producing (T -> O) Application Path is not present in the Forward Open message coming from the Scanner device or if the specified Connection Point is incorrect."},
    {0x01, 0x012C, PLCTAG_ERR_NOT_FOUND, "Configuration Symbol Does not Exist", "The Originator attempted to connect to a configuration tag name that is not supported in the Target."},
    {0x01, 0x012D, PLCTAG_ERR_NOT_FOUND, "Consuming Symbol Does not Exist", "The Originator attempted to connect to a consuming tag name that is not supported in the Target."},
    {0x01, 0x012E, PLCTAG_ERR_NOT_FOUND, "Producing Symbol Does not Exist", "The Originator attempted to connect to a producing tag name that is not supported in the Target."},
    {0x01, 0x012F, PLCTAG_ERR_BAD_DATA, "Inconsistent Application Path Combination", "The combination of Configuration, Consuming and Producing application paths specified are inconsistent."},
    {0x01, 0x0130, PLCTAG_ERR_BAD_DATA, "Inconsistent Consume data format", "Information in the data segment not consistent with the format of the data in the consumed data."},
    {0x01, 0x0131, PLCTAG_ERR_BAD_DATA, "Inconsistent Product data format", "Information in the data segment not consistent with the format of the data in the produced data."},
    {0x01, 0x0132, PLCTAG_ERR_UNSUPPORTED, "Null Forward Open function not supported", "The target device does not support the function requested in the NULL Forward Open request. The request could be such items as Ping device, Configure device application, etc."},
    {0x01, 0x0133, PLCTAG_ERR_BAD_PARAM, "Connection Timeout Multiplier not acceptable", "The Connection Multiplier specified in the Forward Open request not acceptable by the Target device (once multiplied in conjunction with the specified timeout value). Consult the manufacturer device's documentation on what the acceptable timeout and multiplier are for this device."},
    {0x01, 0x0203, PLCTAG_ERR_TIMEOUT, "Connection Timed Out", "This error will be returned by the Productivity Suite CPU if a message is sent to the CPU on a connection that has already timed out. Connections time out if no message is sent to the CPU in the time period specified by the RPI rate X Connection multiplier specified in the Forward Open message."},
    {0x01, 0x0204, PLCTAG_ERR_TIMEOUT, "Unconnected Request Timed Out", "This time out occurs when the device sends an Unconnected Request and no response is received within the specified time out period. In the Productivity Suite CPU, this value may be found in the hardware configuration under the Ethernet port settings for the P3-550 or P3-530."},
    {0x01, 0x0205, PLCTAG_ERR_BAD_PARAM, "Parameter Error in Unconnected Request Service", "This error occurs when Connection Tick Time/Connection time-out combination is specified in the Forward Open or Forward Close message is not supported by the device."},
    {0x01, 0x0206, PLCTAG_ERR_TOO_LARGE, "Message Too Large for Unconnected_Send Service", "Occurs when Unconnected_Send message is too large to be sent to the network."},
    {0x01, 0x0207, PLCTAG_ERR_BAD_REPLY, "Unconnected Acknowledge without Reply", "This error occurs if an Acknowledge was received but no data response occurred. Verify that the message that was sent is supported by the Target device using the device manufacturer's documentation."},
    {0x01, 0x0301, PLCTAG_ERR_NO_MEM, "No Buffer Memory Available", "This error occurs if the Connection memory buffer in the target device is full. Correct this by reducing the frequency of the messages being sent to the device and/or reducing the number of connections to the device. Consult the manufacturer's documentation for other means of correcting this."},
    {0x01, 0x0302, PLCTAG_ERR_NO_RESOURCES, "Network Bandwidth not Available for Data", "This error occurs if the Producer device cannot support the specified RPI rate when the connection has been configured with schedule priority. Reduce the RPI rate or consult the manufacturer's documentation for other means to correct this."},
    {0x01, 0x0303, PLCTAG_ERR_NO_RESOURCES, "No Consumed Connection ID Filter Available", "This error occurs if a Consumer device doesn't have an available consumed_connection_id filter."},
    {0x01, 0x0304, PLCTAG_ERR_BAD_CONFIG, "Not Configured to Send Scheduled Priority Data", "This error occurs if a device has been configured for a scheduled priority message and it cannot send the data at the scheduled time slot."},
    {0x01, 0x0305, PLCTAG_ERR_NO_MATCH, "Schedule Signature Mismatch", "This error occurs if the schedule priority information does not match between the Target and the Originator."},
    {0x01, 0x0306, PLCTAG_ERR_UNSUPPORTED, "Schedule Signature Validation not Possible", "This error occurs when the schedule priority information sent to the device is not validated."},
    {0x01, 0x0311, PLCTAG_ERR_BAD_DEVICE, "Port Not Available", "This error occurs when a port number specified in a port segment is not available. Consult the documentation of the device to verify the correct port number."},
    {0x01, 0x0312, PLCTAG_ERR_BAD_PARAM, "Link Address Not Valid", "The Link address specified in the port segment is not correct. Consult the documentation of the device to verify the correct port number."},
    {0x01, 0x0315, PLCTAG_ERR_BAD_PARAM, "Invalid Segment in Connection Path", "This error occurs when the target device cannot understand the segment type or segment value in the Connection Path. Consult the documentation of the device to verify the correct segment type and value. If a Connection Point greater than 255 is specified this error could occur."},
    {0x01, 0x0316, PLCTAG_ERR_NO_MATCH, "Forward Close Service Connection Path Mismatch", "This error occurs when the Connection path in the Forward Close message does not match the Connection Path configured in the connection. Contact Tech Support if this error persists."},
    {0x01, 0x0317, PLCTAG_ERR_BAD_PARAM, "Scheduling Not Specified", "This error can occur if the Schedule network segment or value is invalid."},
    {0x01, 0x0318, PLCTAG_ERR_BAD_PARAM, "Link Address to Self Invalid", "If the Link address points back to the originator device, this error will occur."},
    {0x01, 0x0319, PLCTAG_ERR_NO_RESOURCES, "Secondary Resource Unavailable", "This occurs in a redundant system when the secondary connection request is unable to duplicate the primary connection request."},
    {0x01, 0x031A, PLCTAG_ERR_DUPLICATE, "Rack Connection Already established", "The connection to a module is refused because part or all of the data requested is already part of an existing rack connection."},
    {0x01, 0x031B, PLCTAG_ERR_DUPLICATE, "Module Connection Already established", "The connection to a rack is refused because part or all of the data requested is already part of an existing module connection."},
    {0x01, 0x031C, PLCTAG_ERR_REMOTE_ERR, "Miscellaneous", "This error is returned when there is no other applicable code for the error condition. Consult the manufacturer's documentation or contact Tech support if this error persist."},
    {0x01, 0x031D, PLCTAG_ERR_NO_MATCH, "Redundant Connection Mismatch", "This error occurs when these parameters don't match when establishing a redundant owner connection: O -> T RPI, O -> T Connection Parameters, T -> O RPI, T -> O Connection Parameters and Transport Type and Trigger."},
    {0x01, 0x031E, PLCTAG_ERR_NO_RESOURCES, "No more User Configurable Link Resources Available in the Producing Module", "This error is returned from the Target device when no more available Consumer connections available for a Producer."},
    {0x01, 0x031F, PLCTAG_ERR_NO_RESOURCES, "No User Configurable Link Consumer Resources Configured in the Producing Module", "This error is returned from the Target device when no Consumer connections have been configured for a Producer connection."},
    {0x01, 0x0800, PLCTAG_ERR_BAD_DEVICE, "Network Link Offline", "The Link path is invalid or not available."},
    {0x01, 0x0810, PLCTAG_ERR_NO_DATA, "No Target Application Data Available", "This error is returned from the Target device when the application has no valid data to produce."},
    {0x01, 0x0811, PLCTAG_ERR_NO_DATA, "No Originator Application Data Available", "This error is returned from the Originator device when the application has no valid data to produce."},
    {0x01, 0x0812, PLCTAG_ERR_UNSUPPORTED, "Node Address has changed since the Network was scheduled", "This specifies that the router has changed node addresses since the value configured in the original connection."},
    {0x01, 0x0813, PLCTAG_ERR_UNSUPPORTED, "Not Configured for Off-subnet Multicast", "The producer has been requested to support a Multicast connection for a consumer on a different subnet and does not support this functionality."},
    {0x01, 0x0814, PLCTAG_ERR_BAD_DATA, "Invalid Produce/Consume Data format", "Information in the data segment not consistent with the format of the data in the consumed or produced data. Errors 0x0130 and 0x0131 are typically used for this situation in most devices now."},
    {0x02, -1, PLCTAG_ERR_NO_RESOURCES, "Resource Unavailable for Unconnected Send", "The Target device does not have the resources to process the Unconnected Send request."},
    {0x03, -1, PLCTAG_ERR_BAD_PARAM, "Parameter value invalid.", ""},
    {0x04, -1, PLCTAG_ERR_NOT_FOUND,"IOI could not be deciphered or tag does not exist.", "The path segment identifier or the segment syntax was not understood by the target device."},
    {0x05, -1, PLCTAG_ERR_BAD_PARAM, "Path Destination Error", "The Class, Instance or Attribute value specified in the Unconnected Explicit Message request is incorrect or not supported in the Target device. Check the manufacturer's documentation for the correct codes to use."},
    {0x06, -1, PLCTAG_ERR_TOO_LARGE, "Data requested would not fit in response packet.", "The data to be read/written needs to be broken up into multiple packets.0x070000 Connection lost: The messaging connection was lost."},
    {0x07, -1, PLCTAG_ERR_BAD_CONNECTION, "Connection lost", "The messaging connection was lost."},
    {0x08, -1, PLCTAG_ERR_UNSUPPORTED, "Unsupported service.", ""},
    {0x09, -1, PLCTAG_ERR_BAD_DATA, "Error in Data Segment", "This error code is returned when an error is encountered in the Data segment portion of a Forward Open message. The Extended Status value is the offset in the Data segment where the error was encountered."},
    {0x0A, -1, PLCTAG_ERR_BAD_STATUS, "Attribute list error", "An attribute in the Get_Attribute_List or Set_Attribute_List response has a non-zero status."},
    {0x0B, -1, PLCTAG_ERR_DUPLICATE, "Already in requested mode/state", "The object is already in the mode/state being requested by the service."},
    {0x0C, -1, PLCTAG_ERR_BAD_STATUS, "Object State Error", "This error is returned from the Target device when the current state of the Object requested does not allow it to be returned. The current state can be specified in the Optional Extended Error status field."},
    {0x0D, -1, PLCTAG_ERR_DUPLICATE, "Object already exists.", "The requested instance of object to be created already exists."},
    {0x0E, -1, PLCTAG_ERR_NOT_ALLOWED, "Attribute not settable", "A request to modify non-modifiable attribute was received."},
    {0x0F, -1, PLCTAG_ERR_NOT_ALLOWED, "Permission denied.", ""},
    {0x10, -1, PLCTAG_ERR_BAD_STATUS, "Device State Error", "This error is returned from the Target device when the current state of the Device requested does not allow it to be returned. The current state can be specified in the Optional Extended Error status field. Check your configured connections points for other Client devices using this same connection."},
    {0x11, -1, PLCTAG_ERR_TOO_LARGE, "Reply data too large", "The data to be transmitted in the response buffer is larger than the allocated response buffer."},
    {0x12, -1, PLCTAG_ERR_NOT_ALLOWED, "Fragmentation of a primitive value", "The service specified an operation that is going to fragment a primitive data value. For example, trying to send a 2 byte value to a REAL data type (4 byte)."},
    {0x13, -1, PLCTAG_ERR_TOO_SMALL, "Not Enough Data", "Not enough data was supplied in the service request specified."},
    {0x14, -1, PLCTAG_ERR_UNSUPPORTED, "Attribute not supported.", "The attribute specified in the request is not supported."},
    {0x15, -1, PLCTAG_ERR_TOO_LARGE, "Too Much Data", "Too much data was supplied in the service request specified."},
    {0x16, -1, PLCTAG_ERR_NOT_FOUND, "Object does not exist.", "The object specified does not exist in the device."},
    {0x17, -1, PLCTAG_ERR_NOT_ALLOWED, "Service fragmentation sequence not in progress.", "The fragmentation sequence for this service is not currently active for this data."},
    {0x18, -1, PLCTAG_ERR_NO_DATA, "No stored attribute data.", "The attribute data of this object was not saved prior to the requested service."},
    {0x19, -1, PLCTAG_ERR_REMOTE_ERR, "Store operation failure.", "The attribute data of this object was not saved due to a failure during the attempt."},
    {0x1A, -1, PLCTAG_ERR_TOO_LARGE, "Routing failure, request packet too large.", "The service request packet was too large for transmission on a network in the path to the destination."},
    {0x1B, -1, PLCTAG_ERR_TOO_LARGE, "Routing failure, response packet too large.", "The service reponse packet was too large for transmission on a network in the path from the destination."},
    {0x1C, -1, PLCTAG_ERR_NO_DATA, "Missing attribute list entry data.", "The service did not supply an attribute in a list of attributes that was needed by the service to perform the requested behavior."},
    {0x1E, -1, PLCTAG_ERR_PARTIAL, "One or more bundled requests failed..", "One or more of the bundled requests has an error."},
    {0x1D, -1, PLCTAG_ERR_BAD_DATA, "Invalid attribute value list.", "The service is returning the list of attributes supplied with status information for those attributes that were invalid."},
    {0x20, -1, PLCTAG_ERR_BAD_PARAM, "Invalid parameter.", "A parameter associated with the request was invalid. This code is used when a parameter does meet the requirements defined in an Application Object specification."},
    {0x21, -1, PLCTAG_ERR_DUPLICATE, "Write-once value or medium already written.", "An attempt was made to write to a write-once-medium that has already been written or to modify a value that cannot be change once established."},
    {0x22, -1, PLCTAG_ERR_BAD_REPLY, "Invalid Reply Received", "An invalid reply is received (example: service code sent doesn't match service code received.)."},
    {0x25, -1, PLCTAG_ERR_BAD_PARAM, "Key failure in path", "The key segment was included as the first segment in the path does not match the destination module."},
    {0x26, -1, PLCTAG_ERR_BAD_PARAM, "The number of IOI words specified does not match IOI word count.", "Check the tag length against what was sent."},
    {0x27, -1, PLCTAG_ERR_BAD_PARAM, "Unexpected attribute in list", "An attempt was made to set an attribute that is not able to be set at this time."},
    {0x28, -1, PLCTAG_ERR_BAD_PARAM, "Invalid Member ID.", "The Member ID specified in the request does not exist in the specified Class/Instance/Attribute."},
    {0x29, -1, PLCTAG_ERR_NOT_ALLOWED, "Member not writable.", "A request to modify a non-modifiable member was received."},
    {0xFF, 0x2104, PLCTAG_ERR_OUT_OF_BOUNDS, "Address is out of range.",""},
    {0xFF, 0x2105, PLCTAG_ERR_OUT_OF_BOUNDS, "Attempt to access beyond the end of the data object.", ""},
    {0xFF, 0x2107, PLCTAG_ERR_BAD_PARAM, "The data type is invalid or not supported.", ""},
    {-1, -1, PLCTAG_ERR_REMOTE_ERR, "Unknown error code.", "Unknown error code."}
};





static int lookup_error_code(uint8_t err_status, uint16_t extended_err_status)
{
    int index = 0;
    int primary_code = (int)(unsigned int)err_status;
    int secondary_code = (int)(unsigned int)extended_err_status;

    while(error_code_table[index].primary_code != -1) {
        if(error_code_table[index].primary_code == primary_code) {
            if(error_code_table[index].secondary_code == secondary_code || error_code_table[index].secondary_code == -1) {
                break;
            }
        }

        index++;
    }

    return index;
}




const char *cip_decode_error_short(uint8_t err_status, uint16_t extended_err_status)
{
    int index = lookup_error_code(err_status, extended_err_status);

    return error_code_table[index].short_desc;
}


const char *cip_decode_error_long(uint8_t err_status, uint16_t extended_err_status)
{
    int index = lookup_error_code(err_status, extended_err_status);

    return error_code_table[index].long_desc;
}


int cip_decode_error_code(uint8_t err_status, uint16_t extended_err_status)
{
    int index = lookup_error_code(err_status, extended_err_status);

    return error_code_table[index].translated_code;
}

