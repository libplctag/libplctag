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

#include <limits.h>
#include <float.h>
#include <platform.h>
#include <lib/libplctag.h>
#include <mb/modbus.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/rc.h>

/* data definitions */
struct modbus_plc_t {
    struct modbus_plc_t *next;
    struct modbus_tag_t *tags;
};

typedef struct modbus_plc_t *modbus_plc_p;

typedef enum { MB_REG_UNKNOWN, MB_REG_DO, MB_REG_DI, MB_REG_AO, MB_REG_AI } modbus_reg_type_t;


struct modbus_tag_t {
    /* base tag parts. */
    TAG_BASE_STRUCT;

    /* next one in the list for this PLC */
    struct modbus_tag_t *next;

    /* register type. */
    modbus_reg_type_t reg_type;

    /* data for the tag. */
    int elem_count;
    int elem_size;
};

typedef struct modbus_tag_t *modbus_tag_p;


/* Modbus module globals. */
mutex_p mb_mutex = NULL;

volatile int library_terminating = 0;


/* helper functions */
static int create_tag_object(attr attribs, modbus_tag_p *tag);
static int find_or_create_plc(attr attribs, modbus_plc_p *plc);
static int get_tag_type(attr attribs);

/* tag vtable functions. */

/* control functions. */
static int mb_abort(plc_tag_p p_tag);
static int mb_read_start(plc_tag_p p_tag);
static int mb_tag_status(plc_tag_p p_tag);
static int mb_tickler(plc_tag_p p_tag);
static int mb_write_start(plc_tag_p p_tag);

/* data accessors */
static int mb_get_int_attrib(plc_tag_p tag, const char *attrib_name, int default_value);
static int mb_set_int_attrib(plc_tag_p tag, const char *attrib_name, int new_value);

static int mb_get_bit(plc_tag_p tag, int offset_bit);
static int mb_set_bit(plc_tag_p tag, int offset_bit, int val);

static uint64_t mb_get_uint64(plc_tag_p tag, int offset);
static int mb_set_uint64(plc_tag_p tag, int offset, uint64_t val);

static int64_t mb_get_int64(plc_tag_p tag, int offset);
static int mb_set_int64(plc_tag_p tag, int offset, int64_t val);

static uint32_t mb_get_uint32(plc_tag_p tag, int offset);
static int mb_set_uint32(plc_tag_p tag, int offset, uint32_t val);

static int32_t mb_get_int32(plc_tag_p tag, int offset);
static int mb_set_int32(plc_tag_p tag, int offset, int32_t val);

static uint16_t mb_get_uint16(plc_tag_p tag, int offset);
static int mb_set_uint16(plc_tag_p tag, int offset, uint16_t val);

static int16_t mb_get_int16(plc_tag_p tag, int offset);
static int mb_set_int16(plc_tag_p tag, int offset, int16_t val);

static uint8_t mb_get_uint8(plc_tag_p tag, int offset);
static int mb_set_uint8(plc_tag_p tag, int offset, uint8_t val);

static int8_t mb_get_int8(plc_tag_p tag, int offset);
static int mb_set_int8(plc_tag_p tag, int offset, int8_t val);

static double mb_get_float64(plc_tag_p tag, int offset);
static int mb_set_float64(plc_tag_p tag, int offset, double val);

static float mb_get_float32(plc_tag_p tag, int offset);
static int mb_set_float32(plc_tag_p tag, int offset, float val);

struct tag_vtable_t modbus_vtable = {
    (tag_vtable_func)mb_abort, 
    (tag_vtable_func)mb_read_start,
    (tag_vtable_func)mb_tag_status, /* shared */
    (tag_vtable_func)mb_tickler,
    (tag_vtable_func)mb_write_start,

    /* data accessors */
    mb_get_int_attrib,
    mb_set_int_attrib,

    mb_get_bit,
    mb_set_bit,

    mb_get_uint64,
    mb_set_uint64,

    mb_get_int64,
    mb_set_int64,

    mb_get_uint32,
    mb_set_uint32,

    mb_get_int32,
    mb_set_int32,

    mb_get_uint16,
    mb_set_uint16,

    mb_get_int16,
    mb_set_int16,

    mb_get_uint8,
    mb_set_uint8,

    mb_get_int8,
    mb_set_int8,

    mb_get_float64,
    mb_set_float64,

    mb_get_float32,
    mb_set_float32
};


/****** main entry point *******/

plc_tag_p mb_tag_create(attr attribs)
{
    int rc = PLCTAG_STATUS_OK;
    modbus_tag_p tag = NULL;
    modbus_plc_p plc = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    /* create the tag object. */
    rc = create_tag_object(attribs, &tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create new tag!  Error %s!", plc_tag_decode_error(rc));
        return NULL;
    }

    /* find the PLC object. */
    rc = find_or_create_plc(attribs, &plc);
    // if(rc != PLCTAG_STATUS_OK) {
    //     pdebug(DEBUG_WARN, "Unable to create new tag!  Error %s!", plc_tag_decode_error(rc));
    //     return NULL;
    // }



    pdebug(DEBUG_INFO, "Done.");

    return NULL;
}

/***** helper functions *****/

int create_tag_object(attr attribs, modbus_tag_p *tag)
{
    int data_size = 0;
    int reg_size = 0;
    int elem_count = attr_get_int(attribs, "elem_count", 1);
    int reg_type = get_tag_type(attribs);

    pdebug(DEBUG_INFO, "Starting.");

    /* determine register type. */
    switch(reg_type) {
        case MB_REG_DO:
            /* fall through */
        case MB_REG_DI:
            reg_size = 1;
            break;

        case MB_REG_AO:
            /* fall through */
        case MB_REG_AI:
            reg_size = 16;
            break;

        default:
            reg_size = 0;
    }

    /* calculate the data size in bytes. */
    data_size = ((elem_count * reg_size) + 7) / 8;

    pdebug(DEBUG_DETAIL, "Tag data size is %d bytes.", data_size);

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_ERR_CREATE;
}





int find_or_create_plc(attr attribs, modbus_plc_p *plc)
{
    pdebug(DEBUG_INFO, "Starting.");
    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_ERR_CREATE;
}



int get_tag_type(attr attribs)
{
    int res = MB_REG_UNKNOWN;
    const char *reg_type = attr_get_int(attribs, "reg_type", "NONE");

    pdebug(DEBUG_INFO, "Starting.");

    /* determine register type. */
    if(str_cmp_i(reg_type, "do") == 0) {
        res = MB_REG_DO;
    } else if(str_cmp_i(reg_type, "di") == 0) {
        res = MB_REG_DI;
    } else if(str_cmp_i(reg_type, "ao") == 0) {
        res = MB_REG_AO;
    } else if(str_cmp_i(reg_type, "ai") == 0) {
        res = MB_REG_AI;
    } 

    pdebug(DEBUG_DETAIL, "Done.");

    return res;
}









/****** Data Accessor Functions ******/


int mb_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value)
{
    int res = default_value;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* match the attribute. */
    if(str_cmp_i(attrib_name, "elem_size") == 0) {
        res = tag->elem_size;
    } else if(str_cmp_i(attrib_name, "elem_count") == 0) {
        res = tag->elem_count;
    }

    return res;
}


int mb_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value)
{
    (void)raw_tag;
    (void)attrib_name;
    (void)new_value;

    return PLCTAG_ERR_UNSUPPORTED;
}



int mb_get_bit(plc_tag_p raw_tag, int offset_bit)
{
    int res = PLCTAG_ERR_OUT_OF_BOUNDS;
    int real_offset = offset_bit;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return PLCTAG_ERR_NO_DATA;
    }

    /* if this is a single bit, then make sure the offset is the tag bit. */
    if(tag->is_bit) {
        real_offset = tag->bit;
    } else {
        real_offset = offset_bit;
    }

    /* is there enough data */
    pdebug(DEBUG_SPEW, "real_offset=%d, byte offset=%d, tag size=%d", real_offset, (real_offset/8), tag->size);
    if((real_offset < 0) || ((real_offset / 8) >= tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    pdebug(DEBUG_SPEW, "selecting bit %d with offset %d in byte %d (%x).", real_offset, (real_offset % 8), (real_offset / 8), tag->data[real_offset / 8]);

    res = !!(((1 << (real_offset % 8)) & 0xFF) & (tag->data[real_offset / 8]));

    return res;
}


int mb_set_bit(plc_tag_p raw_tag, int offset_bit, int val)
{
    int res = PLCTAG_STATUS_OK;
    int real_offset = offset_bit;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return PLCTAG_ERR_NO_DATA;
    }

    /* if this is a single bit, then make sure the offset is the tag bit. */
    if(tag->is_bit) {
        real_offset = tag->bit;
    } else {
        real_offset = offset_bit;
    }

    /* is there enough data */
    if((real_offset < 0) || ((real_offset / 8) >= tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    if(val) {
        tag->data[real_offset / 8] |= (uint8_t)(1 << (real_offset % 8));
    } else {
        tag->data[real_offset / 8] &= (uint8_t)(~(1 << (real_offset % 8)));
    }

    res = PLCTAG_STATUS_OK;

    return res;
}



uint64_t mb_get_uint64(plc_tag_p raw_tag, int offset)
{
    uint64_t res = UINT64_MAX;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return res;
    }

    /* is there enough data? */
    if((offset < 0) || (offset + ((int)sizeof(uint64_t)) > tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return res;
    }

    if(!tag->is_bit) {
        res = ((uint64_t)(tag->data[offset])) +
              ((uint64_t)(tag->data[offset+1]) << 8) +
              ((uint64_t)(tag->data[offset+2]) << 16) +
              ((uint64_t)(tag->data[offset+3]) << 24) +
              ((uint64_t)(tag->data[offset+4]) << 32) +
              ((uint64_t)(tag->data[offset+5]) << 40) +
              ((uint64_t)(tag->data[offset+6]) << 48) +
              ((uint64_t)(tag->data[offset+7]) << 56);
    } else {
        if(mb_get_bit(raw_tag, 0)) {
            res = 1;
        } else {
            res = 0;
        }
    }

    return res;
}



int mb_set_uint64(plc_tag_p raw_tag, int offset, uint64_t val)
{
    int rc = PLCTAG_STATUS_OK;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return PLCTAG_ERR_NO_DATA;
    }

    /* is there enough data */
    if((offset < 0) || (offset + ((int)sizeof(uint64_t)) > tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    /* write the data. */
    if(!tag->is_bit) {
        tag->data[offset]   = (uint8_t)(val & 0xFF);
        tag->data[offset+1] = (uint8_t)((val >> 8) & 0xFF);
        tag->data[offset+2] = (uint8_t)((val >> 16) & 0xFF);
        tag->data[offset+3] = (uint8_t)((val >> 24) & 0xFF);
        tag->data[offset+4] = (uint8_t)((val >> 32) & 0xFF);
        tag->data[offset+5] = (uint8_t)((val >> 40) & 0xFF);
        tag->data[offset+6] = (uint8_t)((val >> 48) & 0xFF);
        tag->data[offset+7] = (uint8_t)((val >> 56) & 0xFF);
    } else {
        if(!val) {
            rc = mb_set_bit(raw_tag, 0, 0);
        } else {
            rc = mb_set_bit(raw_tag, 0, 1);
        }
    }

    return rc;
}




int64_t mb_get_int64(plc_tag_p raw_tag, int offset)
{
    int64_t res = INT64_MIN;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return res;
    }

    /* is there enough data */
    if((offset < 0) || (offset + ((int)sizeof(int64_t)) > tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return res;
    }

    if(!tag->is_bit) {
        res = (int64_t)(((uint64_t)(tag->data[offset])) +
                        ((uint64_t)(tag->data[offset+1]) << 8) +
                        ((uint64_t)(tag->data[offset+2]) << 16) +
                        ((uint64_t)(tag->data[offset+3]) << 24) +
                        ((uint64_t)(tag->data[offset+4]) << 32) +
                        ((uint64_t)(tag->data[offset+5]) << 40) +
                        ((uint64_t)(tag->data[offset+6]) << 48) +
                        ((uint64_t)(tag->data[offset+7]) << 56));
    } else {
        if(mb_get_bit(raw_tag, 0)) {
            res = 1;
        } else {
            res = 0;
        }
    }

    return res;
}



int mb_set_int64(plc_tag_p raw_tag, int offset, int64_t ival)
{
    uint64_t val = (uint64_t)(ival);
    int rc = PLCTAG_STATUS_OK;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return PLCTAG_ERR_NO_DATA;
    }

    /* is there enough data */
    if((offset < 0) || (offset + ((int)sizeof(int64_t)) > tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    if(!tag->is_bit) {
        tag->data[offset]   = (uint8_t)(val & 0xFF);
        tag->data[offset+1] = (uint8_t)((val >> 8) & 0xFF);
        tag->data[offset+2] = (uint8_t)((val >> 16) & 0xFF);
        tag->data[offset+3] = (uint8_t)((val >> 24) & 0xFF);
        tag->data[offset+4] = (uint8_t)((val >> 32) & 0xFF);
        tag->data[offset+5] = (uint8_t)((val >> 40) & 0xFF);
        tag->data[offset+6] = (uint8_t)((val >> 48) & 0xFF);
        tag->data[offset+7] = (uint8_t)((val >> 56) & 0xFF);
    } else {
        if(!val) {
            rc = mb_set_bit(raw_tag, 0, 0);
        } else {
            rc = mb_set_bit(raw_tag, 0, 1);
        }
    }

    return rc;
}









uint32_t mb_get_uint32(plc_tag_p raw_tag, int offset)
{
    uint32_t res = UINT32_MAX;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return res;
    }

    /* is there enough data */
    if((offset < 0) || (offset + ((int)sizeof(uint32_t)) > tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return res;
    }

    if(!tag->is_bit) {
        res = ((uint32_t)(tag->data[offset])) +
              ((uint32_t)(tag->data[offset+1]) << 8) +
              ((uint32_t)(tag->data[offset+2]) << 16) +
              ((uint32_t)(tag->data[offset+3]) << 24);
    } else {
        if(mb_get_bit(raw_tag, 0)) {
            res = 1;
        } else {
            res = 0;
        }
    }

    return res;
}



int mb_set_uint32(plc_tag_p raw_tag, int offset, uint32_t val)
{
    int rc = PLCTAG_STATUS_OK;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return PLCTAG_ERR_NO_DATA;
    }

    /* is there enough data */
    if((offset < 0) || (offset + ((int)sizeof(uint32_t)) > tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    /* write the data. */
    if(!tag->is_bit) {
        tag->data[offset]   = (uint8_t)(val & 0xFF);
        tag->data[offset+1] = (uint8_t)((val >> 8) & 0xFF);
        tag->data[offset+2] = (uint8_t)((val >> 16) & 0xFF);
        tag->data[offset+3] = (uint8_t)((val >> 24) & 0xFF);
    } else {
        if(!val) {
            rc = mb_set_bit(raw_tag, 0, 0);
        } else {
            rc = mb_set_bit(raw_tag, 0, 1);
        }
    }

    return rc;
}




int32_t mb_get_int32(plc_tag_p raw_tag, int offset)
{
    int32_t res = INT32_MIN;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return res;
    }

    /* is there enough data */
    if((offset < 0) || (offset + ((int)sizeof(int32_t)) > tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return res;
    }

    if(!tag->is_bit) {
        res = (int32_t)(((uint32_t)(tag->data[offset])) +
                        ((uint32_t)(tag->data[offset+1]) << 8) +
                        ((uint32_t)(tag->data[offset+2]) << 16) +
                        ((uint32_t)(tag->data[offset+3]) << 24));
    } else {
        if(mb_get_bit(raw_tag, 0)) {
            res = 1;
        } else {
            res = 0;
        }
    }

    return res;
}



int mb_set_int32(plc_tag_p raw_tag, int offset, int32_t ival)
{
    uint32_t val = (uint32_t)(ival);
    int rc = PLCTAG_STATUS_OK;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return PLCTAG_ERR_NO_DATA;
    }

    /* is there enough data */
    if((offset < 0) || (offset + ((int)sizeof(int32_t)) > tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    if(!tag->is_bit) {
        tag->data[offset]   = (uint8_t)(val & 0xFF);
        tag->data[offset+1] = (uint8_t)((val >> 8) & 0xFF);
        tag->data[offset+2] = (uint8_t)((val >> 16) & 0xFF);
        tag->data[offset+3] = (uint8_t)((val >> 24) & 0xFF);
    } else {
        if(!val) {
            rc = mb_set_bit(raw_tag, 0, 0);
        } else {
            rc = mb_set_bit(raw_tag, 0, 1);
        }
    }

    return rc;
}









uint16_t mb_get_uint16(plc_tag_p raw_tag, int offset)
{
    uint16_t res = UINT16_MAX;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return res;
    }

    /* is there enough data */
    if((offset < 0) || (offset + ((int)sizeof(uint16_t)) > tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return res;
    }

    if(!tag->is_bit) {
        res = (uint16_t)((tag->data[offset]) +
                        ((tag->data[offset+1]) << 8));
    } else {
        if(mb_get_bit(raw_tag, 0)) {
            res = 1;
        } else {
            res = 0;
        }
    }

    return res;
}




int mb_set_uint16(plc_tag_p raw_tag, int offset, uint16_t val)
{
    int rc = PLCTAG_STATUS_OK;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return PLCTAG_ERR_NO_DATA;
    }

    /* is there enough data */
    if((offset < 0) || (offset + ((int)sizeof(uint16_t)) > tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    if(!tag->is_bit) {
        tag->data[offset]   = (uint8_t)(val & 0xFF);
        tag->data[offset+1] = (uint8_t)((val >> 8) & 0xFF);
    } else {
        if(!val) {
            rc = mb_set_bit(raw_tag, 0, 0);
        } else {
            rc = mb_set_bit(raw_tag, 0, 1);
        }
    }

    return rc;
}









int16_t  mb_get_int16(plc_tag_p raw_tag, int offset)
{
    int16_t res = INT16_MIN;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return res;
    }

    /* is there enough data */
    if((offset < 0) || (offset + ((int)sizeof(int16_t)) > tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return res;
    }

    if(!tag->is_bit) {
        res = (int16_t)(((tag->data[offset])) +
                        ((tag->data[offset+1]) << 8));
    } else {
        if(mb_get_bit(raw_tag, 0)) {
            res = 1;
        } else {
            res = 0;
        }
    }

    return res;
}




int mb_set_int16(plc_tag_p raw_tag, int offset, int16_t ival)
{
    uint16_t val = (uint16_t)(ival);
    int rc = PLCTAG_STATUS_OK;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return PLCTAG_ERR_NO_DATA;
    }

    /* is there enough data */
    if((offset < 0) || (offset + ((int)sizeof(int16_t)) > tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    if(!tag->is_bit) {
        tag->data[offset]   = (uint8_t)(val & 0xFF);
        tag->data[offset+1] = (uint8_t)((val >> 8) & 0xFF);
    } else {
        if(!val) {
            rc = mb_set_bit(raw_tag, 0, 0);
        } else {
            rc = mb_set_bit(raw_tag, 0, 1);
        }
    }

    return rc;
}



uint8_t mb_get_uint8(plc_tag_p raw_tag, int offset)
{
    uint8_t res = UINT8_MAX;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return res;
    }

    /* is there enough data */
    if((offset < 0) || (offset + ((int)sizeof(uint8_t)) > tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return res;
    }

    if(!tag->is_bit) {
        res = tag->data[offset];
    } else {
        if(mb_get_bit(raw_tag, 0)) {
            res = 1;
        } else {
            res = 0;
        }
    }

    return res;
}




int mb_set_uint8(plc_tag_p raw_tag, int offset, uint8_t val)
{
    int rc = PLCTAG_STATUS_OK;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return PLCTAG_ERR_NO_DATA;
    }

    /* is there enough data */
    if((offset < 0) || (offset + ((int)sizeof(uint8_t)) > tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    if(!tag->is_bit) {
        tag->data[offset] = val;
    } else {
        if(!val) {
            rc = mb_set_bit(raw_tag, 0, 0);
        } else {
            rc = mb_set_bit(raw_tag, 0, 1);
        }
    }

    return rc;
}





int8_t mb_get_int8(plc_tag_p raw_tag, int offset)
{
    int8_t res = INT8_MIN;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return res;
    }

    /* is there enough data */
    if((offset < 0) || (offset + ((int)sizeof(int8_t)) > tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return res;
    }

    if(!tag->is_bit) {
        res = (int8_t)(tag->data[offset]);
    } else {
        if(mb_get_bit(raw_tag, 0)) {
            res = 1;
        } else {
            res = 0;
        }
    }

    return res;
}




int mb_set_int8(plc_tag_p raw_tag, int offset, int8_t ival)
{
    uint8_t val = (uint8_t)(ival);
    int rc = PLCTAG_STATUS_OK;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return PLCTAG_ERR_NO_DATA;
    }

    /* is there enough data */
    if((offset < 0) || (offset + ((int)sizeof(int8_t)) > tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    if(!tag->is_bit) {
        tag->data[offset] = (uint8_t)val;
    } else {
        if(!val) {
            rc = mb_set_bit(raw_tag, 0, 0);
        } else {
            rc = mb_set_bit(raw_tag, 0, 1);
        }
    }

    return rc;
}






double mb_get_float64(plc_tag_p raw_tag, int offset)
{
    uint64_t ures = 0;
    double res = DBL_MAX;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    if(tag->is_bit) {
        pdebug(DEBUG_WARN, "Getting float64 value is unsupported on a bit tag!");
        return res;
    }

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return res;
    }

    /* is there enough data */
    if((offset < 0) || (offset + ((int)sizeof(ures)) > tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return res;
    }

    ures = ((uint64_t)(tag->data[offset])) +
           ((uint64_t)(tag->data[offset+1]) << 8) +
           ((uint64_t)(tag->data[offset+2]) << 16) +
           ((uint64_t)(tag->data[offset+3]) << 24) +
           ((uint64_t)(tag->data[offset+4]) << 32) +
           ((uint64_t)(tag->data[offset+5]) << 40) +
           ((uint64_t)(tag->data[offset+6]) << 48) +
           ((uint64_t)(tag->data[offset+7]) << 56);

    /* copy the data */
    mem_copy(&res,&ures,sizeof(res));

    return res;
}




int mb_set_float64(plc_tag_p raw_tag, int offset, double fval)
{
    int rc = PLCTAG_STATUS_OK;
    uint64_t val = 0;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    if(tag->is_bit) {
        pdebug(DEBUG_WARN, "Setting float64 value is unsupported on a bit tag!");
        return PLCTAG_ERR_UNSUPPORTED;
    }

    mem_copy(&val, &fval, sizeof(val));

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return PLCTAG_ERR_NO_DATA;
    }

    /* is there enough data */
    if((offset < 0) || (offset + ((int)sizeof(val)) > tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    tag->data[offset]   = (uint8_t)(val & 0xFF);
    tag->data[offset+1] = (uint8_t)((val >> 8) & 0xFF);
    tag->data[offset+2] = (uint8_t)((val >> 16) & 0xFF);
    tag->data[offset+3] = (uint8_t)((val >> 24) & 0xFF);
    tag->data[offset+4] = (uint8_t)((val >> 32) & 0xFF);
    tag->data[offset+5] = (uint8_t)((val >> 40) & 0xFF);
    tag->data[offset+6] = (uint8_t)((val >> 48) & 0xFF);
    tag->data[offset+7] = (uint8_t)((val >> 56) & 0xFF);

    return rc;
}



float mb_get_float32(plc_tag_p raw_tag, int offset)
{
    uint32_t ures = (uint32_t)0;
    float res = FLT_MAX;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    if(tag->is_bit) {
        pdebug(DEBUG_WARN, "Getting float32 value is unsupported on a bit tag!");
        return res;
    }

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return res;
    }

    /* is there enough data */
    if((offset < 0) || (offset + ((int)sizeof(ures)) > tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return res;
    }

    ures = ((uint32_t)(tag->data[offset])) +
           ((uint32_t)(tag->data[offset+1]) << 8) +
           ((uint32_t)(tag->data[offset+2]) << 16) +
           ((uint32_t)(tag->data[offset+3]) << 24);

    //pdebug(DEBUG_DETAIL, "ures=%lu", ures);

    /* copy the data */
    mem_copy(&res,&ures,sizeof(res));

    return res;
}




int mb_set_float32(plc_tag_p raw_tag, int offset, float fval)
{
    int rc = PLCTAG_STATUS_OK;
    uint32_t val = 0;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    if(tag->is_bit) {
        pdebug(DEBUG_WARN, "Setting float32 value is unsupported on a bit tag!");
        return PLCTAG_ERR_UNSUPPORTED;
    }

    mem_copy(&val, &fval, sizeof(val));

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return PLCTAG_ERR_NO_DATA;
    }

    /* is there enough data */
    if((offset < 0) || (offset + ((int)sizeof(val)) > tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    tag->data[offset]   = (uint8_t)(val & 0xFF);
    tag->data[offset+1] = (uint8_t)((val >> 8) & 0xFF);
    tag->data[offset+2] = (uint8_t)((val >> 16) & 0xFF);
    tag->data[offset+3] = (uint8_t)((val >> 24) & 0xFF);

    return rc;
}









/****** Library level functions. *******/

void mb_teardown(void)
{
    pdebug(DEBUG_INFO, "Starting.");

    library_terminating = 1;

    pdebug(DEBUG_DETAIL, "Destroying Modbus mutex.");
    if(mb_mutex) {
        mutex_destroy(&mb_mutex);
        mb_mutex = NULL;
    }

    pdebug(DEBUG_INFO, "Done.");
}



int mb_init()
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    pdebug(DEBUG_DETAIL, "Setting up mutex.");
    if(!mb_mutex) {
        rc = mutex_create(&mb_mutex);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error %s creating mutex!", plc_tag_decode_error(rc));
            return rc;
        }
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}

