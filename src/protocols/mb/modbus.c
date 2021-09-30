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
#define MAX_MODBUS_REQUEST_PAYLOAD (246)
#define MAX_MODBUS_RESPONSE_PAYLOAD (250)
#define MAX_MODBUS_PDU_PAYLOAD (253)  /* everything after the server address */
#define MODBUS_INACTIVITY_TIMEOUT (5000)
#define SOCKET_READ_TIMEOUT (20) /* read timeout in milliseconds */
#define SOCKET_WRITE_TIMEOUT (20) /* write timeout in milliseconds */
#define MODBUS_IDLE_WAIT_TIMEOUT (100) /* idle wait timeout in milliseconds */


struct modbus_plc_t {
    struct modbus_plc_t *next;

    /* keep a list of tags for this PLC. */
    struct modbus_tag_t *tags_head;
    struct modbus_tag_t *tags_tail;

    /* hostname/ip and possibly port of the server. */
    char *server;
    sock_p sock;
    uint8_t server_id;

    /* State */
    struct {
        unsigned int terminate:1;
        unsigned int response_ready:1;
        unsigned int request_ready:1;
        unsigned int request_in_flight:1;
    } flags;
    uint16_t seq_id;

    /* thread related state */
    thread_p handler_thread;
    mutex_p mutex;
    cond_p wait_cond;
    enum {
        PLC_START = 0,
        PLC_IDLE,
        PLC_SEND_REQUEST,
        PLC_RECEIVE_RESPONSE,
        PLC_ERR_WAIT
    } state;

    /* comms timeout/disconnect. */
    int64_t inactivity_timeout_ms;

    /* data */
    int read_data_len;
    uint8_t read_data[PLC_READ_DATA_LEN];
    int32_t response_tag_id;

    int write_data_len;
    int write_data_offset;
    uint8_t write_data[PLC_WRITE_DATA_LEN];
    int32_t request_tag_id;
};

typedef struct modbus_plc_t *modbus_plc_p;

typedef enum { MB_REG_UNKNOWN, MB_REG_COIL, MB_REG_DISCRETE_INPUT, MB_REG_HOLDING_REGISTER, MB_REG_INPUT_REGISTER } modbus_reg_type_t;

typedef enum { MB_CMD_READ_COIL_MULTI = 0x01,
               MB_CMD_READ_DISCRETE_INPUT_MULTI = 0x02,
               MB_CMD_READ_HOLDING_REGISTER_MULTI = 0x03,
               MB_CMD_READ_INPUT_REGISTER_MULTI = 0x04,
               MB_CMD_WRITE_COIL_SINGLE = 0x05,
               MB_CMD_WRITE_HOLDING_REGISTER_SINGLE = 0x06,
               MB_CMD_WRITE_COIL_MULTI = 0x0F,
               MB_CMD_WRITE_HOLDING_REGISTER_MULTI = 0x10
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
        unsigned int operation_complete:1;
    } flags;
    uint16_t request_num;
    uint16_t seq_id;
    lock_t tag_lock;

    /* data for the tag. */
    int elem_count;
    int elem_size;

    /* data for outstanding requests. */
};

typedef struct modbus_tag_t *modbus_tag_p;


/* default string types used for Modbus PLCs. */
tag_byte_order_t modbus_tag_byte_order = {
    .is_allocated = 0,

    .int16_order = {1,0},
    .int32_order = {3,2,1,0},
    .int64_order = {7,6,5,4,3,2,1,0},
    .float32_order = {3,2,1,0},
    .float64_order = {7,6,5,4,3,2,1,0},

    .str_is_defined = 0, /* FIXME */
    .str_is_counted = 0,
    .str_is_fixed_length = 0,
    .str_is_zero_terminated = 0,
    .str_is_byte_swapped = 0,

    .str_count_word_bytes = 0,
    .str_max_capacity = 0,
    .str_total_length = 0,
    .str_pad_bytes = 0
};


/* Modbus module globals. */
mutex_p mb_mutex = NULL;
modbus_plc_p plcs = NULL;
volatile int library_terminating = 0;


/* helper functions */
static int create_tag_object(attr attribs, modbus_tag_p *tag);
// static int set_tag_byte_order(attr attribs, modbus_tag_p tag);
// static int check_byte_order_str(const char *byte_order, int length);
static int find_or_create_plc(attr attribs, modbus_plc_p *plc);
static int parse_register_name(attr attribs, modbus_reg_type_t *reg_type, int *reg_base);
static void modbus_tag_destructor(void *tag_arg);
static void modbus_plc_destructor(void *plc_arg);
static THREAD_FUNC(modbus_plc_handler);
static int connect_plc(modbus_plc_p plc);
static int read_packet(modbus_plc_p plc);
static int write_packet(modbus_plc_p plc);
static int run_tags(modbus_plc_p plc);
static int process_tag_unsafe(modbus_tag_p tag, modbus_plc_p plc);
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

struct tag_vtable_t modbus_vtable = {
    (tag_vtable_func)mb_abort,
    (tag_vtable_func)mb_read_start,
    (tag_vtable_func)mb_tag_status, /* shared */
    (tag_vtable_func)mb_tickler,
    (tag_vtable_func)mb_write_start,

    /* data accessors */
    mb_get_int_attrib,
    mb_set_int_attrib
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
            tag->next = NULL;

            if(tag->plc->tags_head) {
                pdebug(DEBUG_DETAIL, "Inserting tag %p at the end (%p) of the list (%p).", tag, tag->plc->tags_tail, tag->plc->tags_head);
                tag->plc->tags_tail->next = tag;
                tag->plc->tags_tail = tag;
            } else {
                pdebug(DEBUG_DETAIL, "Inserting tag %p in empty list.", tag);
                tag->plc->tags_head = tag;
                tag->plc->tags_tail = tag;
            }
        }

        /* trigger a read to get the initial value of the tag. */
        // FIXME - trigger read futher up the chain after plc_tag_create is mostly done.  This is unsafe because
        //    there is setup such as mutex creation done after this routine.
        //tag->read_in_flight = 1;
        //tag->flags._read = 1;
        //cond_signal(tag->plc->wait_cond);
    } else {
        pdebug(DEBUG_WARN, "Unable to create new tag!  Error %s!", plc_tag_decode_error(rc));
        tag->status = (int8_t)rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return (plc_tag_p)tag;
}




/***** helper functions *****/

int create_tag_object(attr attribs, modbus_tag_p *tag)
{
    int rc = PLCTAG_STATUS_OK;
    int data_size = 0;
    int reg_size = 0;
    int elem_count = attr_get_int(attribs, "elem_count", 1);
    modbus_reg_type_t reg_type = MB_REG_UNKNOWN;
    int reg_base = 0;

    pdebug(DEBUG_INFO, "Starting.");

    *tag = NULL;

    /* get register type. */
    rc = parse_register_name(attribs, &reg_type, &reg_base);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error parsing base register name!");
        return rc;
    }

    /* determine register type. */
    switch(reg_type) {
        case MB_REG_COIL:
            /* fall through */
        case MB_REG_DISCRETE_INPUT:
            reg_size = 1;
            break;

        case MB_REG_HOLDING_REGISTER:
            /* fall through */
        case MB_REG_INPUT_REGISTER:
            reg_size = 16;
            break;

        default:
            pdebug(DEBUG_WARN, "Unsupported register type!");
            reg_size = 0;
            return PLCTAG_ERR_BAD_PARAM;
            break;
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

    /* set the default byte order */
    (*tag)->byte_order = &modbus_tag_byte_order;

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

    /* abort everything. */
    mb_abort((plc_tag_p)tag);

    if(tag->plc) {
        /* unlink the tag from the PLC. */
        critical_block(tag->plc->mutex) {
            modbus_tag_p current = tag->plc->tags_head;
            modbus_tag_p prev = NULL;

            pdebug(DEBUG_DETAIL, "Unlinking from the PLC.");

            while(current && current != tag) {
                prev = current;
                current = current->next;
            }

            /* if found */
            if(current == tag) {
                pdebug(DEBUG_DETAIL, "Found tag %d in PLC list.", tag->tag_id);

                /* unlink the tag in the common case it isn't at the front of the list. */
                if(prev) {
                    prev->next = tag->next;
                } else {
                    tag->plc->tags_head = tag->next;
                }

                /* handle the case where the tag is at the end of the list. */
                if(tag->plc->tags_tail == tag) {
                    tag->plc->tags_tail = prev;
                }
            } else {
                pdebug(DEBUG_DETAIL, "Tag %d not found on PLC list.", tag->tag_id);
            }
        }

        pdebug(DEBUG_DETAIL, "Releasing the reference to the PLC.");
        tag->plc = rc_dec(tag->plc);
    }

    if(tag->api_mutex) {
        mutex_destroy(&(tag->api_mutex));
        tag->api_mutex = NULL;
    }

    if(tag->ext_mutex) {
        mutex_destroy(&(tag->ext_mutex));
        tag->ext_mutex = NULL;
    }

    if(tag->byte_order && tag->byte_order->is_allocated) {
        mem_free(tag->byte_order);
        tag->byte_order = NULL;
    }

    pdebug(DEBUG_INFO, "Done.");
}




int find_or_create_plc(attr attribs, modbus_plc_p *plc)
{
    const char *server = attr_get_str(attribs, "gateway", NULL);
    int server_id = attr_get_int(attribs, "path", -1);
    int is_new = 0;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    if(server_id < 0 || server_id > 255) {
        pdebug(DEBUG_WARN, "Server ID, %d, out of bounds or missing!", server_id);
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
            *plc = rc_inc(*walker);
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

            do {
                /* we want to stay connected initially */
                (*plc)->inactivity_timeout_ms = MODBUS_INACTIVITY_TIMEOUT + time_ms();

                /* set up the PLC state */
                (*plc)->state = PLC_START;

                /* make the wait condition variable. */
                rc = cond_create(&(*plc)->wait_cond);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Unable to create wait condition variable!");
                    break;
                }

                rc = thread_create(&((*plc)->handler_thread), modbus_plc_handler, 32768, (void *)(*plc));
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Unable to create new handler thread, error %s!", plc_tag_decode_error(rc));
                    break;
                }

                pdebug(DEBUG_DETAIL, "Created thread %p.", (*plc)->handler_thread);

                rc = mutex_create(&((*plc)->mutex));
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Unable to create new mutex, error %s!", plc_tag_decode_error(rc));
                    break;
                }
            } while(0);
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
        pdebug(DEBUG_DETAIL, "Terminating Modbus handler thread %p.", plc->handler_thread);

        /* set the flag to cause the thread to terminate. */
        plc->flags.terminate = 1;

        /* signal the condition var to free the thread. */
        cond_signal(plc->wait_cond);

        /* wait for the thread to terminate and destroy it. */
        thread_join(plc->handler_thread);
        thread_destroy(&plc->handler_thread);

        plc->handler_thread = NULL;
    }

    if(plc->mutex) {
        mutex_destroy(&plc->mutex);
        plc->mutex = NULL;
    }

    if(plc->wait_cond) {
        cond_destroy(&(plc->wait_cond));
        plc->wait_cond = NULL;
    }

    if(plc->sock) {
        socket_destroy(&plc->sock);
        plc->sock = NULL;
    }

    if(plc->server) {
        mem_free(plc->server);
        plc->server = NULL;
    }

    if(plc->tags_head) {
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
        THREAD_RETURN(0);
    }

    while(! plc->flags.terminate) {
        switch(plc->state) {
        case PLC_START:
            pdebug(DEBUG_DETAIL, "in PLC_START state.");

            /* connect to the PLC */
            rc = connect_plc(plc);
            if(rc == PLCTAG_STATUS_OK) {
                pdebug(DEBUG_DETAIL," Successfully connected to the PLC!");
                plc->state = PLC_IDLE;

                /* immediately check if there is something to do. */
                cond_signal(plc->wait_cond);
            } else {
                pdebug(DEBUG_WARN, "Unable to connect to the PLC, will retry later!");
                err_delay = time_ms() + PLC_SOCKET_ERR_DELAY;
                plc->state = PLC_ERR_WAIT;
            }
            break;

        case PLC_IDLE:
            pdebug(DEBUG_DETAIL, "in PLC_IDLE state.");

            /* look for something to do. */

            /* run all the tags. */
            rc = run_tags(plc);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_DETAIL, "Error %s running tag handlers!", plc_tag_decode_error(rc));
                /* FIXME */
                break;
            }

            if(plc->flags.response_ready) {
                pdebug(DEBUG_DETAIL, "Request left in buffer, tag must have aborted.");

                /* clear the flags for the response. */
                plc->flags.response_ready = 0;
                plc->read_data_len = 0;
            }

            /* do we have a request ready? */
            if(plc->flags.request_ready) {
                pdebug(DEBUG_DETAIL, "Packet ready to send.");
                plc->state = PLC_SEND_REQUEST;
                cond_signal(plc->wait_cond);
            }

            break;

        case PLC_SEND_REQUEST:
            pdebug(DEBUG_DETAIL, "in PLC_SEND_REQUEST state.");

            debug_set_tag_id((int)plc->request_tag_id);

            rc = write_packet(plc);
            if(rc == PLCTAG_STATUS_OK) {
                /* if we sent a packet, we need to wait for a response. */
                if(plc->flags.request_ready == 0) {
                    pdebug(DEBUG_DETAIL, "Request sent, now wait for response.");

                    plc->state = PLC_RECEIVE_RESPONSE;
                }

                /* else we stay in this state and keep trying to write. */
            } else {
                pdebug(DEBUG_WARN, "Closing socket due to write error %s.", plc_tag_decode_error(rc));

                socket_close(plc->sock);
                socket_destroy(&(plc->sock));
                plc->sock = NULL;

                /* set up the state. */
                plc->flags.request_ready = 0;
                plc->write_data_len = 0;
                plc->write_data_offset = 0;

                /* try to reconnect immediately. */
                plc->state = PLC_START;
            }


            /* if we did not send all the packet, we stay in this state and keep trying. */
            cond_signal(plc->wait_cond);

            debug_set_tag_id(0);

            break;


        case PLC_RECEIVE_RESPONSE:
            pdebug(DEBUG_DETAIL, "in PLC_RECEIVE_RESPONSE state.");

            debug_set_tag_id((int)plc->response_tag_id);

            /* get a packet */
            rc = read_packet(plc);
            if(rc == PLCTAG_STATUS_OK) {
                /* did we read a packet? */
                if(plc->flags.response_ready) {
                    pdebug(DEBUG_DETAIL, "Response ready.");

                    /* go process the response and possibly have another tag write a request. */
                    plc->state = PLC_IDLE;
                }

                /* otherwise just keep waiting for the response. */
            } else {
                pdebug(DEBUG_WARN, "Closing socket due to read error %s.", plc_tag_decode_error(rc));

                socket_close(plc->sock);
                socket_destroy(&(plc->sock));
                plc->sock = NULL;

                /* set up the state. */
                plc->flags.response_ready = 0;

                /* try to reconnect immediately. */
                plc->state = PLC_START;
            }


            /* in all cases we want to cycle through the state machine immediately. */
            cond_signal(plc->wait_cond);

            debug_set_tag_id(0);

            break;

        case PLC_ERR_WAIT:
            pdebug(DEBUG_DETAIL, "in PLC_ERR_WAIT state.");

            /* are we done waiting? */
            if(err_delay < time_ms()) {
                plc->state = PLC_START;
                cond_signal(plc->wait_cond);
            }
            break;

        default:
            pdebug(DEBUG_WARN, "Unknown state %d!", plc->state);
            plc->state = PLC_START;
            break;
        }

        /* wait if needed, could be signalled already. */
        cond_wait(plc->wait_cond, MODBUS_IDLE_WAIT_TIMEOUT);
    }

    pdebug(DEBUG_INFO, "Done.");

    THREAD_RETURN(0);
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
        mem_free(server_port);
        return PLCTAG_ERR_BAD_CONFIG;
    } else {
        server = server_port[0];
    }

    if(server_port[1] != NULL) {
        rc = str_to_int(server_port[1], &port);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to extract port number from server string \"%s\"!", plc->server);
            mem_free(server_port);
            server_port = NULL;
            return PLCTAG_ERR_BAD_CONFIG;
        }
    } else {
        port = MODBUS_DEFAULT_PORT;
    }

    pdebug(DEBUG_DETAIL, "Using server \"%s\" and port %d.", server, port);

    rc = socket_create(&(plc->sock));
    if(rc != PLCTAG_STATUS_OK) {
        /* done with the split string. */
        mem_free(server_port);
        server_port = NULL;

        pdebug(DEBUG_WARN, "Unable to create socket object, error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    /* connect to the socket */
    pdebug(DEBUG_DETAIL, "Connecting to %s on port %d...", server, port);
    rc = socket_connect_tcp(plc->sock, server, port);
    if(rc != PLCTAG_STATUS_OK) {
        /* done with the split string. */
        mem_free(server_port);

        pdebug(DEBUG_WARN, "Unable to connect to the server \"%s\", got error %s!", plc->server, plc_tag_decode_error(rc));
        socket_destroy(&(plc->sock));
        return rc;
    }

    /* done with the split string. */
    if(server_port) {
        mem_free(server_port);
        server_port = NULL;
    }

    /* we just connected, keep the connection open for a few seconds. */
    plc->inactivity_timeout_ms = MODBUS_INACTIVITY_TIMEOUT + time_ms();

    /* clear the state for reading and writing. */
    plc->flags.request_in_flight = 0;
    plc->flags.request_ready = 0;
    plc->flags.response_ready = 0;
    plc->read_data_len = 0;
    plc->write_data_len = 0;
    plc->write_data_offset = 0;

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}



int read_packet(modbus_plc_p plc)
{
    int rc = 1;
    int data_needed = PLC_READ_DATA_LEN - plc->read_data_len; /* ask for all we can store. */

    pdebug(DEBUG_DETAIL, "Starting.");

    /* socket could be closed due to inactivity. */
    if(!plc->sock) {
        pdebug(DEBUG_SPEW, "Socket is closed or missing.");
        return PLCTAG_STATUS_OK;
    }

    /* read as much as we can. */
    rc = socket_read(plc->sock, plc->read_data + plc->read_data_len, data_needed, SOCKET_READ_TIMEOUT);
    if(rc >= 0) {
        /* got data! Or got nothing, but no error. */
        plc->read_data_len += rc;

        /* calculate how much we still need. */
        data_needed = data_needed - plc->read_data_len;

        rc = PLCTAG_STATUS_OK;

        pdebug_dump_bytes(DEBUG_SPEW, plc->read_data, plc->read_data_len);
    } else if(rc == PLCTAG_ERR_TIMEOUT) {
        pdebug(DEBUG_DETAIL, "Done. Socket read timed out.");
        rc = PLCTAG_STATUS_OK;
    } else {
        pdebug(DEBUG_WARN, "Error, %s, reading socket!", plc_tag_decode_error(rc));
        return rc;
    }

    /* If we have enough data, calculate the actual amount we need. */
    if(plc->read_data_len >= MODBUS_MBAP_SIZE) {
        int packet_size = plc->read_data[5] + (plc->read_data[4] << 8);
        data_needed = (MODBUS_MBAP_SIZE + packet_size) - plc->read_data_len;

        pdebug(DEBUG_DETAIL, "data_needed=%d, packet_size=%d, read_data_len=%d", data_needed, packet_size, plc->read_data_len);

        if(data_needed > PLC_READ_DATA_LEN) {
            pdebug(DEBUG_WARN, "Error, packet size, %d, greater than buffer size, %d!", data_needed, PLC_READ_DATA_LEN);
            rc = PLCTAG_ERR_TOO_LARGE;
            return rc;
        }
    }

    if(data_needed == 0) {
        /* we got our packet. */
        pdebug(DEBUG_DETAIL, "Received full packet.");
        pdebug_dump_bytes(DEBUG_DETAIL, plc->read_data, plc->read_data_len);
        plc->flags.response_ready = 1;

        /* regardless of what request this is, there is nothing in flight. */
        plc->flags.request_in_flight = 0;
    }

    /* if we have some data in the buffer, keep the connection open. */
    if(plc->read_data_len > 0) {
        plc->inactivity_timeout_ms = MODBUS_INACTIVITY_TIMEOUT + time_ms();
    }

    /* if we have some data in the buffer, keep the connection open. */
    if(plc->read_data_len > 0) {
        plc->inactivity_timeout_ms = MODBUS_INACTIVITY_TIMEOUT + time_ms();
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}



int write_packet(modbus_plc_p plc)
{
    int rc = 1;
    int data_left = plc->write_data_len - plc->write_data_offset;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* if we have some data in the buffer, keep the connection open. */
    if(plc->write_data_len > 0) {
        plc->inactivity_timeout_ms = MODBUS_INACTIVITY_TIMEOUT + time_ms();
    }

    /* check socket, could be closed due to inactivity. */
    if(!plc->sock) {
        pdebug(DEBUG_DETAIL, "No socket or socket is closed.");
        return PLCTAG_STATUS_OK;
    }

    /* is there anything to do? */
    if(! plc->flags.request_ready) {
        pdebug(DEBUG_DETAIL, "Done. Nothing to do.");
        return PLCTAG_STATUS_OK;
    }

    /* try to get some data. */
    rc = socket_write(plc->sock, plc->write_data + plc->write_data_offset, data_left, SOCKET_WRITE_TIMEOUT);
    if(rc >= 0) {
        plc->write_data_offset += rc;
        data_left = plc->write_data_len - plc->write_data_offset;
    } else if(rc == PLCTAG_ERR_TIMEOUT) {
        pdebug(DEBUG_DETAIL, "Done.  Timeout writing to socket.");
        rc = PLCTAG_STATUS_OK;
    } else {
        pdebug(DEBUG_WARN, "Error, %s, writing to socket!", plc_tag_decode_error(rc));
        return rc;
    }

    if(rc >= 0) {
        /* clean up if full write was done. */
        if(data_left == 0) {
            pdebug(DEBUG_DETAIL, "Full packet written.");
            pdebug_dump_bytes(DEBUG_DETAIL, plc->write_data, plc->write_data_len);

            plc->flags.request_ready = 0;
            plc->write_data_len = 0;
            plc->write_data_offset = 0;
            plc->response_tag_id = plc->request_tag_id;
            plc->request_tag_id = 0;
        }

        rc = PLCTAG_STATUS_OK;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}



int run_tags(modbus_plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    /*
     * This is a little contorted here.   Because the tag could have been destroyed while
     * we were processing it, that could end up calling rc_dec() on the PLC itself.   So
     * we take another reference here and then release it after the loop.
     *
     * If we do not do that, then the PLC destructor could be called while we hold the
     * PLC's mutex here.   That results in deadlock.
     *
     * The basic loop is to go along all the tags taking each off the list.
     *   - if the tag is still waiting after processing, then add it to the waiting list.
     *   - if the tag is done, then add it to the completed list.
     *
     * Once all tags are processed, add the completed list to the end of the waiting list.
     */

    if(rc_inc(plc)) {
        critical_block(plc->mutex) {
            modbus_tag_p tail = plc->tags_tail;
            modbus_tag_p current = plc->tags_head;
            modbus_tag_p prev = NULL;
            modbus_tag_p next = NULL;

            pdebug(DEBUG_DETAIL, "Processing tag list.");

            while(current) {
                debug_set_tag_id(current->tag_id);

                pdebug(DEBUG_DETAIL, "Processing tag %d.", current->tag_id);

                next = current->next;

                current->flags.operation_complete = 0;

                rc = process_tag_unsafe(current, plc);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN,  "Error, %s, processing tag %d!", plc_tag_decode_error(rc), current->tag_id);
                }

                /* to ensure fairness, we move tags that completed operations to the end of the list. */
                if(current->flags.operation_complete) {
                    pdebug(DEBUG_DETAIL, "Moving tag to the end of the completed list.");

                    current->flags.operation_complete = 0;

                    /* unlink tag from the early part of the list. */
                    if(prev) {
                        prev->next = next;
                    } else {
                        plc->tags_head = next;
                    }

                    tail->next = current;
                    tail = current;
                    current->next = NULL;
                }

                debug_set_tag_id(0);

                /* terminate if we are at the end of the original list. */
                if(current == plc->tags_tail) {
                    pdebug(DEBUG_DETAIL, "Processed last tag.");
                    break;
                }

                prev = current;
                current = next;
            }

            if(tail) {
                pdebug(DEBUG_DETAIL, "Moving tail to end of processed tags.");
                plc->tags_tail = tail;
            }

            pdebug(DEBUG_DETAIL, "Done processing tag list.");
        }

        if(plc->flags.response_ready) {
            pdebug(DEBUG_WARN, "Response still pending after full tag pass.  Clearing buffer.");

            plc->flags.response_ready = 0;
            plc->read_data_len = 0;

            /* trigger the loop to not wait. */
            cond_signal(plc->wait_cond);
        }

        /* now drop the reference, which could cause the destructor to trigger. */
        rc_dec(plc);
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


/*
 * This is called in the context of the PLC thread.
 *
 * The process cannot lock the tag's API mutex.   If the tag user side is
 * blocked in plc_tag_read, plc_tag_write or any other possibly blocking
 * API call, then the API mutex will be held.
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

int process_tag_unsafe(modbus_tag_p tag, modbus_plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.");

    if(tag_get_abort_flag(tag)) {
        pdebug(DEBUG_DETAIL, "Aborting any in flight operations!");

        /* do this as one block to prevent half-changed state. */
        spin_block(&tag->tag_lock) {
            tag->flags._read = 0;
            tag->flags._write = 0;
            tag->flags._busy = 0;
            tag->flags._abort = 0;
            tag->flags.operation_complete = 0;
        }

        tag->seq_id = 0;

        pdebug(DEBUG_DETAIL, "Done.  Tag aborted operation.");
        return rc;
    }

    /* if there is not a socket, then we skip all this. */
    if(plc->sock) {
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

                    tag->status = (int8_t)rc;
                } else {
                    pdebug(DEBUG_SPEW, "No response yet.");
                }
            } else {
                /* we have a write request to do and nothing is in flight. */
                if(!plc->flags.request_ready && !plc->flags.request_in_flight) {
                    rc = create_write_request(plc, tag);
                } else {
                    pdebug(DEBUG_SPEW, "No buffer space for a response.");
                }

                tag->status = (int8_t)rc;
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

                    tag->status = (int8_t)rc;
                } else {
                    pdebug(DEBUG_SPEW, "No response yet.");
                }
            } else {
                /* we have a write request to do and nothing is in flight. */
                if(!plc->flags.request_ready && !plc->flags.request_in_flight) {
                    rc = create_read_request(plc, tag);
                } else {
                    pdebug(DEBUG_SPEW, "No buffer space for a response.");
                }

                tag->status = (int8_t)rc;
            }
        }
    } else {
        pdebug(DEBUG_SPEW, "Socket is closed, so clearing busy flag.");
        tag_set_busy_flag(tag, 0);

        /* if the tag wants to do something, make sure we retry the connection. */
        spin_block(&(tag->tag_lock)) {
            if(tag->flags._read || tag->flags._write) {
                plc->inactivity_timeout_ms = MODBUS_INACTIVITY_TIMEOUT + time_ms();
            }
        }
    }

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}


/* Read response.
 *    Byte  Meaning
 *      0    High byte of request sequence ID.
 *      1    Low byte of request sequence ID.
 *      2    High byte of the protocol version identifier (zero).
 *      3    Low byte of the protocol version identifier (zero).
 *      4    High byte of the message length.
 *      5    Low byte of the message length.
 *      6    Device address.
 *      7    Function code.
 *      8    First byte of the result.
 *      ...  up to 253 bytes of payload.
 */


int check_read_response(modbus_plc_p plc, modbus_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    uint16_t seq_id = (uint16_t)((uint16_t)plc->read_data[1] +(uint16_t)(plc->read_data[0] << 8));
    int partial_read = 0;

    pdebug(DEBUG_INFO, "Starting.");

    if(seq_id == tag->seq_id) {
        uint8_t has_error = plc->read_data[7] & (uint8_t)0x80;

        /* the operation is complete regardless of the outcome. */
        tag->flags.operation_complete = 1;

        if(has_error) {
            rc = translate_modbus_error(plc->read_data[8]);

            pdebug(DEBUG_WARN, "Got read response %ud, with error %s, of length %d.", (int)(unsigned int)seq_id, plc_tag_decode_error(rc), plc->read_data_len);
        } else {
            int registers_per_request = (MAX_MODBUS_RESPONSE_PAYLOAD * 8) / tag->elem_size;
            int register_offset = (tag->request_num * registers_per_request);
            int byte_offset = (register_offset * tag->elem_size) / 8;
            uint8_t payload_size = plc->read_data[8];
            int copy_size = ((tag->size - byte_offset) < payload_size ? (tag->size - byte_offset) : payload_size);

            /* no error. So copy the data. */
            pdebug(DEBUG_INFO, "Got read response %u of length %d with payload of size %d.", (int)(unsigned int)seq_id, plc->read_data_len, payload_size);
            pdebug(DEBUG_DETAIL, "registers_per_request = %d", registers_per_request);
            pdebug(DEBUG_DETAIL, "register_offset = %d", register_offset);
            pdebug(DEBUG_DETAIL, "byte_offset = %d", byte_offset);
            pdebug(DEBUG_DETAIL, "copy_size = %d", copy_size);

            mem_copy(tag->data + byte_offset, &plc->read_data[9], copy_size);

            /* are we done? */
            if(tag->size > (byte_offset + copy_size)) {
                /* Not yet. */
                pdebug(DEBUG_DETAIL, "Not done reading entire tag.");
                partial_read = 1;
            } else {
                /* read is done. */
                pdebug(DEBUG_DETAIL, "Read is complete.");
                partial_read = 0;
            }

            rc = PLCTAG_STATUS_OK;
        }

        /* either way, clean up the PLC buffer. */
        plc->read_data_len = 0;
        plc->flags.response_ready = 0;

        /* clean up tag*/
        if(!partial_read) {
            spin_block(&tag->tag_lock) {
                tag->flags._read = 0;
                tag->flags._busy = 0;
                tag->seq_id = 0;
                tag->read_complete = 1;
                tag->status = (int8_t)rc;
                tag->request_num = 0;
            }
        } else {
            /*
             * keep doing a read, but clear the busy flag so that we
             * keep creating new requests.
             */
            spin_block(&tag->tag_lock) {
                tag->flags._read = 1;
                tag->flags._busy = 0;
                tag->request_num++;
                tag->status = (int8_t)rc;
            }
        }
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
    int registers_per_request = (MAX_MODBUS_RESPONSE_PAYLOAD * 8) / tag->elem_size;
    int base_register = tag->reg_base + (tag->request_num * registers_per_request);
    int register_count = tag->elem_count - (tag->request_num * registers_per_request);

    pdebug(DEBUG_INFO, "Starting.");

    pdebug(DEBUG_DETAIL, "seq_id=%d", seq_id);
    pdebug(DEBUG_DETAIL, "registers_per_request = %d", registers_per_request);
    pdebug(DEBUG_DETAIL, "base_register = %d", base_register);
    pdebug(DEBUG_DETAIL, "register_count = %d", register_count);

    /* clamp the number of registers we ask for to what will fit. */
    if(register_count > registers_per_request) {
        register_count = registers_per_request;
    }

    pdebug(DEBUG_INFO, "preparing read request for %d registers (of %d total) from base register %d.", register_count, tag->elem_count, base_register);

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
        case MB_REG_COIL:
            plc->write_data[7] = MB_CMD_READ_COIL_MULTI; plc->write_data_len++;
            break;

        case MB_REG_DISCRETE_INPUT:
            plc->write_data[7] = MB_CMD_READ_DISCRETE_INPUT_MULTI; plc->write_data_len++;
            break;

        case MB_REG_HOLDING_REGISTER:
            plc->write_data[7] = MB_CMD_READ_HOLDING_REGISTER_MULTI; plc->write_data_len++;
            break;

        case MB_REG_INPUT_REGISTER:
            plc->write_data[7] = MB_CMD_READ_INPUT_REGISTER_MULTI; plc->write_data_len++;
            break;

        default:
            pdebug(DEBUG_WARN, "Unsupported register type %d!", tag->reg_type);
            return PLCTAG_ERR_UNSUPPORTED;
            break;
    }

    /* register base. */
    plc->write_data[plc->write_data_len] = (uint8_t)((base_register >> 8) & 0xFF); plc->write_data_len++;
    plc->write_data[plc->write_data_len] = (uint8_t)((base_register >> 0) & 0xFF); plc->write_data_len++;

    /* number of elements to read. */
    plc->write_data[plc->write_data_len] = (uint8_t)((register_count >> 8) & 0xFF); plc->write_data_len++;
    plc->write_data[plc->write_data_len] = (uint8_t)((register_count >> 0) & 0xFF); plc->write_data_len++;

    pdebug(DEBUG_DETAIL, "Created request:");
    pdebug_dump_bytes(DEBUG_DETAIL, plc->write_data, plc->write_data_len);

    /* ready to go! */
    spin_block(&tag->tag_lock) {
        tag->flags._busy = 1;
        tag->seq_id = seq_id;
    }

    /* FIXME - could this ever be hoisted above the barrier above? */
    plc->flags.request_ready = 1;
    plc->flags.request_in_flight = 1;
    plc->request_tag_id = tag->tag_id;

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}




/* Write response.
 *    Byte  Meaning
 *      0    High byte of request sequence ID.
 *      1    Low byte of request sequence ID.
 *      2    High byte of the protocol version identifier (zero).
 *      3    Low byte of the protocol version identifier (zero).
 *      4    High byte of the message length.
 *      5    Low byte of the message length.
 *      6    Device address.
 *      7    Function code.
 *      8    High byte of first register address/Error code.
 *      9    Low byte of the first register address.
 *     10    High byte of the register count.
 *     11    Low byte of the register count.
 */

int check_write_response(modbus_plc_p plc, modbus_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    uint16_t seq_id = (uint16_t)((uint16_t)plc->read_data[1] + (uint16_t)(plc->read_data[0] << 8));
    int partial_write = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    if(seq_id == tag->seq_id) {
        uint8_t has_error = plc->read_data[7] & (uint8_t)0x80;

        /* this is our response, so the operation is complete regardless of the status. */
        tag->flags.operation_complete = 1;

        if(has_error) {
            rc = translate_modbus_error(plc->read_data[8]);

            pdebug(DEBUG_WARN, "Got write response %ud, with error %s, of length %d.", (int)(unsigned int)seq_id, plc_tag_decode_error(rc), plc->read_data_len);
        } else {
            int registers_per_request = (MAX_MODBUS_RESPONSE_PAYLOAD * 8) / tag->elem_size;
            int register_offset = (tag->request_num * registers_per_request);
            int byte_offset = (register_offset * tag->elem_size) / 8;

            /* no error. So copy the data. */
            pdebug(DEBUG_DETAIL, "registers_per_request = %d", registers_per_request);
            pdebug(DEBUG_DETAIL, "register_offset = %d", register_offset);
            pdebug(DEBUG_DETAIL, "byte_offset = %d", byte_offset);

            /* are we done? */
            if(tag->size > byte_offset) {
                /* Not yet. */
                pdebug(DEBUG_INFO, "Not done writing entire tag.");
                partial_write = 1;
            } else {
                /* read is done. */
                pdebug(DEBUG_INFO, "Write is complete.");
                partial_write = 0;
            }

            rc = PLCTAG_STATUS_OK;
        }

        /* either way, clean up the PLC buffer. */
        plc->read_data_len = 0;
        plc->flags.response_ready = 0;
        /* clean up tag*/
        if(!partial_write) {
            spin_block(&tag->tag_lock) {
                tag->flags._write = 0;
                tag->flags._busy = 0;
                tag->seq_id = 0;
                tag->request_num = 0;
                tag->write_complete = 1;
                tag->status = (int8_t)rc;
            }
        } else {
            /*
             * keep doing a write, but clear the busy flag so that we
             * keep creating new requests.
             */
            spin_block(&tag->tag_lock) {
                tag->flags._write = 1;
                tag->flags._busy = 0;
                tag->status = (int8_t)rc;
            }
        }
    } else {
        pdebug(DEBUG_SPEW, "Not our response.");

        rc = PLCTAG_STATUS_OK;
    }

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}


/* build the write request.
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
 *     12    Number of bytes of data to write.
 *     13... Data bytes.
 */

int create_write_request(modbus_plc_p plc, modbus_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    uint16_t seq_id = (++(plc->seq_id) ? plc->seq_id : ++(plc->seq_id)); // disallow zero
    int registers_per_request = (MAX_MODBUS_REQUEST_PAYLOAD * 8) / tag->elem_size;
    int base_register = tag->reg_base + (tag->request_num * registers_per_request);
    int register_count = tag->elem_count - (tag->request_num * registers_per_request);
    int register_offset = (tag->request_num * registers_per_request);
    int byte_offset = (register_offset * tag->elem_size) / 8;
    int request_payload_size = 0;

    pdebug(DEBUG_INFO, "Starting.");

    pdebug(DEBUG_DETAIL, "seq_id=%d", seq_id);
    pdebug(DEBUG_DETAIL, "registers_per_request = %d", registers_per_request);
    pdebug(DEBUG_DETAIL, "base_register = %d", base_register);
    pdebug(DEBUG_DETAIL, "register_count = %d", register_count);
    pdebug(DEBUG_DETAIL, "register_offset = %d", register_offset);
    pdebug(DEBUG_DETAIL, "byte_offset = %d", byte_offset);

    /* clamp the number of registers we ask for to what will fit. */
    if(register_count > registers_per_request) {
        register_count = registers_per_request;
    }

    /* how many bytes, rounded up to the nearest byte. */
    request_payload_size = ((register_count * tag->elem_size) + 7) / 8;

    pdebug(DEBUG_INFO, "preparing write request for %d registers (of %d total) from base register %d of payload size %d in bytes.", register_count, tag->elem_count, base_register, request_payload_size);

    /* FIXME - remove this when we figure out how to push multiple requests. */
    plc->write_data_len = 0;

    /* build the request sequence ID */
    plc->write_data[plc->write_data_len] = (uint8_t)((seq_id >> 8) & 0xFF); plc->write_data_len++;
    plc->write_data[plc->write_data_len] = (uint8_t)((seq_id >> 0) & 0xFF); plc->write_data_len++;

    /* protocol version is always zero */
    plc->write_data[plc->write_data_len] = 0; plc->write_data_len++;
    plc->write_data[plc->write_data_len] = 0; plc->write_data_len++;

    /* request packet length */
    plc->write_data[plc->write_data_len] = (uint8_t)(((request_payload_size + 7) >> 8) & 0xFF); plc->write_data_len++;
    plc->write_data[plc->write_data_len] = (uint8_t)(((request_payload_size + 7) >> 0) & 0xFF); plc->write_data_len++;

    /* device address */
    plc->write_data[plc->write_data_len] = plc->server_id; plc->write_data_len++;

    /* function code depends on the register type. */
    switch(tag->reg_type) {
        case MB_REG_COIL:
            plc->write_data[7] = MB_CMD_WRITE_COIL_MULTI; plc->write_data_len++;
            break;

        case MB_REG_DISCRETE_INPUT:
            pdebug(DEBUG_WARN, "Done. You cannot write a discrete input!");
            return PLCTAG_ERR_UNSUPPORTED;
            break;

        case MB_REG_HOLDING_REGISTER:
            plc->write_data[7] = MB_CMD_WRITE_HOLDING_REGISTER_MULTI; plc->write_data_len++;
            break;

        case MB_REG_INPUT_REGISTER:
            pdebug(DEBUG_WARN, "Done. You cannot write an analog input!");
            return PLCTAG_ERR_UNSUPPORTED;
            break;

        default:
            pdebug(DEBUG_WARN, "Done. Unsupported register type %d!", tag->reg_type);
            return PLCTAG_ERR_UNSUPPORTED;
            break;
    }

    /* register base. */
    plc->write_data[plc->write_data_len] = (uint8_t)((base_register >> 8) & 0xFF); plc->write_data_len++;
    plc->write_data[plc->write_data_len] = (uint8_t)((base_register >> 0) & 0xFF); plc->write_data_len++;

    /* number of elements to read. */
    plc->write_data[plc->write_data_len] = (uint8_t)((register_count >> 8) & 0xFF); plc->write_data_len++;
    plc->write_data[plc->write_data_len] = (uint8_t)((register_count >> 0) & 0xFF); plc->write_data_len++;

    /* number of bytes of data to write. */
    plc->write_data[plc->write_data_len] = (uint8_t)(unsigned int)(request_payload_size); plc->write_data_len++;

    /* copy the tag data. */
    mem_copy(&plc->write_data[plc->write_data_len], &tag->data[byte_offset], request_payload_size);
    plc->write_data_len += request_payload_size;

    pdebug(DEBUG_DETAIL, "Created request:");
    pdebug_dump_bytes(DEBUG_DETAIL, plc->write_data, plc->write_data_len);

    /* ready to go. */
    spin_block(&tag->tag_lock) {
        tag->flags._busy = 1;
        tag->seq_id = (uint16_t)(unsigned int)seq_id;
        tag->request_num++;
    }

    plc->flags.request_ready = 1;
    plc->flags.request_in_flight = 1;
    plc->request_tag_id = tag->tag_id;

    pdebug(DEBUG_DETAIL, "Done.");

    debug_set_tag_id(tag->tag_id);

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




int parse_register_name(attr attribs, modbus_reg_type_t *reg_type, int *reg_base)
{
    int rc = PLCTAG_STATUS_OK;
    const char *reg_name = attr_get_str(attribs, "name", NULL);

    pdebug(DEBUG_INFO, "Starting.");

    if(!reg_name || str_length(reg_name)<3) {
        pdebug(DEBUG_WARN, "Incorrect or unsupported register name!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* see if we can parse the register number. */
    rc = str_to_int(&reg_name[2], reg_base);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to parse register number!");
        *reg_base = 0;
        *reg_type = MB_REG_UNKNOWN;
        return rc;
    }

    /* get the register type. */
    if(str_cmp_i_n(reg_name, "co", 2) == 0) {
        pdebug(DEBUG_DETAIL, "Found coil type.");
        *reg_type = MB_REG_COIL;
    } else if(str_cmp_i_n(reg_name, "di", 2) == 0) {
        pdebug(DEBUG_DETAIL, "Found discrete input type.");
        *reg_type = MB_REG_DISCRETE_INPUT;
    } else if(str_cmp_i_n(reg_name, "hr", 2) == 0) {
        pdebug(DEBUG_DETAIL, "Found holding register type.");
        *reg_type = MB_REG_HOLDING_REGISTER;
    } else if(str_cmp_i_n(reg_name, "ir", 2) == 0) {
        pdebug(DEBUG_DETAIL, "Found input register type.");
        *reg_type = MB_REG_INPUT_REGISTER;
    } else {
        pdebug(DEBUG_WARN, "Unknown register type, %s!", reg_name);
        *reg_base = 0;
        *reg_type = MB_REG_UNKNOWN;
        return PLCTAG_ERR_BAD_PARAM;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
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

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    tag_set_abort_flag(tag, 1);

    /* wake the PLC loop if we need to. */
    cond_signal(tag->plc->wait_cond);

    pdebug(DEBUG_DETAIL, "Done.");

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

    tag->status = PLCTAG_STATUS_OK;
    tag_set_read_flag(tag, 1);

    /* wake up the PLC */
    cond_signal(tag->plc->wait_cond);

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

    cond_signal(tag->plc->wait_cond);

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_PENDING;
}


/****** Data Accessor Functions ******/

int mb_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value)
{
    int res = default_value;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    tag->status = PLCTAG_STATUS_OK;

    /* match the attribute. */
    if(str_cmp_i(attrib_name, "elem_size") == 0) {
        res = (tag->elem_size + 7)/8; /* return size in bytes! */
    } else if(str_cmp_i(attrib_name, "elem_count") == 0) {
        res = tag->elem_count;
    } else {
        pdebug(DEBUG_WARN,"Attribute \"%s\" is not supported.", attrib_name);
        tag->status = PLCTAG_ERR_UNSUPPORTED;
    }

    return res;
}


int mb_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value)
{
    (void)attrib_name;
    (void)new_value;

    pdebug(DEBUG_WARN, "Attribute \"%s\" is unsupported!", attrib_name);

    raw_tag->status = PLCTAG_ERR_UNSUPPORTED;

    return PLCTAG_ERR_UNSUPPORTED;
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

