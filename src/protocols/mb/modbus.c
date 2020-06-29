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

#define PLC_SOCKET_ERR_DELAY (5000)
#define MODBUS_DEFAULT_PORT (502)
#define PLC_READ_DATA_LEN (300)
#define PLC_WRITE_DATA_LEN (300)
#define MODBUS_MBAP_SIZE (6)

struct modbus_plc_t {
    struct modbus_plc_t *next;

    /* keep a list of tags for this PLC. */
    struct modbus_tag_t *tags;

    /* hostname/ip and possibly port of the server. */
    char *server;
    sock_p sock;
    uint8_t server_id;

    /* State */
    struct {
        unsigned int terminate:1;
        unsigned int response_ready:1;
        unsigned int request_ready:1;
    } flags;
    uint16_t seq_id;

    /* thread related state */
    thread_p handler_thread;
    mutex_p mutex;

    /* data buffers. */
    int read_data_len;
    uint8_t read_data[PLC_READ_DATA_LEN];
    int write_data_len;
    int write_data_offset;
    uint8_t write_data[PLC_WRITE_DATA_LEN];
};

typedef struct modbus_plc_t *modbus_plc_p;

typedef enum { MB_REG_UNKNOWN, MB_REG_DO, MB_REG_DI, MB_REG_AO, MB_REG_AI } modbus_reg_type_t;

typedef enum { MB_CMD_READ_DO_MULTI = 0x01,  
               MB_CMD_READ_DI_MULTI = 0x02,  
               MB_CMD_READ_AO_MULTI = 0x03, 
               MB_CMD_READ_AI_MULTI = 0x04,
               MB_CMD_WRITE_DO_SINGLE = 0x05, 
               MB_CMD_WRITE_AO_SINGLE = 0x06,
               MB_CMD_WRITE_DO_MULTI = 0x0F,
               MB_CMD_WRITE_AO_MULTI = 0x10
             } modbug_cmd_t;

struct modbus_tag_t {
    /* base tag parts. */
    TAG_BASE_STRUCT;

    /* next one in the list for this PLC */
    struct modbus_tag_t *next;

    /* register type. */
    modbus_reg_type_t reg_type;
    uint16_t reg_base;

    /* the PLC we are using */
    modbus_plc_p plc;

    /* actions and state */
    struct {
        unsigned int _abort:1;
        unsigned int _read:1;
        unsigned int _write:1;
        unsigned int _busy:1;
    } flags;
    lock_t tag_lock;

    /* data for the tag. */
    int elem_count;
    int elem_size;

    /* data for outstanding requests. */
    uint16_t seq_id;
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
static int connect_plc(modbus_plc_p plc);
static int read_packet(modbus_plc_p plc);
static int write_packet(modbus_plc_p plc);
static int process_tag(modbus_tag_p tag, modbus_plc_p plc);
static int check_read_response(modbus_plc_p plc, modbus_tag_p tag);
static int create_read_request(modbus_plc_p plc, modbus_tag_p tag);
static int check_write_response(modbus_plc_p plc, modbus_tag_p tag);
static int create_write_request(modbus_plc_p plc, modbus_tag_p tag);
static int translate_modbus_error(uint8_t err_code);

static int tag_get_abort_flag(modbus_tag_p tag);
static int tag_set_abort_flag(modbus_tag_p tag, int new_val);

static int tag_get_read_flag(modbus_tag_p tag);
static int tag_set_read_flag(modbus_tag_p tag, int new_val);

static int tag_get_write_flag(modbus_tag_p tag);
static int tag_set_write_flag(modbus_tag_p tag, int new_val);

static int tag_get_busy_flag(modbus_tag_p tag);
static int tag_set_busy_flag(modbus_tag_p tag, int new_val);

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
    int reg_base = attr_get_int(attribs, "reg_base", -1);

    pdebug(DEBUG_INFO, "Starting.");

    *tag = NULL;

    if(reg_base < 0) {
        pdebug(DEBUG_WARN, "Register base is missing or negative!");
        return PLCTAG_ERR_BAD_PARAM;
    }

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
    (*tag)->reg_base = (uint16_t)(unsigned int)reg_base;
    (*tag)->reg_type = reg_type;
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
    int server_id = attr_get_int(attribs, "server_id", -1);
    int is_new = 0;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    if(server_id < 0 || server_id > 255) {
        pdebug(DEBUG_WARN, "Server ID out of bounds or missing!");
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    /* see if we can find a matching server. */
    critical_block(mb_mutex) {
        modbus_plc_p *walker = &plcs;

        while(*walker && (*walker)->server_id == (uint8_t)(unsigned int)server_id && str_cmp_i(server, (*walker)->server) != 0) {
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
                    (*plc)->server_id = (uint8_t)(unsigned int)server_id;
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
    int rc = PLCTAG_STATUS_OK;
    modbus_plc_p plc = (modbus_plc_p)arg;
    int64_t err_delay = 0;

    pdebug(DEBUG_INFO, "Starting.");

    if(!plc) {
        pdebug(DEBUG_WARN, "Null PLC pointer passed!");
        return NULL;
    }

    while(! plc->flags.terminate) {
        int keep_going = 0;

        if(err_delay < time_ms()) {
            do {
                if(! plc->sock) {
                    /* socket must not be open! */
                    rc = connect_plc(plc);
                    if(rc != PLCTAG_STATUS_OK) {
                        err_delay = time_ms() + PLC_SOCKET_ERR_DELAY;
                        break;
                    }
                }

                /* read packet */
                rc = read_packet(plc);
                if(rc != PLCTAG_STATUS_OK) {
                    /* problem, punt! */
                    err_delay = time_ms() + PLC_SOCKET_ERR_DELAY;
                    break;
                }

                if(plc->flags.response_ready) {
                    keep_going = 1;
                }

                /* write packet */
                rc = write_packet(plc);
                if(rc != PLCTAG_STATUS_OK) {
                    /* oops! */
                    err_delay = time_ms() + PLC_SOCKET_ERR_DELAY;
                    break;
                }

                /* run all the tags. */
                critical_block(plc->mutex) {
                    /* don't do anything if there are no tags. */
                    if(plc->tags) {
                        modbus_tag_p *tag_walker = &(plc->tags);

                        while(*tag_walker) {
                            modbus_tag_p tag = rc_inc(*tag_walker);

                            /* the tag might be in the destructor. */
                            if(tag) {
                                pdebug(DEBUG_DETAIL, "Processing tag %d.", tag->tag_id);

                                rc = process_tag(tag, plc);
                                if(rc != PLCTAG_STATUS_OK) {
                                    pdebug(DEBUG_WARN,  "Error, %s, processing tag %d!", plc_tag_decode_error(rc), tag->tag_id);
                                }

                                /* release reference. */
                                tag = rc_dec(tag);
                            }

                            tag_walker = &((*tag_walker)->next);
                        }
                    }
                }
            } while(0);

            if(plc->flags.response_ready) {
                pdebug(DEBUG_WARN, "Response still pending after full tag pass.  Clearing buffer.");

                plc->flags.response_ready = 0;
                plc->read_data_len = 0;
            }

            if(rc != PLCTAG_STATUS_OK) {

            }
        } else {
            keep_going = 0;
        }

        if(!keep_going) {
            sleep_ms(1);
        }
    }

    pdebug(DEBUG_INFO, "Done.");

    return NULL;
}



int connect_plc(modbus_plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    char **server_port = NULL;
    char *server = NULL;
    int port = MODBUS_DEFAULT_PORT;

    pdebug(DEBUG_DETAIL, "Starting.");

    server_port = str_split(plc->server, ":");
    if(!server_port) {
        pdebug(DEBUG_WARN, "Unable to split server and port string!");
        return PLCTAG_ERR_BAD_CONFIG;
    }

    if(server_port[0] == NULL) {
        pdebug(DEBUG_WARN, "Server string is malformed or empty!");
        return PLCTAG_ERR_BAD_CONFIG;
    } else {
        server = server_port[0];
    }

    if(server_port[1] != NULL) {
        rc = str_to_int(server_port[1], &port);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to extract port number from server string \"%s\"!", plc->server);
            return PLCTAG_ERR_BAD_CONFIG;
        }
    } else {
        port = MODBUS_DEFAULT_PORT;
    }

    pdebug(DEBUG_DETAIL, "Using server \"%s\" and port %d.", server, port);

    rc = socket_create(&(plc->sock));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create socket object, error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    /* connect to the socket */
    pdebug(DEBUG_DETAIL, "Connecting to %s on port %d...", server, port);
    rc = socket_connect_tcp(plc->sock, server, port);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to connect to the server \"%s\", got error %s!", plc->server, plc_tag_decode_error(rc));
        socket_destroy(&(plc->sock));
        return rc;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}



int read_packet(modbus_plc_p plc)
{
    int rc = 1;
    int data_needed = 1;

    pdebug(DEBUG_SPEW, "Starting.");

    /* Loop until we stop getting data or we have all the data. */
    while(rc > 0 && data_needed > 0) {
        if(plc->read_data_len >= MODBUS_MBAP_SIZE) {
            int packet_size = plc->read_data[5] + (plc->read_data[4] << 8);
            data_needed = (MODBUS_MBAP_SIZE + packet_size) - plc->read_data_len;

            pdebug(DEBUG_DETAIL, "data_needed=%d, packet_size=%d, read_data_len=%d", data_needed, packet_size, plc->read_data_len);

            if(data_needed > PLC_READ_DATA_LEN) {
                pdebug(DEBUG_WARN, "Packet size, %d, greater than buffer size, %d!", data_needed, PLC_READ_DATA_LEN);
                rc = PLCTAG_ERR_TOO_LARGE;
            }
        } else {
            data_needed = MODBUS_MBAP_SIZE - plc->read_data_len;            
        }

        rc = socket_read(plc->sock, plc->read_data + plc->read_data_len, data_needed);
        if(rc >= 0) {
            /* got data! Or got nothing, but no error. */
            plc->read_data_len += rc;

            pdebug_dump_bytes(DEBUG_DETAIL, plc->read_data, plc->read_data_len);
        } else {
            pdebug(DEBUG_WARN, "Error, %s, reading socket!", plc_tag_decode_error(rc));
            break;
        }
    } while(rc > 0 && data_needed > 0);

    if(rc >= 0) {
        if(data_needed == 0) {
            /* we got our packet. */
            pdebug(DEBUG_DETAIL, "Received full packet.");
            pdebug_dump_bytes(DEBUG_DETAIL, plc->read_data, plc->read_data_len);
            plc->flags.response_ready = 1;
        }

        rc = PLCTAG_STATUS_OK;
    } 

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}



int write_packet(modbus_plc_p plc)
{
    int rc = 1;
    int data_left = plc->write_data_len - plc->write_data_offset;

    pdebug(DEBUG_SPEW, "Starting.");

    if(! plc->flags.request_ready) {
        pdebug(DEBUG_SPEW, "Done. Nothing to do.");
        return PLCTAG_STATUS_OK;
    }

    while(rc > 0 && data_left > 0) {
        rc = socket_write(plc->sock, plc->write_data + plc->write_data_offset, data_left);
        if(rc >= 0) {
            plc->write_data_offset += rc;
            data_left = plc->write_data_len - plc->write_data_offset;
        } else {
            pdebug(DEBUG_WARN, "Error, %s, writing to socket!", plc_tag_decode_error(rc));
        }
    }

    if(rc >= 0) {
        /* clean up if full write was done. */
        if(data_left == 0) {
            pdebug(DEBUG_DETAIL, "Full packet written.");
            pdebug_dump_bytes(DEBUG_DETAIL, plc->write_data, plc->write_data_len);

            plc->flags.request_ready = 0;
            plc->write_data_len = 0;
            plc->write_data_offset = 0;
        }

        rc = PLCTAG_STATUS_OK;
    }

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}

/*
 * This is called in the context of the PLC thread.
 * 
 * The process cannot lock the tag's API mutex.   If the tag user side is
 * blocked in plc_tag_read, plc_tag_write or any other possibly blocking
 * API call, then the API mutex will be help.
 * 
 * However, we can ensure that the tag will not be deleted out from underneath
 * us because this function is called under the PLC's mutex.   plc_tag_delete()
 * will first call the destructor and try to remove the tag from the PLC's list
 * and that requires the PLC's mutex to be held.   So as long as we are
 * under the PLC's mutex, we cannot have the tag disappear out from underneath
 * us or out of the list.
 * 
 * To help ensure that, we take a reference to the tag as well.  If the tag was
 * in the process of being destroyed, the reference returned will be null.
 * 
 * So if we are here, we are not going to have the tag disappear nor are we going to 
 * have the PLC's tag list mutated.
 */

int process_tag(modbus_tag_p tag, modbus_plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* try to get the mutex on the tag. */
    // if(mutex_try_lock(tag->api_mutex) == PLCTAG_STATUS_OK) {
        /* got the mutex, so we can look at and modify the tag. */
        if(tag_get_abort_flag(tag)) {
            pdebug(DEBUG_DETAIL, "Aborting any in flight operations!");

            /* do this as one block to prevent half-changed state. */
            spin_block(&tag->tag_lock) {
                tag->flags._read = 0;
                tag->flags._write = 0;
                tag->flags._busy = 0;
                tag->flags._abort = 0;
            }

            tag->seq_id = 0;
        }

        if(tag_get_write_flag(tag)) {
            if(tag_get_busy_flag(tag)) {
                if(plc->flags.response_ready) {
                    /* we have an outstanding write request. */
                    rc = check_write_response(plc, tag);
                    if(rc == PLCTAG_STATUS_OK) {
                        pdebug(DEBUG_SPEW, "Either not our response or we got a response!");
                    } else {
                        pdebug(DEBUG_SPEW, "We got an error on our write response check, %s!", plc_tag_decode_error(rc));
                    }
                } else {
                    pdebug(DEBUG_SPEW, "No response yet.");
                }
            } else {
                /* we have a write request but it is not in flight. */
                if(! plc->flags.request_ready) {
                    rc = create_write_request(plc, tag);
                } else {
                    pdebug(DEBUG_SPEW, "No buffer space for a response.");
                }
            }
        }

        if(tag_get_read_flag(tag)) {
            if(tag_get_busy_flag(tag)) {
                if(plc->flags.response_ready) {
                    /* there is a request in flight, is there a response? */
                    rc = check_read_response(plc, tag);
                    if(rc == PLCTAG_STATUS_OK) {
                        /* this is our response. */
                        pdebug(DEBUG_SPEW, "Either not our response or we got a good response.");
                    } else {
                        pdebug(DEBUG_SPEW, "We got an error on our read response check, %s!", plc_tag_decode_error(rc));
                    }
                } else {
                    pdebug(DEBUG_SPEW, "No response yet.");
                }
            } else {
                /* we have a write request but it is not in flight. */
                if(! plc->flags.request_ready) {
                    rc = create_read_request(plc, tag);
                } else {
                    pdebug(DEBUG_SPEW, "No buffer space for a response.");
                }
            }
        }

        // mutex_unlock(tag->api_mutex);
    // }

    /* set the tag status. */
    tag->status = rc;

    /* does this tag need to do anything? */

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int check_read_response(modbus_plc_p plc, modbus_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    uint16_t seq_id = (uint16_t)((uint16_t)plc->read_data[1] +(uint16_t)(plc->read_data[0] << 8));

    pdebug(DEBUG_DETAIL, "Starting.");

    if(seq_id == tag->seq_id) {
        uint8_t has_error = plc->read_data[7] & (uint8_t)0x80;

        if(has_error) {
            rc = translate_modbus_error(plc->read_data[8]);

            pdebug(DEBUG_WARN, "Got read response %ud, with error %s, of length %d.", (int)(unsigned int)seq_id, plc_tag_decode_error(rc), plc->read_data_len);
        } else {
            int payload_size = (int)(unsigned int)plc->read_data[8];
            int copy_size = (tag->size < payload_size ? tag->size : payload_size);

            /* FIXME - handle tags longer than packet size. */

            /* no error. So copy the data. */
            pdebug(DEBUG_DETAIL, "Got read response %ud of length %d with payload of size %d.", (int)(unsigned int)seq_id, plc->read_data_len, payload_size);

            mem_copy(tag->data, &plc->read_data[9], copy_size);

            rc = PLCTAG_STATUS_OK;
        }

        /* either way, clean up the PLC buffer. */
        plc->read_data_len = 0;
        plc->flags.response_ready = 0;

        /* clean up tag*/
        tag_set_read_flag(tag, 0);
        tag_set_busy_flag(tag, 0);
        tag->seq_id = 0;
    } else {
        pdebug(DEBUG_DETAIL, "Not our response.");

        rc = PLCTAG_STATUS_OK;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int create_read_request(modbus_plc_p plc, modbus_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    uint16_t seq_id = (++(plc->seq_id) ? plc->seq_id : ++(plc->seq_id)); // disallow zero

    pdebug(DEBUG_DETAIL, "Starting.");

    /* build the read request.
     *    Byte  Meaning
     *      0    High byte of request sequence ID.
     *      1    Low byte of request sequence ID.
     *      2    High byte of the protocol version identifier (zero).
     *      3    Low byte of the protocol version identifier (zero).
     *      4    High byte of the message length.
     *      5    Low byte of the message length.
     *      6    Device address.
     *      7    Function code.
     *      8    High byte of first register address.
     *      9    Low byte of the first register address.
     *     10    High byte of the register count.
     *     11    Low byte of the register count.
     */

    plc->write_data_len = 0;

    /* build the request sequence ID */
    plc->write_data[plc->write_data_len] = (uint8_t)((seq_id >> 8) & 0xFF); plc->write_data_len++;
    plc->write_data[plc->write_data_len] = (uint8_t)((seq_id >> 0) & 0xFF); plc->write_data_len++;

    /* protocol version is always zero */
    plc->write_data[plc->write_data_len] = 0; plc->write_data_len++;
    plc->write_data[plc->write_data_len] = 0; plc->write_data_len++;

    /* request packet length */
    plc->write_data[plc->write_data_len] = 0; plc->write_data_len++;
    plc->write_data[plc->write_data_len] = 6; plc->write_data_len++;

    /* device address */
    plc->write_data[plc->write_data_len] = plc->server_id; plc->write_data_len++;

    /* function code depends on the register type. */
    switch(tag->reg_type) {
        case MB_REG_DO:
            plc->write_data[7] = MB_CMD_READ_DO_MULTI; plc->write_data_len++;
            break;

        case MB_REG_DI:
            plc->write_data[7] = MB_CMD_READ_DI_MULTI; plc->write_data_len++;
            break;

        case MB_REG_AO:
            plc->write_data[7] = MB_CMD_READ_AO_MULTI; plc->write_data_len++;
            break;

        case MB_REG_AI:
            plc->write_data[7] = MB_CMD_READ_AI_MULTI; plc->write_data_len++;
            break;

        default:
            pdebug(DEBUG_WARN, "Unsupported register type %d!", tag->reg_type);
            return PLCTAG_ERR_UNSUPPORTED;
            break;
    }

    /* register base. */
    plc->write_data[8] = (uint8_t)((tag->reg_base >> 8) & 0xFF); plc->write_data_len++;
    plc->write_data[9] = (uint8_t)((tag->reg_base >> 0) & 0xFF); plc->write_data_len++;

    /* number of elements to read. */
    plc->write_data[10] = (uint8_t)((tag->elem_count >> 8) & 0xFF); plc->write_data_len++;
    plc->write_data[11] = (uint8_t)((tag->elem_count >> 0) & 0xFF); plc->write_data_len++;

    /* ready to go. */

    plc->flags.request_ready = 1;
    tag_set_busy_flag(tag, 1);
    tag->seq_id = seq_id;

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int check_write_response(modbus_plc_p plc, modbus_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    uint16_t seq_id = (uint16_t)((uint16_t)plc->read_data[1] + (uint16_t)(plc->read_data[0] << 8));

    pdebug(DEBUG_SPEW, "Starting.");

    if(seq_id == tag->seq_id) {
        uint8_t has_error = plc->read_data[7] & (uint8_t)0x80;

        if(has_error) {
            rc = translate_modbus_error(plc->read_data[8]);

            pdebug(DEBUG_WARN, "Got write response %ud, with error %s, of length %d.", (int)(unsigned int)seq_id, plc_tag_decode_error(rc), plc->read_data_len);
        } else {
            /* FIXME - handle tags longer than packet size. */

            /* no error. */
            pdebug(DEBUG_DETAIL, "Got write response %ud of length %d.", (int)(unsigned int)seq_id, plc->read_data_len);

            rc = PLCTAG_STATUS_OK;
        }

        /* either way, clean up the PLC buffer. */
        plc->read_data_len = 0;
        plc->flags.response_ready = 0;

        /* clean up tag*/
        tag_set_write_flag(tag, 0);
        tag_set_busy_flag(tag, 0);
        tag->seq_id = 0;
    } else {
        pdebug(DEBUG_SPEW, "Not our response.");

        rc = PLCTAG_STATUS_OK;
    }

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}


int create_write_request(modbus_plc_p plc, modbus_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    uint16_t seq_id = (++(plc->seq_id) ? plc->seq_id : ++(plc->seq_id)); // disallow zero

    pdebug(DEBUG_DETAIL, "Starting.");

    rc = PLCTAG_ERR_UNSUPPORTED;

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}



int translate_modbus_error(uint8_t err_code)
{
    int rc = PLCTAG_STATUS_OK;

    switch(err_code) {
        case 0x01:
            pdebug(DEBUG_WARN, "The received function code can not be processed!");
            rc = PLCTAG_ERR_UNSUPPORTED;
            break;

        case 0x02:
            pdebug(DEBUG_WARN, "The data address specified in the request is not available!");
            rc = PLCTAG_ERR_NOT_FOUND;
            break;

        case 0x03:
            pdebug(DEBUG_WARN, "The value contained in the query data field is an invalid value!");
            rc = PLCTAG_ERR_BAD_PARAM;
            break;

        case 0x04:
            pdebug(DEBUG_WARN, "An unrecoverable error occurred while the server attempted to perform the requested action!");
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;

        case 0x05:
            pdebug(DEBUG_WARN, "The server will take a long time processing this request!");
            rc = PLCTAG_ERR_PARTIAL;
            break;

        case 0x06:
            pdebug(DEBUG_WARN, "The server is busy!");
            rc = PLCTAG_ERR_BUSY;
            break;

        case 0x07:
            pdebug(DEBUG_WARN, "The server can not execute the program function specified in the request!");
            rc = PLCTAG_ERR_UNSUPPORTED;
            break;

        case 0x08:
            pdebug(DEBUG_WARN, "The slave detected a parity error when reading the extended memory!");
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;

        default:
            pdebug(DEBUG_WARN, "Unknown error response %u received!", (int)(unsigned int)(err_code));
            rc = PLCTAG_ERR_UNSUPPORTED;
            break;
    }

    return rc;
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
    } else {
        pdebug(DEBUG_WARN, "Unsupported register type %s.", reg_type);
        res = MB_REG_UNKNOWN;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return res;
}




int tag_get_abort_flag(modbus_tag_p tag)
{
    int res = 0;

    spin_block(&tag->tag_lock) {
        res = tag->flags._abort;
    }

    return res;
}

int tag_set_abort_flag(modbus_tag_p tag, int new_val)
{
    int old_val = 0;

    spin_block(&tag->tag_lock) {
        old_val = tag->flags._abort;
        tag->flags._abort = ((new_val) ? 1 : 0);
    }

    return old_val;
}


int tag_get_read_flag(modbus_tag_p tag)
{
    int res = 0;

    spin_block(&tag->tag_lock) {
        res = tag->flags._read;
    }

    return res;
}

int tag_set_read_flag(modbus_tag_p tag, int new_val)
{
    int old_val = 0;

    spin_block(&tag->tag_lock) {
        old_val = tag->flags._read;
        tag->flags._read = ((new_val) ? 1 : 0);
    }

    return old_val;
}



int tag_get_write_flag(modbus_tag_p tag)
{
    int res = 0;

    spin_block(&tag->tag_lock) {
        res = tag->flags._write;
    }

    return res;
}

int tag_set_write_flag(modbus_tag_p tag, int new_val)
{
    int old_val = 0;

    spin_block(&tag->tag_lock) {
        old_val = tag->flags._write;
        tag->flags._write = ((new_val) ? 1 : 0);
    }

    return old_val;
}



int tag_get_busy_flag(modbus_tag_p tag)
{
    int res = 0;

    spin_block(&tag->tag_lock) {
        res = tag->flags._busy;
    }

    return res;
}

int tag_set_busy_flag(modbus_tag_p tag, int new_val)
{
    int old_val = 0;

    spin_block(&tag->tag_lock) {
        old_val = tag->flags._busy;
        tag->flags._busy = ((new_val) ? 1 : 0);
    }

    return old_val;
}





/****** Tag Control Functions ******/

int mb_abort(plc_tag_p p_tag)
{
    modbus_tag_p tag = (modbus_tag_p)p_tag;

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    tag_set_abort_flag(tag, 1);

    return PLCTAG_STATUS_OK;
}



int mb_read_start(plc_tag_p p_tag)
{
    modbus_tag_p tag = (modbus_tag_p)p_tag;
    int op_in_flight = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    spin_block(&tag->tag_lock) {
        if(tag->flags._abort || tag->flags._read || tag->flags._write) {
            op_in_flight = 1;
        } else {
            op_in_flight = 0;
        }
    }

    if(op_in_flight) {
        pdebug(DEBUG_WARN, "Operation in progress!");
        return PLCTAG_ERR_BUSY;
    }

    tag_set_read_flag(tag, 1);

    tag->status = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_PENDING;
}



int mb_tag_status(plc_tag_p p_tag)
{
    modbus_tag_p tag = (modbus_tag_p)p_tag;
    int op_in_flight = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(tag->status != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_SPEW, "Status not OK, returning %s.", plc_tag_decode_error(tag->status));
        return tag->status;
    }

    spin_block(&tag->tag_lock) {
        if(tag->flags._abort || tag->flags._read || tag->flags._write) {
            op_in_flight = 1;
        } else {
            op_in_flight = 0;
        }
    }

    if(op_in_flight) {
        pdebug(DEBUG_SPEW, "Operation in progress, returning PLCTAG_STATUS_PENDING.");
        return PLCTAG_STATUS_PENDING;
    }

    pdebug(DEBUG_SPEW, "Done.");

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
    int op_in_flight = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    spin_block(&tag->tag_lock) {
        if(tag->flags._abort || tag->flags._read || tag->flags._write) {
            op_in_flight = 1;
        } else {
            op_in_flight = 0;
        }
    }

    if(op_in_flight) {
        pdebug(DEBUG_WARN, "Operation in progress!");
        return PLCTAG_ERR_BUSY;
    }

    tag_set_write_flag(tag, 1);
    tag->status = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_PENDING;
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

