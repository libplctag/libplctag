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

    /* keep a list of tags for this PLC. */
    struct modbus_tag_t *tags;

    /* hostname/ip and possibly port of the server. */
    char *server;

    /* flags */
    struct {
        unsigned int terminate:1;
    } flags;

    /* thread related state */
    thread_p handler_thread;
    mutex_p mutex;

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

    /* the PLC we are using */
    modbus_plc_p plc;

    /* actions */
    struct {
        unsigned int abort:1;
        unsigned int read:1;
        unsigned int write:1;
    } flags;

    /* data for the tag. */
    int elem_count;
    int elem_size;
};

typedef struct modbus_tag_t *modbus_tag_p;


/* Modbus module globals. */
mutex_p mb_mutex = NULL;
modbus_plc_p plcs = NULL;
volatile int library_terminating = 0;


/* helper functions */
static int create_tag_object(attr attribs, modbus_tag_p *tag);
static int find_or_create_plc(attr attribs, modbus_plc_p *plc);
static int get_tag_type(attr attribs);
static void modbus_tag_destructor(void *tag_arg);
static void modbus_plc_destructor(void *plc_arg);
static THREAD_FUNC(modbus_plc_handler);

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

    pdebug(DEBUG_INFO, "Starting.");

    /* create the tag object. */
    rc = create_tag_object(attribs, &tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create new tag!  Error %s!", plc_tag_decode_error(rc));
        return NULL;
    }

    /* find the PLC object. */
    rc = find_or_create_plc(attribs, &(tag->plc));
    if(rc == PLCTAG_STATUS_OK) {
        /* put the tag on the PLC's list. */
        critical_block(tag->plc->mutex) {
            tag->next = tag->plc->tags;
            tag->plc->tags = tag;
        }
    } else {
        pdebug(DEBUG_WARN, "Unable to create new tag!  Error %s!", plc_tag_decode_error(rc));
        tag->status = rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return (plc_tag_p)tag;
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

    /* allocate the tag */
    *tag = (modbus_tag_p)rc_alloc((int)(unsigned int)sizeof(struct modbus_tag_t)+data_size, modbus_tag_destructor);
    if(! *tag) {
        pdebug(DEBUG_WARN, "Unable to allocate Modbus tag!");
        return PLCTAG_ERR_NO_MEM;
    }

    /* point the data just after the tag struct. */
    (*tag)->data = (uint8_t *)((*tag) + 1);

    /* set the various size/element fields. */
    (*tag)->elem_count = elem_count;
    (*tag)->elem_size = reg_size;
    (*tag)->size = data_size;

    /* set up the vtable */
    (*tag)->vtable = &modbus_vtable;

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}



void modbus_tag_destructor(void *tag_arg) 
{
    modbus_tag_p tag = (modbus_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Destructor called with null pointer!");
        return;
    }

    if(tag->plc) {
        /* unlink the tag from the PLC. */
        critical_block(tag->plc->mutex) {
            modbus_tag_p *tag_walker = &(tag->plc->tags);

            while(*tag_walker && *tag_walker != tag) {
                tag_walker = &((*tag_walker)->next);
            }

            if(*tag_walker) {
                *tag_walker = tag->next;
            } else {
                pdebug(DEBUG_WARN, "Tag not found on PLC list!");
            }
        }

        tag->plc = rc_dec(tag->plc);
    }

    pdebug(DEBUG_INFO, "Done.");
}




int find_or_create_plc(attr attribs, modbus_plc_p *plc)
{
    const char *server = attr_get_str(attribs, "server", NULL);
    int is_new = 0;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    /* see if we can find a matching server. */
    critical_block(mb_mutex) {
        modbus_plc_p *walker = &plcs;

        while(*walker && str_cmp_i(server, (*walker)->server) != 0) {
            walker = &((*walker)->next);
        }

        /* did we find one. */
        if(*walker) {
            *plc = *walker;
            is_new = 0;
        } else {
            /* nope, make a new one.  Do as little as possible in the mutex. */
            is_new = 1;

            *plc = (modbus_plc_p)rc_alloc((int)(unsigned int)sizeof(struct modbus_plc_t), modbus_plc_destructor);
            if(*plc) {
                /* copy the server string so that we can find this again. */
                (*plc)->server = str_dup(server);
                if(! ((*plc)->server)) {
                    pdebug(DEBUG_WARN, "Unable to allocate Modbus PLC server string!");
                    rc = PLCTAG_ERR_NO_MEM;
                } else {
                    /* link up the list. */
                    (*plc)->next = plcs;
                    plcs = *plc;
                }
            } else {
                pdebug(DEBUG_WARN, "Unable to allocate Modbus PLC object!");
                rc = PLCTAG_ERR_NO_MEM;
            }
        }
    }

    /* if everything went well and it is new, set up the new PLC. */
    if(rc == PLCTAG_STATUS_OK) {
        if(is_new) {
            pdebug(DEBUG_INFO, "Creating new PLC.");

            rc = thread_create(&((*plc)->handler_thread), modbus_plc_handler, 32768, (void *)(*plc));
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unable to create new handler thread, error %s!", plc_tag_decode_error(rc));
            } else {
                rc = mutex_create(&((*plc)->mutex));
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Unable to create new mutex, error %s!", plc_tag_decode_error(rc));
                }
            }
        }
    }

    if(rc != PLCTAG_STATUS_OK && *plc) {
        pdebug(DEBUG_WARN, "PLC lookup and/or creation failed!");

        /* clean up. */
        *plc = rc_dec(*plc);

    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}



void modbus_plc_destructor(void *plc_arg) 
{
    modbus_plc_p plc = (modbus_plc_p)plc_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!plc) {
        pdebug(DEBUG_WARN, "Destructor called with null pointer!");
        return;
    }

    /* remove the plc from the list. */
    critical_block(mb_mutex) {
        modbus_plc_p *walker = &plcs;

        while(*walker && *walker != plc) {
            walker = &((*walker)->next);
        }

        if(*walker) {
            /* unlink the list. */
            *walker = plc->next;
            plc->next = NULL;
        } else {
            pdebug(DEBUG_WARN, "PLC not found in the list!");
        }
    }

    /* shut down the thread. */
    if(plc->handler_thread) {
        plc->flags.terminate = 1;
        thread_join(plc->handler_thread);
        plc->handler_thread = NULL;
    }

    if(plc->mutex) {
        mutex_destroy(&plc->mutex);
        plc->mutex = NULL;
    }

    if(plc->server) {
        mem_free(plc->server);
        plc->server = NULL;
    }

    if(plc->tags) {
        pdebug(DEBUG_WARN, "There are tags still remaining, memory leak possible!");
    }

    pdebug(DEBUG_INFO, "Done.");
}



THREAD_FUNC(modbus_plc_handler)
{
    modbus_plc_p plc = (modbus_plc_p)arg;

    if(!plc) {
        pdebug(DEBUG_WARN, "Null PLC pointer passed!");
        return NULL;
    }

    pdebug(DEBUG_INFO, "Starting.");

    while(! plc->flags.terminate) {
        int keep_going = 0;



        if(!keep_going) {
            sleep_ms(1);
        }
    }

    pdebug(DEBUG_INFO, "Done.");

    return NULL;
}


int get_tag_type(attr attribs)
{
    int res = MB_REG_UNKNOWN;
    const char *reg_type = attr_get_str(attribs, "reg_type", "NONE");

    pdebug(DEBUG_DETAIL, "Starting.");

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








/****** Tag Control Functions ******/

int mb_abort(plc_tag_p p_tag)
{
    modbus_tag_p tag = (modbus_tag_p)p_tag;

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    tag->flags.abort = 1;
    tag->flags.read = 0;
    tag->flags.write = 0;

    return PLCTAG_STATUS_OK;
}



int mb_read_start(plc_tag_p p_tag)
{
    modbus_tag_p tag = (modbus_tag_p)p_tag;

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(tag->flags.abort || tag->flags.read || tag->flags.write) {
        return PLCTAG_ERR_BUSY;
    }

    tag->flags.read = 1;
    tag->status = PLCTAG_STATUS_OK;

    return PLCTAG_STATUS_OK;
}



int mb_tag_status(plc_tag_p p_tag)
{
    modbus_tag_p tag = (modbus_tag_p)p_tag;

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(tag->status != PLCTAG_STATUS_OK) {
        return tag->status;
    }

    if(tag->flags.abort || tag->flags.read || tag->flags.write) {
        return PLCTAG_STATUS_PENDING;
    }

    return PLCTAG_STATUS_OK;
}


/* not used. */
int mb_tickler(plc_tag_p p_tag)
{
    (void)p_tag;

    return PLCTAG_STATUS_OK;
}



int mb_write_start(plc_tag_p p_tag)
{
    modbus_tag_p tag = (modbus_tag_p)p_tag;

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(tag->flags.abort || tag->flags.read || tag->flags.write) {
        return PLCTAG_ERR_BUSY;
    }

    tag->flags.write = 1;
    tag->status = PLCTAG_STATUS_OK;

    return PLCTAG_STATUS_OK;
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
    } else {
        pdebug(DEBUG_WARN,"Attribute \"%s\" is not supported.", attrib_name);
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
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return PLCTAG_ERR_NO_DATA;
    }

    /* is there enough data */
    pdebug(DEBUG_SPEW, "offset_bit=%d, byte offset=%d, tag size=%d", offset_bit, (offset_bit/8), tag->size);
    if((offset_bit < 0) || ((offset_bit / 8) >= tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    pdebug(DEBUG_SPEW, "selecting bit %d with offset %d in byte %d (%x).", offset_bit, (offset_bit % 8), (offset_bit / 8), tag->data[offset_bit / 8]);

    res = !!(((1 << (offset_bit % 8)) & 0xFF) & (tag->data[offset_bit / 8]));

    return res;
}


int mb_set_bit(plc_tag_p raw_tag, int offset_bit, int val)
{
    int res = PLCTAG_STATUS_OK;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN,"Tag has no data!");
        return PLCTAG_ERR_NO_DATA;
    }

    /* is there enough data */
    if((offset_bit < 0) || ((offset_bit / 8) >= tag->size)) {
        pdebug(DEBUG_WARN,"Data offset out of bounds.");
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    if(val) {
        tag->data[offset_bit / 8] |= (uint8_t)(1 << (offset_bit % 8));
    } else {
        tag->data[offset_bit / 8] &= (uint8_t)(~(1 << (offset_bit % 8)));
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

    res =   ((uint64_t)(tag->data[offset+7])) +
            ((uint64_t)(tag->data[offset+6]) << 8) +
            ((uint64_t)(tag->data[offset+5]) << 16) +
            ((uint64_t)(tag->data[offset+4]) << 24) +
            ((uint64_t)(tag->data[offset+3]) << 32) +
            ((uint64_t)(tag->data[offset+2]) << 40) +
            ((uint64_t)(tag->data[offset+1]) << 48) +
            ((uint64_t)(tag->data[offset+0]) << 56);

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
    tag->data[offset+7] = (uint8_t)((val >> 0 ) & 0xFF);
    tag->data[offset+6] = (uint8_t)((val >> 8 ) & 0xFF);
    tag->data[offset+5] = (uint8_t)((val >> 16) & 0xFF);
    tag->data[offset+4] = (uint8_t)((val >> 24) & 0xFF);
    tag->data[offset+3] = (uint8_t)((val >> 32) & 0xFF);
    tag->data[offset+2] = (uint8_t)((val >> 40) & 0xFF);
    tag->data[offset+1] = (uint8_t)((val >> 48) & 0xFF);
    tag->data[offset+0] = (uint8_t)((val >> 56) & 0xFF);

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

    res = (int64_t)(((uint64_t)(tag->data[offset+7]) << 0 ) +
                    ((uint64_t)(tag->data[offset+6]) << 8 ) +
                    ((uint64_t)(tag->data[offset+5]) << 16) +
                    ((uint64_t)(tag->data[offset+4]) << 24) +
                    ((uint64_t)(tag->data[offset+3]) << 32) +
                    ((uint64_t)(tag->data[offset+2]) << 40) +
                    ((uint64_t)(tag->data[offset+1]) << 48) +
                    ((uint64_t)(tag->data[offset+0]) << 56));

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

    tag->data[offset+7] = (uint8_t)((val >> 0 ) & 0xFF);
    tag->data[offset+6] = (uint8_t)((val >> 8 ) & 0xFF);
    tag->data[offset+5] = (uint8_t)((val >> 16) & 0xFF);
    tag->data[offset+4] = (uint8_t)((val >> 24) & 0xFF);
    tag->data[offset+3] = (uint8_t)((val >> 32) & 0xFF);
    tag->data[offset+2] = (uint8_t)((val >> 40) & 0xFF);
    tag->data[offset+1] = (uint8_t)((val >> 48) & 0xFF);
    tag->data[offset+0] = (uint8_t)((val >> 56) & 0xFF);

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

    res =   ((uint32_t)(tag->data[offset+3]) << 0 ) +
            ((uint32_t)(tag->data[offset+2]) << 8 ) +
            ((uint32_t)(tag->data[offset+1]) << 16) +
            ((uint32_t)(tag->data[offset+0]) << 24);

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
    tag->data[offset+3] = (uint8_t)((val >> 0 ) & 0xFF);
    tag->data[offset+2] = (uint8_t)((val >> 8 ) & 0xFF);
    tag->data[offset+1] = (uint8_t)((val >> 16) & 0xFF);
    tag->data[offset+0] = (uint8_t)((val >> 24) & 0xFF);

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

    res = (int32_t)(((uint32_t)(tag->data[offset+3]) << 0 ) +
                    ((uint32_t)(tag->data[offset+2]) << 8 ) +
                    ((uint32_t)(tag->data[offset+1]) << 16) +
                    ((uint32_t)(tag->data[offset+0]) << 24));

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

    tag->data[offset+3] = (uint8_t)((val >> 0 ) & 0xFF);
    tag->data[offset+2] = (uint8_t)((val >> 8 ) & 0xFF);
    tag->data[offset+1] = (uint8_t)((val >> 16) & 0xFF);
    tag->data[offset+0] = (uint8_t)((val >> 24) & 0xFF);

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

    res = (uint16_t)((tag->data[offset+1]) +
                    ((tag->data[offset+0]) << 8));

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

    tag->data[offset+1] = (uint8_t)((val >> 0) & 0xFF);
    tag->data[offset+0] = (uint8_t)((val >> 8) & 0xFF);

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

    res = (int16_t)(((tag->data[offset+1])) +
                    ((tag->data[offset+0]) << 8));

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

    tag->data[offset+1] = (uint8_t)((val >> 0) & 0xFF);
    tag->data[offset+0] = (uint8_t)((val >> 8) & 0xFF);

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

    res = tag->data[offset];

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

    tag->data[offset] = val;

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

    res = (int8_t)(tag->data[offset]);

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

    tag->data[offset] = (uint8_t)val;

    return rc;
}



double mb_get_float64(plc_tag_p raw_tag, int offset)
{
    uint64_t ures = 0;
    double res = DBL_MAX;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

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

    ures = ((uint64_t)(tag->data[offset+7]) << 0 ) +
           ((uint64_t)(tag->data[offset+6]) << 8 ) +
           ((uint64_t)(tag->data[offset+5]) << 16) +
           ((uint64_t)(tag->data[offset+4]) << 24) +
           ((uint64_t)(tag->data[offset+3]) << 32) +
           ((uint64_t)(tag->data[offset+2]) << 40) +
           ((uint64_t)(tag->data[offset+1]) << 48) +
           ((uint64_t)(tag->data[offset+0]) << 56);

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

    tag->data[offset+7] = (uint8_t)((val >> 0 ) & 0xFF);
    tag->data[offset+6] = (uint8_t)((val >> 8 ) & 0xFF);
    tag->data[offset+5] = (uint8_t)((val >> 16) & 0xFF);
    tag->data[offset+4] = (uint8_t)((val >> 24) & 0xFF);
    tag->data[offset+3] = (uint8_t)((val >> 32) & 0xFF);
    tag->data[offset+2] = (uint8_t)((val >> 40) & 0xFF);
    tag->data[offset+1] = (uint8_t)((val >> 48) & 0xFF);
    tag->data[offset+0] = (uint8_t)((val >> 56) & 0xFF);

    return rc;
}



float mb_get_float32(plc_tag_p raw_tag, int offset)
{
    uint32_t ures = (uint32_t)0;
    float res = FLT_MAX;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

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

    ures = ((uint32_t)(tag->data[offset+3]) << 0 ) +
           ((uint32_t)(tag->data[offset+2]) << 8 ) +
           ((uint32_t)(tag->data[offset+1]) << 16) +
           ((uint32_t)(tag->data[offset+0]) << 24);

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

    tag->data[offset+3] = (uint8_t)((val >> 0 ) & 0xFF);
    tag->data[offset+2] = (uint8_t)((val >> 8 ) & 0xFF);
    tag->data[offset+1] = (uint8_t)((val >> 16) & 0xFF);
    tag->data[offset+0] = (uint8_t)((val >> 24) & 0xFF);

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

