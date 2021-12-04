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
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <platform.h>
#include <lib/libplctag.h>
#include <mb/modbus.h>
#include <util/atomic_int.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/rc.h>

/* data definitions */

#define PLC_SOCKET_ERR_MAX_DELAY (5000)
#define PLC_SOCKET_ERR_START_DELAY (50)
#define PLC_SOCKET_ERR_DELAY_WAIT_INCREMENT (10)
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
#define SOCKET_CONNECT_TIMEOUT (20) /* connect timeout step in milliseconds */
#define MODBUS_IDLE_WAIT_TIMEOUT (100) /* idle wait timeout in milliseconds */
#define MAX_MODBUS_REQUESTS (16) /* per the Modbus specification */

typedef struct modbus_tag_t *modbus_tag_p;
typedef struct modbus_tag_list_t *modbus_tag_list_p;

struct modbus_tag_list_t {
    struct modbus_tag_t *head;
    struct modbus_tag_t *tail;
};



struct modbus_plc_t {
    struct modbus_plc_t *next;

    /* keep a list of tags for this PLC. */
    struct modbus_tag_list_t tag_list;
    // struct modbus_tag_list_t request_tag_list;
    // struct modbus_tag_list_t response_tag_list;

    /* hostname/ip and possibly port of the server. */
    char *server;
    sock_p sock;
    uint8_t server_id;
    int connection_group_id;

    /* State */
    struct {
        unsigned int terminate:1;
        unsigned int response_ready:1;
        unsigned int request_ready:1;
        // unsigned int request_in_flight:1;
    } flags;
    uint16_t seq_id;

    /* thread related state */
    thread_p handler_thread;
    mutex_p mutex;
    //cond_p wait_cond;
    enum {
        PLC_CONNECT_START = 0,
        PLC_CONNECT_WAIT,
        PLC_READY,
        PLC_BUILD_REQUEST,
        PLC_SEND_REQUEST,
        PLC_RECEIVE_RESPONSE,
        PLC_ERR_WAIT
    } state;
    int max_requests_in_flight;
    int32_t tags_with_requests[MAX_MODBUS_REQUESTS];

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

typedef enum {
    MB_CMD_READ_COIL_MULTI = 0x01,
    MB_CMD_READ_DISCRETE_INPUT_MULTI = 0x02,
    MB_CMD_READ_HOLDING_REGISTER_MULTI = 0x03,
    MB_CMD_READ_INPUT_REGISTER_MULTI = 0x04,
    MB_CMD_WRITE_COIL_SINGLE = 0x05,
    MB_CMD_WRITE_HOLDING_REGISTER_SINGLE = 0x06,
    MB_CMD_WRITE_COIL_MULTI = 0x0F,
    MB_CMD_WRITE_HOLDING_REGISTER_MULTI = 0x10
} modbug_cmd_t;



typedef enum {
    TAG_OP_IDLE = 0,
    TAG_OP_READ_REQUEST,
    TAG_OP_READ_RESPONSE,
    TAG_OP_WRITE_REQUEST,
    TAG_OP_WRITE_RESPONSE
} tag_op_type_t;


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

    tag_op_type_t op;
    uint16_t request_num;
    uint16_t seq_id;

    /* which request slot are we using? */
    int request_slot;

    /* data for the tag. */
    int elem_count;
    int elem_size;
};


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
static int find_or_create_plc(attr attribs, modbus_plc_p *plc);
static int parse_register_name(attr attribs, modbus_reg_type_t *reg_type, int *reg_base);
static void modbus_tag_destructor(void *tag_arg);
static void modbus_plc_destructor(void *plc_arg);
static THREAD_FUNC(modbus_plc_handler);
static void wake_plc_thread(modbus_plc_p plc);
static int connect_plc(modbus_plc_p plc);
static int tickle_all_tags(modbus_plc_p plc);
static int tickle_tag(modbus_plc_p plc, modbus_tag_p tag);
static int find_request_slot(modbus_plc_p plc, modbus_tag_p tag);
static void clear_request_slot(modbus_plc_p plc, modbus_tag_p tag);
static int receive_response(modbus_plc_p plc);
static int send_request(modbus_plc_p plc);
static int check_read_response(modbus_plc_p plc, modbus_tag_p tag);
static int create_read_request(modbus_plc_p plc, modbus_tag_p tag);
static int check_write_response(modbus_plc_p plc, modbus_tag_p tag);
static int create_write_request(modbus_plc_p plc, modbus_tag_p tag);
static int translate_modbus_error(uint8_t err_code);

/* tag list functions */
// static int list_is_empty(modbus_tag_list_p list);
static modbus_tag_p pop_tag(modbus_tag_list_p list);
static void push_tag(modbus_tag_list_p list, modbus_tag_p tag);
static struct modbus_tag_list_t merge_lists(modbus_tag_list_p first, modbus_tag_list_p last);
static int remove_tag(modbus_tag_list_p list, modbus_tag_p tag);


/* tag vtable functions. */

/* control functions. */
static int mb_abort(plc_tag_p p_tag);
static int mb_read_start(plc_tag_p p_tag);
static int mb_tag_status(plc_tag_p p_tag);
static int mb_tickler(plc_tag_p p_tag);
static int mb_write_start(plc_tag_p p_tag);
static int mb_wake_plc(plc_tag_p p_tag);


/* data accessors */
static int mb_get_int_attrib(plc_tag_p tag, const char *attrib_name, int default_value);
static int mb_set_int_attrib(plc_tag_p tag, const char *attrib_name, int new_value);

struct tag_vtable_t modbus_vtable = {
    (tag_vtable_func)mb_abort,
    (tag_vtable_func)mb_read_start,
    (tag_vtable_func)mb_tag_status,
    (tag_vtable_func)mb_tickler,
    (tag_vtable_func)mb_write_start,
    (tag_vtable_func)mb_wake_plc,

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

    /* set up the generic parts. */
    rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to initialize generic tag parts!");
        rc_dec(tag);
        return (plc_tag_p)NULL;
    }

    /* find the PLC object. */
    rc = find_or_create_plc(attribs, &(tag->plc));
    if(rc == PLCTAG_STATUS_OK) {
        /* put the tag on the PLC's list. */
        critical_block(tag->plc->mutex) {
            push_tag(&(tag->plc->tag_list), tag);
        }
    } else {
        pdebug(DEBUG_WARN, "Unable to create new tag!  Error %s!", plc_tag_decode_error(rc));
        tag->status = (int8_t)rc;
    }

    /* kick off a read. */
    mb_read_start((plc_tag_p)tag);

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

    /* set initial tag operation state. */
    (*tag)->op = TAG_OP_IDLE;

    /* initialize the current request slot */
    (*tag)->request_slot = -1;

    /* make sure the generic tag tickler thread does not call the generic tickler. */
    (*tag)->skip_tickler = 1;

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
            int rc = remove_tag(&(tag->plc->tag_list), tag);
            if(rc == PLCTAG_STATUS_OK) {
                pdebug(DEBUG_DETAIL, "Tag removed from the PLC successfully.");
            } else if(rc == PLCTAG_ERR_NOT_FOUND) {
                pdebug(DEBUG_WARN, "Tag not found in the PLC's list for the tag's operation %d.", tag->op);
            } else {
                pdebug(DEBUG_WARN, "Error %s while trying to remove the tag from the PLC's list!", plc_tag_decode_error(rc));
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

    if(tag->tag_cond_wait) {
        cond_destroy(&(tag->tag_cond_wait));
        tag->tag_cond_wait = NULL;
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
    int connection_group_id = attr_get_int(attribs, "connection_group_id", 0);
    int max_requests_in_flight = attr_get_int(attribs, "max_requests_in_flight", 1);
    int is_new = 0;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    /* clamp maximum requests in flight. */
    if(max_requests_in_flight > MAX_MODBUS_REQUESTS) {
        pdebug(DEBUG_WARN, "max_requests_in_flight set to %d which is higher than the Modbus limit of %d.", max_requests_in_flight, MAX_MODBUS_REQUESTS);
        max_requests_in_flight = MAX_MODBUS_REQUESTS;
    }

    if(max_requests_in_flight < 1) {
        pdebug(DEBUG_WARN, "max_requests_in_flight must be between 1 and %d, inclusive, was %d.", MAX_MODBUS_REQUESTS, max_requests_in_flight);
        max_requests_in_flight = 1;
    }

    if(server_id < 0 || server_id > 255) {
        pdebug(DEBUG_WARN, "Server ID, %d, out of bounds or missing!", server_id);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    /* see if we can find a matching server. */
    critical_block(mb_mutex) {
        modbus_plc_p *walker = &plcs;

        while(*walker && (*walker)->connection_group_id != connection_group_id && (*walker)->server_id != (uint8_t)(unsigned int)server_id && str_cmp_i(server, (*walker)->server) != 0) {
            walker = &((*walker)->next);
        }

        /* did we find one. */
        if(*walker && (*walker)->connection_group_id == connection_group_id && (*walker)->server_id == (uint8_t)(unsigned int)server_id && str_cmp_i(server, (*walker)->server) == 0) {
            pdebug(DEBUG_DETAIL, "Using existing PLC connection.");
            *plc = rc_inc(*walker);
            is_new = 0;
        } else {
            /* nope, make a new one.  Do as little as possible in the mutex. */

            pdebug(DEBUG_DETAIL, "Creating new PLC connection.");

            is_new = 1;

            *plc = (modbus_plc_p)rc_alloc((int)(unsigned int)sizeof(struct modbus_plc_t), modbus_plc_destructor);
            if(*plc) {
                pdebug(DEBUG_DETAIL, "Setting connection_group_id to %d.", connection_group_id);
                (*plc)->connection_group_id = connection_group_id;

                /* copy the server string so that we can find this again. */
                (*plc)->server = str_dup(server);
                if(! ((*plc)->server)) {
                    pdebug(DEBUG_WARN, "Unable to allocate Modbus PLC server string!");
                    rc = PLCTAG_ERR_NO_MEM;
                } else {
                    /* make sure we can be found. */
                    (*plc)->server_id = (uint8_t)(unsigned int)server_id;

                    /* other tags could try to add themselves immediately. */

                    /* clear tag list */
                    (*plc)->tag_list.head = NULL;
                    (*plc)->tag_list.tail = NULL;

                    /* create the PLC mutex to protect the tag list. */
                    rc = mutex_create(&((*plc)->mutex));
                    if(rc != PLCTAG_STATUS_OK) {
                        pdebug(DEBUG_WARN, "Unable to create new mutex, error %s!", plc_tag_decode_error(rc));
                        break;
                    }

                    /* set up the maximum request depth. */
                    (*plc)->max_requests_in_flight = max_requests_in_flight;

                    /* link up the the PLC into the global list. */
                    (*plc)->next = plcs;
                    plcs = *plc;

                    /* now the PLC can be found and the tag list is ready for use. */
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
                (*plc)->state = PLC_CONNECT_START;

                rc = thread_create(&((*plc)->handler_thread), modbus_plc_handler, 32768, (void *)(*plc));
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Unable to create new handler thread, error %s!", plc_tag_decode_error(rc));
                    break;
                }

                pdebug(DEBUG_DETAIL, "Created thread %p.", (*plc)->handler_thread);
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


/* never enter this from within the handler thread itself! */
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

        /* signal the socket to free the thread. */
        wake_plc_thread(plc);

        /* wait for the thread to terminate and destroy it. */
        thread_join(plc->handler_thread);
        thread_destroy(&plc->handler_thread);

        plc->handler_thread = NULL;
    }

    if(plc->mutex) {
        mutex_destroy(&plc->mutex);
        plc->mutex = NULL;
    }

    if(plc->sock) {
        socket_destroy(&plc->sock);
        plc->sock = NULL;
    }

    if(plc->server) {
        mem_free(plc->server);
        plc->server = NULL;
    }

    /* check to make sure we have no tags left. */
    if(plc->tag_list.head) {
        pdebug(DEBUG_WARN, "There are tags still remaining in the tag list, memory leak possible!");
    }

    pdebug(DEBUG_INFO, "Done.");
}

#define UPDATE_ERR_DELAY() \
            do { \
                err_delay = err_delay*2; \
                if(err_delay > PLC_SOCKET_ERR_MAX_DELAY) { \
                    err_delay = PLC_SOCKET_ERR_MAX_DELAY; \
                } \
                err_delay_until = (int64_t)((double)err_delay*((double)rand()/(double)(RAND_MAX))) + time_ms(); \
            } while(0)


THREAD_FUNC(modbus_plc_handler)
{
    int rc = PLCTAG_STATUS_OK;
    modbus_plc_p plc = (modbus_plc_p)arg;
    int64_t err_delay = PLC_SOCKET_ERR_START_DELAY;
    int64_t err_delay_until = 0;
    int sock_events = SOCK_EVENT_NONE;
    int waitable_events = SOCK_EVENT_NONE;

    pdebug(DEBUG_INFO, "Starting.");

    if(!plc) {
        pdebug(DEBUG_WARN, "Null PLC pointer passed!");
        THREAD_RETURN(0);
    }

    while(! plc->flags.terminate) {
        rc = tickle_all_tags(plc);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error %s tickling tags!", plc_tag_decode_error(rc));
            /* FIXME - what should we do here? */
        }

        /* if there is still a response marked ready, clean it up. */
        if(plc->flags.response_ready) {
            pdebug(DEBUG_DETAIL, "Orphan response found.");
            plc->flags.response_ready = 0;
            plc->read_data_len = 0;
        }

        switch(plc->state) {
        case PLC_CONNECT_START:
            pdebug(DEBUG_DETAIL, "in PLC_CONNECT_START state.");

            /* connect to the PLC */
            rc = connect_plc(plc);
            if(rc == PLCTAG_STATUS_PENDING) {
                pdebug(DEBUG_DETAIL, "Socket connection process started.  Going to PLC_CONNECT_WAIT state.");
                plc->state = PLC_CONNECT_WAIT;
            } else if(rc == PLCTAG_STATUS_OK) {
                pdebug(DEBUG_DETAIL, "Successfully connected to the PLC.  Going to PLC_READY state.");

                /* reset err_delay */
                err_delay = PLC_SOCKET_ERR_START_DELAY;

                plc->state = PLC_READY;
            } else {
                pdebug(DEBUG_WARN, "Error %s received while starting socket connection.", plc_tag_decode_error(rc));

                socket_destroy(&(plc->sock));

                /* exponential increase with jitter. */
                UPDATE_ERR_DELAY();

                pdebug(DEBUG_WARN, "Unable to connect to the PLC, will retry later! Going to PLC_ERR_WAIT state to wait %"PRId64"ms.", err_delay);

                plc->state = PLC_ERR_WAIT;
            }
            break;

        case PLC_CONNECT_WAIT:
            rc = socket_connect_tcp_check(plc->sock, SOCKET_CONNECT_TIMEOUT);
            if(rc == PLCTAG_STATUS_OK) {
                pdebug(DEBUG_DETAIL, "Socket connected, going to state PLC_READY.");

                /* we just connected, keep the connection open for a few seconds. */
                plc->inactivity_timeout_ms = MODBUS_INACTIVITY_TIMEOUT + time_ms();

                /* reset err_delay */
                err_delay = PLC_SOCKET_ERR_START_DELAY;

                plc->state = PLC_READY;
            } else if(rc == PLCTAG_ERR_TIMEOUT) {
                pdebug(DEBUG_DETAIL, "Still waiting for socket to connect.");

                /* do not wait more.   The TCP connection check will wait in select(). */
            } else {
                pdebug(DEBUG_WARN, "Error %s received while waiting for socket connection.", plc_tag_decode_error(rc));

                socket_destroy(&(plc->sock));

                /* exponential increase with jitter. */
                UPDATE_ERR_DELAY();

                pdebug(DEBUG_WARN, "Unable to connect to the PLC, will retry later! Going to PLC_ERR_WAIT state to wait %"PRId64"ms.", err_delay);

                plc->state = PLC_ERR_WAIT;
            }
            break;

        case PLC_READY:
            pdebug(DEBUG_DETAIL, "in PLC_READY state.");

            /* calculate what events we should be waiting for. */
            waitable_events = SOCK_EVENT_DEFAULT_MASK | SOCK_EVENT_CAN_READ;

            /* if there is a request queued for sending, send it. */
            if(plc->flags.request_ready) {
                waitable_events |= SOCK_EVENT_CAN_WRITE;
            }

            /* this will wait if nothing wakes it up or until it times out. */
            sock_events = socket_wait_event(plc->sock, waitable_events, MODBUS_IDLE_WAIT_TIMEOUT);

            /* check for socket errors or disconnects. */
            if((sock_events & SOCK_EVENT_ERROR) || (sock_events & SOCK_EVENT_DISCONNECT)) {
                if(sock_events & SOCK_EVENT_DISCONNECT) {
                    pdebug(DEBUG_WARN, "Unexepected socket disconnect!");
                } else {
                    pdebug(DEBUG_WARN, "Unexpected socket error!");
                }

                pdebug(DEBUG_WARN, "Going to state PLC_CONNECT_START");

                socket_destroy(&(plc->sock));

                plc->state = PLC_CONNECT_START;
                break;
            }

            /* preference pushing requests to the PLC */
            if(sock_events & SOCK_EVENT_CAN_WRITE) {
                if(plc->flags.request_ready) {
                    pdebug(DEBUG_DETAIL, "There is a request ready to send and we can send, going to state PLC_SEND_REQUEST.");
                    plc->state = PLC_SEND_REQUEST;
                    break;
                } else {
                    /* clear the buffer indexes just in case */
                    plc->write_data_len = 0;
                    plc->write_data_offset = 0;
                    pdebug(DEBUG_DETAIL, "Request ready state changed while we waited for the socket.");
                }
            }

            if(sock_events & SOCK_EVENT_CAN_READ) {
                pdebug(DEBUG_DETAIL, "We can receive a response going to state PLC_RECEIVE_RESPONSE.");
                plc->state = PLC_RECEIVE_RESPONSE;
                break;
            }

            if(sock_events & SOCK_EVENT_TIMEOUT) {
                pdebug(DEBUG_DETAIL, "Timed out waiting for something to happen.");
            }

            if(sock_events & SOCK_EVENT_WAKE_UP) {
                pdebug(DEBUG_DETAIL, "Someone woke us up.");
            }

            break;

        case PLC_SEND_REQUEST:
            debug_set_tag_id((int)plc->request_tag_id);
            pdebug(DEBUG_DETAIL, "in PLC_SEND_REQUEST state.");

            rc = send_request(plc);
            if(rc == PLCTAG_STATUS_OK) {
                pdebug(DEBUG_DETAIL, "Request sent, going to back to state PLC_READY.");

                plc->flags.request_ready = 0;
                plc->write_data_len = 0;
                plc->write_data_offset = 0;

                plc->state = PLC_READY;
            } else if(rc == PLCTAG_STATUS_PENDING) {
                pdebug(DEBUG_DETAIL, "Not all data written, will try again.");
            } else {
                pdebug(DEBUG_WARN, "Closing socket due to write error %s.", plc_tag_decode_error(rc));

                socket_destroy(&(plc->sock));

                /* set up the state. */
                plc->flags.response_ready = 0;
                plc->flags.request_ready = 0;
                plc->read_data_len = 0;
                plc->write_data_len = 0;
                plc->write_data_offset = 0;

                /* try to reconnect immediately. */
                plc->state = PLC_CONNECT_START;
            }

            /* if we did not send all the packet, we stay in this state and keep trying. */

            debug_set_tag_id(0);

            break;


        case PLC_RECEIVE_RESPONSE:
            pdebug(DEBUG_DETAIL, "in PLC_RECEIVE_RESPONSE state.");

            /* get a packet */
            rc = receive_response(plc);
            if(rc == PLCTAG_STATUS_OK) {
                pdebug(DEBUG_DETAIL, "Response ready, going back to PLC_READY state.");
                plc->flags.response_ready = 1;
                plc->state = PLC_READY;
            } else if(rc == PLCTAG_STATUS_PENDING) {
                pdebug(DEBUG_DETAIL, "Response not complete, continue reading data.");
            } else {
                pdebug(DEBUG_WARN, "Closing socket due to read error %s.", plc_tag_decode_error(rc));

                socket_destroy(&(plc->sock));

                /* set up the state. */
                plc->flags.response_ready = 0;
                plc->flags.request_ready = 0;
                plc->read_data_len = 0;
                plc->write_data_len = 0;
                plc->write_data_offset = 0;

                /* try to reconnect immediately. */
                plc->state = PLC_CONNECT_START;
            }

            /* in all cases we want to cycle through the state machine immediately. */

            break;

        case PLC_ERR_WAIT:
            pdebug(DEBUG_DETAIL, "in PLC_ERR_WAIT state.");

            /* clean up the socket in case we did not earlier */
            if(plc->sock) {
                socket_destroy(&(plc->sock));
            }

            /* wait until done. */
            if(err_delay_until > time_ms()) {
                pdebug(DEBUG_DETAIL, "Waiting for at least %"PRId64"ms.", (err_delay_until - time_ms()));
                sleep_ms(PLC_SOCKET_ERR_DELAY_WAIT_INCREMENT);
            } else {
                pdebug(DEBUG_DETAIL, "Error wait is over, going to state PLC_CONNECT_START.");
                plc->state = PLC_CONNECT_START;
            }
            break;

        default:
            pdebug(DEBUG_WARN, "Unknown state %d!", plc->state);
            plc->state = PLC_CONNECT_START;
            break;
        }

        /* wait if needed, could be signalled already. */
        //cond_wait(plc->wait_cond, MODBUS_IDLE_WAIT_TIMEOUT);
    }

    pdebug(DEBUG_INFO, "Done.");

    THREAD_RETURN(0);
}


void wake_plc_thread(modbus_plc_p plc)
{
    pdebug(DEBUG_DETAIL, "Starting.");

    if(plc ) {
        if(plc->sock) {
            socket_wake(plc->sock);
        } else {
            pdebug(DEBUG_DETAIL, "PLC socket pointer is NULL.");
        }
    } else {
        pdebug(DEBUG_WARN, "PLC pointer is NULL!");
    }

    pdebug(DEBUG_DETAIL, "Done.");
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
    rc = socket_connect_tcp_start(plc->sock, server, port);
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
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

    /* clear the state for reading and writing. */
    plc->flags.request_ready = 0;
    plc->flags.response_ready = 0;
    plc->read_data_len = 0;
    plc->write_data_len = 0;
    plc->write_data_offset = 0;

    pdebug(DEBUG_DETAIL, "Done with status %s.", plc_tag_decode_error(rc));

    return rc;
}


int tickle_all_tags(modbus_plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    struct modbus_tag_list_t idle_list = { NULL, NULL };
    struct modbus_tag_list_t active_list = { NULL, NULL };
    modbus_tag_p tag = NULL;

    pdebug(DEBUG_DETAIL, "Starting.");

    /*
     * The mutex prevents the list from changing, the PLC
     * from being freed, and the tags from being freed.
     */
    critical_block(plc->mutex) {
        while((tag = pop_tag(&(plc->tag_list)))) {
            int tag_pushed = 0;

            debug_set_tag_id(tag->tag_id);

            /* make sure nothing else can modify the tag while we are */
            critical_block(tag->api_mutex) {
            // if(mutex_try_lock(tag->api_mutex) == PLCTAG_STATUS_OK) {
                rc = tickle_tag(plc, tag);
                if(rc == PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_SPEW, "Pushing tag onto idle list.");
                    push_tag(&idle_list, tag);
                    tag_pushed = 1;
                } else if(rc == PLCTAG_STATUS_PENDING) {
                    pdebug(DEBUG_SPEW, "Pushing tag onto active list.");
                    push_tag(&active_list, tag);
                    tag_pushed = 1;
                } else {
                    pdebug(DEBUG_WARN, "Error %s tickling tag! Pushing tag onto idle list.", plc_tag_decode_error(rc));
                    push_tag(&idle_list, tag);
                    tag_pushed = 1;
                }
            }

            if(tag_pushed) {
                /* call the callbacks outside the API mutex. */
                plc_tag_generic_handle_event_callbacks((plc_tag_p)tag);
            } else {
                pdebug(DEBUG_WARN, "Tag mutex not taken!  Doing emergency push of tag onto idle list.");
                push_tag(&idle_list, tag);
            }

            rc = PLCTAG_STATUS_OK;

            debug_set_tag_id(0);
        }

        /* merge the lists and replace the old list. */
        pdebug(DEBUG_SPEW, "Merging active and idle lists.");
        plc->tag_list = merge_lists(&active_list, &idle_list);
    }

    pdebug(DEBUG_DETAIL, "Done: %s", plc_tag_decode_error(rc));

    return rc;
}



int tickle_tag(modbus_plc_p plc, modbus_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    int op = tag->op;

    pdebug(DEBUG_SPEW, "Starting.");

    switch(op) {
        case TAG_OP_IDLE:
            pdebug(DEBUG_SPEW, "Tag is idle.");
            rc = PLCTAG_STATUS_OK;
            break;

        case TAG_OP_READ_REQUEST:
            /* if the PLC is ready and there is no request queued yet, build a request. */
            if(find_request_slot(plc, tag) == PLCTAG_STATUS_OK) {
                rc = create_read_request(plc, tag);
                if(rc == PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_DETAIL, "Read request created.");

                    tag->op = TAG_OP_READ_RESPONSE;
                    plc->flags.request_ready = 1;

                    rc = PLCTAG_STATUS_PENDING;
                } else {
                    pdebug(DEBUG_WARN, "Error %s creating read request!", plc_tag_decode_error(rc));

                    /* remove the tag from the request slot. */
                    clear_request_slot(plc, tag);

                    tag->op = TAG_OP_IDLE;
                    tag->read_complete = 1;
                    tag->read_in_flight = 0;
                    tag->status = (int8_t)rc;

                    plc_tag_tickler_wake();
                    plc_tag_generic_wake_tag((plc_tag_p)tag);
                    rc = PLCTAG_STATUS_OK;
                }
            } else {
                pdebug(DEBUG_SPEW, "Request already in flight or PLC not ready, waiting for next chance.");
                rc = PLCTAG_STATUS_PENDING;
            }
            break;

        case TAG_OP_READ_RESPONSE:
            /* cross check the state. */
            if(plc->state == PLC_CONNECT_START
            || plc->state == PLC_CONNECT_WAIT
            || plc->state == PLC_ERR_WAIT) {
                pdebug(DEBUG_WARN, "PLC changed state, restarting request.");
                tag->op = TAG_OP_READ_REQUEST;
                break;
            }

            if(plc->flags.response_ready) {
                rc = check_read_response(plc, tag);
                if(rc == PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_DETAIL, "Found our response.");

                    plc->flags.response_ready = 0;
                    tag->op = TAG_OP_IDLE;

                    /* remove the tag from the request slot. */
                    clear_request_slot(plc, tag);

                    /* tell the world we are done. */
                    plc_tag_tickler_wake();
                    plc_tag_generic_wake_tag((plc_tag_p)tag);

                    rc = PLCTAG_STATUS_OK;
                } else if(rc == PLCTAG_ERR_PARTIAL) {
                    pdebug(DEBUG_DETAIL, "Found our response, but we are not done.");

                    plc->flags.response_ready = 0;
                    tag->op = TAG_OP_READ_REQUEST;

                    /* remove the tag from the request slot. */
                    clear_request_slot(plc, tag);

                    rc = PLCTAG_STATUS_OK;
                } else if(rc == PLCTAG_ERR_NO_MATCH) {
                    pdebug(DEBUG_SPEW, "Not our response.");
                    rc = PLCTAG_STATUS_PENDING;
                } else {
                    pdebug(DEBUG_WARN, "Error %s checking read response!", plc_tag_decode_error(rc));

                    tag->op = TAG_OP_IDLE;
                    tag->read_complete = 1;
                    tag->read_in_flight = 0;
                    tag->status = (int8_t)rc;

                    /* remove the tag from the request slot. */
                    clear_request_slot(plc, tag);

                    /* tell the world we are done. */
                    plc_tag_tickler_wake();
                    plc_tag_generic_wake_tag((plc_tag_p)tag);

                    rc = PLCTAG_STATUS_OK;
                }
            } else {
                pdebug(DEBUG_SPEW, "No response yet, Continue waiting.");
                rc = PLCTAG_STATUS_PENDING;
            }
            break;

        case TAG_OP_WRITE_REQUEST:
            /* if the PLC is ready and there is no request queued yet, build a request. */
            if(find_request_slot(plc, tag) == PLCTAG_STATUS_OK) {
                rc = create_write_request(plc, tag);
                if(rc == PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_DETAIL, "Write request created.");

                    tag->op = TAG_OP_WRITE_RESPONSE;
                    plc->flags.request_ready = 1;

                    rc = PLCTAG_STATUS_PENDING;
                } else {
                    pdebug(DEBUG_WARN, "Error %s creating write request!", plc_tag_decode_error(rc));

                    /* remove the tag from the request slot. */
                    clear_request_slot(plc, tag);

                    tag->op = TAG_OP_IDLE;
                    tag->write_complete = 1;
                    tag->write_in_flight = 0;
                    tag->status = (int8_t)rc;

                    plc_tag_tickler_wake();
                    plc_tag_generic_wake_tag((plc_tag_p)tag);

                    rc = PLCTAG_STATUS_OK;
                }
            } else {
                pdebug(DEBUG_SPEW, "Request already in flight or PLC not ready, waiting for next chance.");
                rc = PLCTAG_STATUS_PENDING;
            }
            break;

        case TAG_OP_WRITE_RESPONSE:
            /* cross check the state. */
            if(plc->state == PLC_CONNECT_START
            || plc->state == PLC_CONNECT_WAIT
            || plc->state == PLC_ERR_WAIT) {
                pdebug(DEBUG_WARN, "PLC changed state, restarting request.");
                tag->op = TAG_OP_WRITE_REQUEST;
                break;
            }

            if(plc->flags.response_ready) {
                rc = check_write_response(plc, tag);
                if(rc == PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_DETAIL, "Found our response.");

                    plc->flags.response_ready = 0;
                    tag->op = TAG_OP_IDLE;

                    /* remove the tag from the request slot. */
                    clear_request_slot(plc, tag);

                    /* tell the world we are done. */
                    plc_tag_tickler_wake();
                    plc_tag_generic_wake_tag((plc_tag_p)tag);

                    rc = PLCTAG_STATUS_OK;
                } else if(rc == PLCTAG_ERR_PARTIAL) {
                    pdebug(DEBUG_DETAIL, "Found our response, but we are not done.");

                    plc->flags.response_ready = 0;
                    tag->op = TAG_OP_WRITE_REQUEST;

                    rc = PLCTAG_STATUS_OK;
                } else if(rc == PLCTAG_ERR_NO_MATCH) {
                    pdebug(DEBUG_SPEW, "Not our response.");
                    rc = PLCTAG_STATUS_PENDING;
                } else {
                    pdebug(DEBUG_WARN, "Error %s checking write response!", plc_tag_decode_error(rc));

                    tag->op = TAG_OP_IDLE;
                    tag->write_complete = 1;
                    tag->write_in_flight = 0;
                    tag->status = (int8_t)rc;

                    /* remove the tag from the request slot. */
                    clear_request_slot(plc, tag);

                    /* tell the world we are done. */
                    plc_tag_tickler_wake();
                    plc_tag_generic_wake_tag((plc_tag_p)tag);

                    rc = PLCTAG_STATUS_OK;
                }
            } else {
                pdebug(DEBUG_SPEW, "No response yet, Continue waiting.");
                rc = PLCTAG_STATUS_PENDING;
            }
            break;

        default:
            pdebug(DEBUG_WARN, "Unknown tag operation %d!", op);

            tag->op = TAG_OP_IDLE;
            tag->status = (int8_t)PLCTAG_ERR_NOT_IMPLEMENTED;

            /* tell the world we are done. */
            plc_tag_tickler_wake();
            plc_tag_generic_wake_tag((plc_tag_p)tag);

            rc = PLCTAG_STATUS_OK;
            break;
    }

    /*
     * Call the generic tag tickler function to handle auto read/write and set
     * up events.
     */
    plc_tag_generic_tickler((plc_tag_p)tag);

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}




int find_request_slot(modbus_plc_p plc, modbus_tag_p tag)
{
    pdebug(DEBUG_SPEW, "Starting.");

    if(plc->flags.request_ready) {
        pdebug(DEBUG_DETAIL, "There is a request already queued for sending.");
        return PLCTAG_ERR_BUSY;
    }

    if(plc->state != PLC_READY) {
        pdebug(DEBUG_DETAIL, "PLC not ready.");
        return PLCTAG_ERR_BUSY;
    }

    if(tag->tag_id == 0) {
        pdebug(DEBUG_DETAIL, "Tag not ready.");
        return PLCTAG_ERR_BUSY;
    }

    /* search for a slot. */
    for(int slot=0; slot < plc->max_requests_in_flight; slot++) {
        if(plc->tags_with_requests[slot] == 0) {
            pdebug(DEBUG_DETAIL, "Found request slot %d for tag %"PRId32".", slot, tag->tag_id);
            plc->tags_with_requests[slot] = tag->tag_id;
            tag->request_slot = slot;
            return PLCTAG_STATUS_OK;
        }
    }

    pdebug(DEBUG_SPEW, "Done.");

    return PLCTAG_ERR_NO_RESOURCES;
}


void clear_request_slot(modbus_plc_p plc, modbus_tag_p tag)
{
    pdebug(DEBUG_DETAIL, "Starting for tag %"PRId32".", tag->tag_id);

    /* find the tag in the slots. */
    for(int slot=0; slot < plc->max_requests_in_flight; slot++) {
        if(plc->tags_with_requests[slot] == tag->tag_id) {
            pdebug(DEBUG_DETAIL, "Found tag %"PRId32" in slot %d.", tag->tag_id, slot);

            if(slot != tag->request_slot) {
                pdebug(DEBUG_DETAIL, "Tag was not in expected slot %d!", tag->request_slot);
            }

            plc->tags_with_requests[slot] = 0;
            tag->request_slot = -1;
        }
    }

    pdebug(DEBUG_DETAIL, "Done for tag %"PRId32".", tag->tag_id);
}



int receive_response(modbus_plc_p plc)
{
    int rc = 0;
    int data_needed = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* socket could be closed due to inactivity. */
    if(!plc->sock) {
        pdebug(DEBUG_SPEW, "Socket is closed or missing.");
        return PLCTAG_STATUS_OK;
    }

    do {
        /* how much data do we need? */
        if(plc->read_data_len >= MODBUS_MBAP_SIZE) {
            int packet_size = plc->read_data[5] + (plc->read_data[4] << 8);
            data_needed = (MODBUS_MBAP_SIZE + packet_size) - plc->read_data_len;

            pdebug(DEBUG_DETAIL, "Packet header read, data_needed=%d, packet_size=%d, read_data_len=%d", data_needed, packet_size, plc->read_data_len);

            if(data_needed > PLC_READ_DATA_LEN) {
                pdebug(DEBUG_WARN, "Error, packet size, %d, greater than buffer size, %d!", data_needed, PLC_READ_DATA_LEN);
                return PLCTAG_ERR_TOO_LARGE;
            } else if(data_needed < 0) {
                pdebug(DEBUG_WARN, "Read more than a packet!  Expected %d bytes, but got %d bytes!", (MODBUS_MBAP_SIZE + packet_size), plc->read_data_len);
                return PLCTAG_ERR_TOO_LARGE;
            }
        } else {
            data_needed = MODBUS_MBAP_SIZE - plc->read_data_len;
            pdebug(DEBUG_DETAIL, "Still reading packet header, data_needed=%d, read_data_len=%d", data_needed, plc->read_data_len);
        }

        if(data_needed == 0) {
            pdebug(DEBUG_DETAIL, "Got all data needed.");
            break;
        }

        /* read the socket. */
        rc = socket_read(plc->sock, plc->read_data + plc->read_data_len, data_needed, SOCKET_READ_TIMEOUT);
        if(rc >= 0) {
            /* got data! Or got nothing, but no error. */
            plc->read_data_len += rc;

            pdebug_dump_bytes(DEBUG_SPEW, plc->read_data, plc->read_data_len);
        } else if(rc == PLCTAG_ERR_TIMEOUT) {
            pdebug(DEBUG_DETAIL, "Done. Socket read timed out.");
            return PLCTAG_STATUS_PENDING;
        } else {
            pdebug(DEBUG_WARN, "Error, %s, reading socket!", plc_tag_decode_error(rc));
            return rc;
        }

        pdebug(DEBUG_DETAIL, "After reading the socket, total read=%d and data needed=%d.", plc->read_data_len, data_needed);
    } while(rc > 0);


    if(data_needed == 0) {
        /* we got our packet. */
        pdebug(DEBUG_DETAIL, "Received full packet.");
        pdebug_dump_bytes(DEBUG_DETAIL, plc->read_data, plc->read_data_len);
        plc->flags.response_ready = 1;

        rc = PLCTAG_STATUS_OK;
    } else {
        /* data_needed is greater than zero. */
        pdebug(DEBUG_DETAIL, "Received partial packet of %d bytes of %d.", plc->read_data_len, (data_needed + plc->read_data_len));
        rc = PLCTAG_STATUS_PENDING;
    }

    /* if we have some data in the buffer, keep the connection open. */
    if(plc->read_data_len > 0) {
        plc->inactivity_timeout_ms = MODBUS_INACTIVITY_TIMEOUT + time_ms();
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}



int send_request(modbus_plc_p plc)
{
    int rc = 1;
    int data_left = plc->write_data_len - plc->write_data_offset;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* check socket, could be closed due to inactivity. */
    if(!plc->sock) {
        pdebug(DEBUG_DETAIL, "No socket or socket is closed.");
        return PLCTAG_ERR_BAD_CONNECTION;
    }

    /* if we have some data in the buffer, keep the connection open. */
    if(plc->write_data_len > 0) {
        plc->inactivity_timeout_ms = MODBUS_INACTIVITY_TIMEOUT + time_ms();
    }

    /* is there anything to do? */
    if(! plc->flags.request_ready) {
        pdebug(DEBUG_WARN, "No packet to send!");
        return PLCTAG_ERR_NO_DATA;
    }

    /* try to send some data. */
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

    /* clean up if full write was done. */
    if(data_left == 0) {
        pdebug(DEBUG_DETAIL, "Full packet written.");
        pdebug_dump_bytes(DEBUG_DETAIL, plc->write_data, plc->write_data_len);

        // plc->flags.request_ready = 0;
        plc->write_data_len = 0;
        plc->write_data_offset = 0;
        plc->response_tag_id = plc->request_tag_id;
        plc->request_tag_id = 0;

        rc = PLCTAG_STATUS_OK;
    } else {
        pdebug(DEBUG_DETAIL, "Partial packet written.");
        rc = PLCTAG_STATUS_PENDING;
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

    pdebug(DEBUG_DETAIL, "Starting.");

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

    tag->seq_id = seq_id;
    plc->flags.request_ready = 1;
    plc->request_tag_id = tag->tag_id;

    pdebug(DEBUG_DETAIL, "Created read request:");
    pdebug_dump_bytes(DEBUG_DETAIL, plc->write_data, plc->write_data_len);

    pdebug(DEBUG_DETAIL, "Done.");

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

    pdebug(DEBUG_DETAIL, "Starting.");

    if(seq_id == tag->seq_id) {
        uint8_t has_error = plc->read_data[7] & (uint8_t)0x80;

        /* the operation is complete regardless of the outcome. */
        //tag->flags.operation_complete = 1;

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
            pdebug(DEBUG_DETAIL, "Got read response %u of length %d with payload of size %d.", (int)(unsigned int)seq_id, plc->read_data_len, payload_size);
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
            pdebug(DEBUG_DETAIL, "Read is complete.  Cleaning up tag state.");
            tag->seq_id = 0;
            tag->read_complete = 1;
            tag->read_in_flight = 0;
            tag->status = (int8_t)rc;
            tag->request_num = 0;
        } else {
            pdebug(DEBUG_DETAIL, "Read is partially complete.  We need to do at least one more request.");
            rc = PLCTAG_ERR_PARTIAL;
            tag->request_num++;
            tag->status = (int8_t)PLCTAG_STATUS_PENDING;
        }
    } else {
        pdebug(DEBUG_DETAIL, "Not our response.");

        rc = PLCTAG_ERR_NO_MATCH;
    }

    pdebug(DEBUG_DETAIL, "Done: %s", plc_tag_decode_error(rc));

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

    pdebug(DEBUG_DETAIL, "Starting.");

    pdebug(DEBUG_SPEW, "seq_id=%d", seq_id);
    pdebug(DEBUG_SPEW, "registers_per_request = %d", registers_per_request);
    pdebug(DEBUG_SPEW, "base_register = %d", base_register);
    pdebug(DEBUG_SPEW, "register_count = %d", register_count);
    pdebug(DEBUG_SPEW, "register_offset = %d", register_offset);
    pdebug(DEBUG_SPEW, "byte_offset = %d", byte_offset);

    /* clamp the number of registers we ask for to what will fit. */
    if(register_count > registers_per_request) {
        register_count = registers_per_request;
    }

    /* how many bytes, rounded up to the nearest byte. */
    request_payload_size = ((register_count * tag->elem_size) + 7) / 8;

    pdebug(DEBUG_DETAIL, "preparing write request for %d registers (of %d total) from base register %d of payload size %d in bytes.", register_count, tag->elem_count, base_register, request_payload_size);

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

    tag->seq_id = (uint16_t)(unsigned int)seq_id;
    plc->flags.request_ready = 1;
    plc->request_tag_id = tag->tag_id;

    pdebug(DEBUG_DETAIL, "Created write request:");
    pdebug_dump_bytes(DEBUG_DETAIL, plc->write_data, plc->write_data_len);

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
        //tag->flags.operation_complete = 1;

        if(has_error) {
            rc = translate_modbus_error(plc->read_data[8]);

            pdebug(DEBUG_WARN, "Got write response %ud, with error %s, of length %d.", (int)(unsigned int)seq_id, plc_tag_decode_error(rc), plc->read_data_len);
        } else {
            int registers_per_request = (MAX_MODBUS_RESPONSE_PAYLOAD * 8) / tag->elem_size;
            int next_register_offset = ((tag->request_num+1) * registers_per_request);
            int next_byte_offset = (next_register_offset * tag->elem_size) / 8;

            /* no error. So copy the data. */
            pdebug(DEBUG_DETAIL, "registers_per_request = %d", registers_per_request);
            pdebug(DEBUG_DETAIL, "next_register_offset = %d", next_register_offset);
            pdebug(DEBUG_DETAIL, "next_byte_offset = %d", next_byte_offset);

            /* are we done? */
            if(tag->size > next_byte_offset) {
                /* Not yet. */
                pdebug(DEBUG_SPEW, "Not done writing entire tag.");
                partial_write = 1;
            } else {
                /* read is done. */
                pdebug(DEBUG_DETAIL, "Write is complete.");
                partial_write = 0;
            }

            rc = PLCTAG_STATUS_OK;
        }

        /* either way, clean up the PLC buffer. */
        plc->read_data_len = 0;
        plc->flags.response_ready = 0;

        /* clean up tag*/
        if(!partial_write) {
            pdebug(DEBUG_DETAIL, "Write complete. Cleaning up tag state.");
            tag->seq_id = 0;
            tag->request_num = 0;
            tag->write_complete = 1;
            tag->write_in_flight = 0;
            tag->status = (int8_t)rc;
        } else {
            pdebug(DEBUG_DETAIL, "Write partially complete.  We need to do at least one more write request.");
            rc = PLCTAG_ERR_PARTIAL;
            tag->request_num++;
            tag->status = (int8_t)PLCTAG_STATUS_PENDING;
        }
    } else {
        pdebug(DEBUG_SPEW, "Not our response.");

        rc = PLCTAG_STATUS_PENDING;
    }

    pdebug(DEBUG_SPEW, "Done.");

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



modbus_tag_p pop_tag(modbus_tag_list_p list)
{
    modbus_tag_p tmp = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    tmp = list->head;

    /* unlink */
    if(tmp) {
        list->head = tmp->next;
        tmp->next = NULL;
    }

    /* if we removed the last element, then set the tail to NULL. */
    if(!list->head) {
        list->tail = NULL;
    }

    pdebug(DEBUG_SPEW, "Done.");

    return tmp;
}



void push_tag(modbus_tag_list_p list, modbus_tag_p tag)
{
    pdebug(DEBUG_SPEW, "Starting.");

    tag->next = NULL;

    if(list->tail) {
        list->tail->next = tag;
        list->tail = tag;
    } else {
        /* nothing in the list. */
        list->head = tag;
        list->tail = tag;
    }

    pdebug(DEBUG_SPEW, "Done.");
}



struct modbus_tag_list_t merge_lists(modbus_tag_list_p first, modbus_tag_list_p last)
{
    struct modbus_tag_list_t result;

    pdebug(DEBUG_SPEW, "Starting.");

    if(first->head) {
        modbus_tag_p tmp = first->head;

        pdebug(DEBUG_SPEW, "First list:");
        while(tmp) {
            pdebug(DEBUG_SPEW, "  tag %d", tmp->tag_id);
            tmp = tmp->next;
        }
    } else {
        pdebug(DEBUG_SPEW, "First list: empty.");
    }

    if(last->head) {
        modbus_tag_p tmp = last->head;

        pdebug(DEBUG_SPEW, "Second list:");
        while(tmp) {
            pdebug(DEBUG_SPEW, "  tag %d", tmp->tag_id);
            tmp = tmp->next;
        }
    } else {
        pdebug(DEBUG_SPEW, "Second list: empty.");
    }

    /* set up the head of the new list. */
    result.head = (first->head ? first->head : last->head);

    /* stitch up the tail of the first list. */
    if(first->tail) {
        first->tail->next = last->head;
    }

    /* set up the tail of the new list */
    result.tail = (last->tail ? last->tail : first->tail);

    /* make sure the old lists do not reference the tags. */
    first->head = NULL;
    first->tail = NULL;
    last->head = NULL;
    last->tail = NULL;

    if(result.head) {
        modbus_tag_p tmp = result.head;

        pdebug(DEBUG_SPEW, "Result list:");
        while(tmp) {
            pdebug(DEBUG_SPEW, "  tag %d", tmp->tag_id);
            tmp = tmp->next;
        }
    } else {
        pdebug(DEBUG_SPEW, "Result list: empty.");
    }

    pdebug(DEBUG_SPEW, "Done.");

    return result;
}



int remove_tag(modbus_tag_list_p list, modbus_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    modbus_tag_p cur = list->head;
    modbus_tag_p prev = NULL;

    pdebug(DEBUG_DETAIL, "Starting.");

    while(cur && cur != tag) {
        prev = cur;
        cur = cur->next;
    }

    if(cur == tag) {
        /* found it. */
        if(prev) {
            prev->next = tag->next;
        } else {
            /* at the head of the list. */
            list->head = tag->next;
        }

        if(list->tail == tag) {
            list->tail = prev;
        }

        rc = PLCTAG_STATUS_OK;
    } else {
        rc = PLCTAG_STATUS_PENDING;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


/****** Tag Control Functions ******/

/* These must all be called with the API mutex on the tag held. */

int mb_abort(plc_tag_p p_tag)
{
    modbus_tag_p tag = (modbus_tag_p)p_tag;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /*
     * This is safe to do because we hold the tag
     * API mutex here.   When the PLC thread runs
     * and calls tickle_tag (the only place where
     * the op changes) it holds the API mutex as well.
     * Thus this code below is only accessible by
     * one thread at a time.
     */
    tag->seq_id = 0;
    tag->request_num = 0;
    tag->status = (int8_t)PLCTAG_STATUS_OK;
    tag->op = TAG_OP_IDLE;

    clear_request_slot(tag->plc, tag);

    /* wake the PLC loop if we need to. */
    wake_plc_thread(tag->plc);

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}



int mb_read_start(plc_tag_p p_tag)
{
    modbus_tag_p tag = (modbus_tag_p)p_tag;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /*
     * This is safe to do because we hold the tag
     * API mutex here.   When the PLC thread runs
     * and calls tickle_tag (the only place where
     * the op changes) it holds the API mutex as well.
     * Thus this code below is only accessible by
     * one thread at a time.
     */
    if(tag->op == TAG_OP_IDLE) {
        tag->op = TAG_OP_READ_REQUEST;
    } else {
        pdebug(DEBUG_WARN, "Operation in progress!");
        return PLCTAG_ERR_BUSY;
    }

    /* wake the PLC loop if we need to. */
    wake_plc_thread(tag->plc);

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_PENDING;
}



int mb_tag_status(plc_tag_p p_tag)
{
    modbus_tag_p tag = (modbus_tag_p)p_tag;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(tag->status != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Status not OK, returning %s.", plc_tag_decode_error(tag->status));
        return tag->status;
    }

    if(tag->op != TAG_OP_IDLE) {
        pdebug(DEBUG_DETAIL, "Operation in progress, returning PLCTAG_STATUS_PENDING.");
        return PLCTAG_STATUS_PENDING;
    }

    pdebug(DEBUG_DETAIL, "Done.");

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

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /*
     * This is safe to do because we hold the tag
     * API mutex here.   When the PLC thread runs
     * and calls tickle_tag (the only place where
     * the op changes) it holds the API mutex as well.
     * Thus this code below is only accessible by
     * one thread at a time.
     */
    if(tag->op == TAG_OP_IDLE) {
        tag->op = TAG_OP_WRITE_REQUEST;
    } else {
        pdebug(DEBUG_WARN, "Operation in progress!");
        return PLCTAG_ERR_BUSY;
    }

    /* wake the PLC loop if we need to. */
    wake_plc_thread(tag->plc);

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_PENDING;
}



int mb_wake_plc(plc_tag_p p_tag)
{
    modbus_tag_p tag = (modbus_tag_p)p_tag;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* wake the PLC thread. */
    wake_plc_thread(tag->plc);

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

