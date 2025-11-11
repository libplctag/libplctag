/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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
#include <libplctag/lib/libplctag.h>
#include <libplctag/protocols/mb/modbus.h>
#include <limits.h>
#include <platform.h>
#include <stdlib.h>
#include <utils/atomic_utils.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/random_utils.h>
#include <utils/rc.h>

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
#define MAX_MODBUS_PDU_PAYLOAD (253) /* everything after the server address */
#define MODBUS_INACTIVITY_TIMEOUT (5000)
#define SOCKET_READ_TIMEOUT (20)       /* read timeout in milliseconds */
#define SOCKET_WRITE_TIMEOUT (20)      /* write timeout in milliseconds */
#define SOCKET_CONNECT_TIMEOUT (20)    /* connect timeout step in milliseconds */
#define MODBUS_IDLE_WAIT_TIMEOUT (100) /* idle wait timeout in milliseconds */
#define MAX_MODBUS_REQUESTS (16)       /* per the Modbus specification */

typedef struct modbus_tag_t *modbus_tag_p;
typedef struct modbus_tag_list_t *modbus_tag_list_p;

struct modbus_plc_t {
    struct modbus_plc_t *next;

    /* Keep a ring linked list of tags for this PLC */
    struct modbus_tag_t *tag_ring;

    /* Count of tags currently attached to this PLC */
    atomic_int32_t tag_count;

    /* hostname/ip and possibly port of the server. */
    char *server;
    sock_p sock;
    uint8_t server_id;
    int connection_group_id;

    /* State */
    struct {
        unsigned int terminate : 1;
        unsigned int response_ready : 1;
        unsigned int request_ready : 1;
        // unsigned int request_in_flight:1;
    } flags;
    uint16_t seq_id;

    /* thread related state */
    thread_p handler_thread;
    mutex_p mutex;

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

typedef enum {
    MB_REG_UNKNOWN,
    MB_REG_COIL,
    MB_REG_DISCRETE_INPUT,
    MB_REG_HOLDING_REGISTER,
    MB_REG_INPUT_REGISTER
} modbus_reg_type_t;

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
tag_byte_order_t modbus_tag_byte_order = {.is_allocated = 0,

                                          .int16_order = {1, 0},
                                          .int32_order = {3, 2, 1, 0},
                                          .int64_order = {7, 6, 5, 4, 3, 2, 1, 0},
                                          .float32_order = {3, 2, 1, 0},
                                          .float64_order = {7, 6, 5, 4, 3, 2, 1, 0},

                                          .str_is_defined = 0, /* FIXME */
                                          .str_is_counted = 0,
                                          .str_is_fixed_length = 0,
                                          .str_is_zero_terminated = 0,
                                          .str_is_byte_swapped = 0,

                                          .str_pad_to_multiple_bytes = 0,
                                          .str_count_word_bytes = 0,
                                          .str_max_capacity = 0,
                                          .str_total_length = 0,
                                          .str_pad_bytes = 0};


/* Modbus module globals. */
mutex_p mb_mutex = NULL;
modbus_plc_p plcs = NULL;

/* PLC lifecycle management */
static atomic_int32_t plc_count;
static cond_p plc_cleanup_cond = NULL;


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
static const char *op_to_str(tag_op_type_t op);

/* tag list functions */
static void debug_ring(modbus_plc_p plc);
static int add_tag(modbus_plc_p plc, modbus_tag_p tag);
static int remove_tag(modbus_plc_p plc, modbus_tag_p tag);


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
    .abort = mb_abort,
    .read = mb_read_start,
    .status = mb_tag_status,
    .tickler = mb_tickler,
    .write = mb_write_start,
    .wake_plc = mb_wake_plc,

    /* data accessors */
    .get_int_attrib = mb_get_int_attrib,
    .set_int_attrib = mb_set_int_attrib,
    .get_byte_array_attrib = NULL,
};


/****** main entry point *******/

plc_tag_p mb_tag_create(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                        void *userdata) {
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
    rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to initialize generic tag parts!");
        rc_dec(tag);
        return (plc_tag_p)NULL;
    }

    /* find the PLC object. */
    rc = find_or_create_plc(attribs, &(tag->plc));
    if(rc == PLCTAG_STATUS_OK) {
        /* put the tag on the PLC's list. */
        add_tag(tag->plc, tag);
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

int create_tag_object(attr attribs, modbus_tag_p *tag) {
    int rc = PLCTAG_STATUS_OK;
    int data_size = 0;
    int reg_size = 0;
    int elem_count = attr_get_int(attribs, "elem_count", 1);
    modbus_reg_type_t reg_type = MB_REG_UNKNOWN;
    int reg_base = 0;

    if(elem_count < 0) {
        pdebug(DEBUG_WARN, "Element count should not be a negative value!");
        return PLCTAG_ERR_BAD_PARAM;
    }

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
        case MB_REG_DISCRETE_INPUT: reg_size = 1; break;

        case MB_REG_HOLDING_REGISTER:
            /* fall through */
        case MB_REG_INPUT_REGISTER: reg_size = 16; break;

        default:
            pdebug(DEBUG_WARN, "Unsupported register type!");
            return PLCTAG_ERR_BAD_PARAM;
            break;
    }

    /* calculate the data size in bytes. */
    data_size = ((elem_count * reg_size) + 7) / 8;

    pdebug(DEBUG_DETAIL, "Tag data size is %d bytes.", data_size);

    /* allocate the tag */
    *tag = (modbus_tag_p)rc_alloc((int)(unsigned int)sizeof(struct modbus_tag_t) + data_size, modbus_tag_destructor);
    if(!*tag) {
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


void modbus_tag_destructor(void *tag_arg) {
    modbus_tag_p tag = (modbus_tag_p)tag_arg;
    modbus_plc_p plc = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Destructor called with null pointer!");
        return;
    }

    /* Take a reference to the PLC to safely check its state */
    if(tag->plc) {
        plc = rc_inc(tag->plc);
    }

    /* abort everything, but only if PLC is still valid and not terminating */
    if(plc && !plc->flags.terminate) {
        pdebug(DEBUG_DETAIL, "PLC is active, calling mb_abort.");
        mb_abort((plc_tag_p)tag);
    } else {
        pdebug(DEBUG_DETAIL, "PLC is terminating or null, skipping mb_abort.");
    }

    /* Release the temporary reference */
    if(plc) {
        plc = rc_dec(plc);
    }

    if(tag->plc) {
        /* unlink the tag from the PLC. */
        int rc = remove_tag(tag->plc, tag);
        if(rc == PLCTAG_STATUS_OK) {
            pdebug(DEBUG_DETAIL, "Tag removed from the PLC successfully.");
        } else if(rc == PLCTAG_ERR_NOT_FOUND) {
            pdebug(DEBUG_WARN, "Tag not found in the PLC's list.");
        } else {
            pdebug(DEBUG_WARN, "Error %s while trying to remove the tag from the PLC's list!", plc_tag_decode_error(rc));
        }

        pdebug(DEBUG_DETAIL, "rc_dec: Releasing the reference to the PLC.");
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


int find_or_create_plc(attr attribs, modbus_plc_p *plc) {
    const char *server = attr_get_str(attribs, "gateway", NULL);
    int server_id = attr_get_int(attribs, "path", -1);
    int connection_group_id = attr_get_int(attribs, "connection_group_id", 0);
    int max_requests_in_flight = attr_get_int(attribs, "max_requests_in_flight", 1);
    int is_new = 0;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    /* clamp maximum requests in flight. */
    if(max_requests_in_flight > MAX_MODBUS_REQUESTS) {
        pdebug(DEBUG_WARN, "max_requests_in_flight set to %d which is higher than the Modbus limit of %d.",
               max_requests_in_flight, MAX_MODBUS_REQUESTS);
        max_requests_in_flight = MAX_MODBUS_REQUESTS;
    }

    if(max_requests_in_flight < 1) {
        pdebug(DEBUG_WARN, "max_requests_in_flight must be between 1 and %d, inclusive, was %d.", MAX_MODBUS_REQUESTS,
               max_requests_in_flight);
        max_requests_in_flight = 1;
    }

    if(server_id < 0 || server_id > 255) {
        pdebug(DEBUG_WARN, "Server ID, %d, out of bounds or missing!", server_id);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* see if we can find a matching server. */
    critical_block(mb_mutex) {
        modbus_plc_p *walker = &plcs;

        while(*walker && ((*walker)->connection_group_id != connection_group_id
              || (*walker)->server_id != (uint8_t)(unsigned int)server_id || str_cmp_i(server, (*walker)->server) != 0)) {

            pdebug(DEBUG_DETAIL, "walking past PLC: connection_group_id=%d, server_id=%d, server=%s", (*walker)->connection_group_id,
                   (*walker)->server_id, (*walker)->server);
                   
            walker = &((*walker)->next);
        }

        pdebug(DEBUG_DETAIL, "Finished walking PLC list walker=%p.", (void *)*walker);
        if(*walker) {
            pdebug(DEBUG_DETAIL, "Found matching PLC: connection_group_id=%d, server_id=%d, server=%s", (*walker)->connection_group_id,
                   (*walker)->server_id, (*walker)->server);
        }

        /* did we find one. */
        if(*walker && (*walker)->connection_group_id == connection_group_id
           && (*walker)->server_id == (uint8_t)(unsigned int)server_id && str_cmp_i(server, (*walker)->server) == 0) {
            pdebug(DEBUG_DETAIL, "Using existing PLC connection.");
            pdebug(DEBUG_DETAIL, "rc_inc: Acquiring Modbus connection reference.");
            *plc = rc_inc(*walker); /* this could result in NULL if the reference count is already zero */
            is_new = 0;
        } 

        /* 
         * this needs to be a separate check because the rc_inc() above may return
         * NULL if the ref count is already zero.
         */
        if(*walker == NULL) {
            /* nope, make a new one.  Do as little as possible in the mutex. */

            pdebug(DEBUG_DETAIL, "Creating new PLC connection.");

            pdebug(DEBUG_DETAIL, "connection_group_id=%d, server_id=%d, server=%s", connection_group_id, server_id, server);

            is_new = 1;

            *plc = (modbus_plc_p)rc_alloc((int)(unsigned int)sizeof(struct modbus_plc_t), modbus_plc_destructor);
            if(*plc) {
                pdebug(DEBUG_DETAIL, "Setting connection_group_id to %d.", connection_group_id);
                (*plc)->connection_group_id = connection_group_id;

                /* copy the server string so that we can find this again. */
                (*plc)->server = str_dup(server);
                if(!((*plc)->server)) {
                    pdebug(DEBUG_WARN, "Unable to allocate Modbus PLC server string!");
                    rc = PLCTAG_ERR_NO_MEM;
                } else {
                    /* make sure we can be found. */
                    (*plc)->server_id = (uint8_t)(unsigned int)server_id;

                    /* other tags could try to add themselves immediately. */

                    /* clear tag list */
                    (*plc)->tag_ring = NULL;

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

                /* The handler thread will hold a reference to the PLC.
                 * Increment the refcount so that when the handler thread exits and calls rc_dec(),
                 * the PLC won't be freed until that happens. The refcount will be decremented when
                 * the handler thread exits. */
                pdebug(DEBUG_DETAIL, "rc_inc: Handler thread acquiring reference to PLC.");
                *plc = rc_inc(*plc);
                if(!*plc) {
                    pdebug(DEBUG_WARN, "Unable to increment PLC reference count!");
                    rc = PLCTAG_ERR_NO_MEM;
                    break;
                }

                rc = thread_create(&((*plc)->handler_thread), modbus_plc_handler, 32768, (void *)(*plc));
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Unable to create new handler thread, error %s!", plc_tag_decode_error(rc));
                    /* Release the reference we just took since thread creation failed */
                    *plc = rc_dec(*plc);
                    break;
                }

                pdebug(DEBUG_DETAIL, "Created thread %p.", (*plc)->handler_thread);

                /* Increment PLC count for lifecycle tracking */
                atomic_add_int32(&plc_count, 1);
                pdebug(DEBUG_DETAIL, "PLC created, count now %d.", atomic_get_int32(&plc_count));
            } while(0);
        }
    }

    if(rc != PLCTAG_STATUS_OK && *plc) {
        pdebug(DEBUG_WARN, "PLC lookup and/or creation failed!");

        /* clean up. */
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing the reference to the PLC.");
        *plc = rc_dec(*plc);
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


/* never enter this from within the handler thread itself! */
void modbus_plc_destructor(void *plc_arg) {
    modbus_plc_p plc = (modbus_plc_p)plc_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!plc) {
        pdebug(DEBUG_WARN, "Destructor called with null pointer!");
        return;
    }

    /* remove the plc from the list. */
    critical_block(mb_mutex) {
        modbus_plc_p *walker = &plcs;

        while(*walker && *walker != plc) { walker = &((*walker)->next); }

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

        /* signal the socket to free the thread. Direct access is safe here since
         * this is the destructor and the PLC is being destroyed anyway. */
        pdebug(DEBUG_DETAIL, "Waking Modbus handler thread %p.", plc->handler_thread);
        if(plc->sock) {
            pdebug(DEBUG_DETAIL, "Waking socket directly.");
            socket_wake(plc->sock);
        }

        /* wait for the thread to terminate and destroy it. */
        thread_join(plc->handler_thread);
        thread_destroy(&plc->handler_thread);

        pdebug(DEBUG_DETAIL, "Modbus handler thread %p destroyed.", plc->handler_thread);

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
    if(plc->tag_ring) { pdebug(DEBUG_WARN, "There are tags still remaining in the tag list, memory leak possible!"); }

    /* Decrement PLC count and signal cleanup when last one is destroyed */
    atomic_add_int32(&plc_count, -1);
    int32_t remaining = atomic_get_int32(&plc_count);
    pdebug(DEBUG_DETAIL, "PLC destroyed, count now %d.", remaining);

    if(remaining == 0 && plc_cleanup_cond) {
        pdebug(DEBUG_INFO, "Last PLC destroyed, signaling cleanup condition.");
        cond_signal(plc_cleanup_cond);
    }

    pdebug(DEBUG_INFO, "Done.");
}

#define UPDATE_ERR_DELAY()                                                                 \
    do {                                                                                   \
        err_delay = err_delay * 2;                                                         \
        if(err_delay > PLC_SOCKET_ERR_MAX_DELAY) { err_delay = PLC_SOCKET_ERR_MAX_DELAY; } \
        err_delay_until = (int64_t)random_u64((uint64_t)err_delay) + time_ms();            \
    } while(0)


THREAD_FUNC(modbus_plc_handler) {
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

    while(!plc->flags.terminate && !atomic_get_bool(&library_terminating)) {
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

                    pdebug(DEBUG_WARN, "MUTEX: Acquire. Closing socket due to connection error.");
                    critical_block(plc->mutex) { socket_destroy(&(plc->sock)); }
                    pdebug(DEBUG_WARN, "MUTEX: Release.Socket connection failed.");

                    /* exponential increase with jitter. */
                    UPDATE_ERR_DELAY();

                    pdebug(DEBUG_WARN,
                           "Unable to connect to the PLC, will retry later! Going to PLC_ERR_WAIT state to wait %" PRId64 "ms.",
                           err_delay);

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

                    pdebug(DEBUG_WARN, "MUTEX: Acquire. Closing socket due to connection error.");
                    critical_block(plc->mutex) { socket_destroy(&(plc->sock)); }
                    pdebug(DEBUG_WARN, "MUTEX: Release. Socket closed due to connection error.");

                    /* exponential increase with jitter. */
                    UPDATE_ERR_DELAY();

                    pdebug(DEBUG_WARN,
                           "Unable to connect to the PLC, will retry later! Going to PLC_ERR_WAIT state to wait %" PRId64 "ms.",
                           err_delay);

                    plc->state = PLC_ERR_WAIT;
                }
                break;

            case PLC_READY:
                pdebug(DEBUG_DETAIL, "in PLC_READY state.");

                /* calculate what events we should be waiting for. */
                waitable_events = SOCK_EVENT_DEFAULT_MASK | SOCK_EVENT_CAN_READ;

                /* if there is a request queued for sending, send it. */
                if(plc->flags.request_ready) { waitable_events |= SOCK_EVENT_CAN_WRITE; }

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

                    pdebug(DEBUG_WARN, "MUTEX: Acquire. Closing socket due to error or disconnect.");
                    critical_block(plc->mutex) { socket_destroy(&(plc->sock)); }
                    pdebug(DEBUG_WARN, "MUTEX: Release. Socket closed due to error or disconnect.");

                    plc->state = PLC_CONNECT_START;
                    break;
                }

                /* preference pushing requests to the PLC */
                if(sock_events & SOCK_EVENT_CAN_WRITE) {
                    if(plc->flags.request_ready) {
                        pdebug(DEBUG_DETAIL,
                               "There is a request ready to send and we can send, going to state PLC_SEND_REQUEST.");
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

                if(sock_events & SOCK_EVENT_TIMEOUT) { pdebug(DEBUG_DETAIL, "Timed out waiting for something to happen."); }

                if(sock_events & SOCK_EVENT_WAKE_UP) { pdebug(DEBUG_DETAIL, "Someone woke us up."); }

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

                    pdebug(DEBUG_WARN, "MUTEX: Acquire. Closing socket due to write error.");
                    critical_block(plc->mutex) { socket_destroy(&(plc->sock)); }
                    pdebug(DEBUG_WARN, "MUTEX: Release. Socket closed due to write error.");

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

                    pdebug(DEBUG_WARN, "MUTEX: Acquire. Closing socket due to read error.");
                    critical_block(plc->mutex) { socket_destroy(&(plc->sock)); }
                    pdebug(DEBUG_WARN, "MUTEX: Release. Socket closed due to read error.");

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
                pdebug(DEBUG_WARN, "MUTEX: Acquire. Closing socket if necessary in PLC_ERR_WAIT state.");
                critical_block(plc->mutex) {
                    if(plc->sock) { socket_destroy(&(plc->sock)); }
                }
                pdebug(DEBUG_WARN, "MUTEX: Release. Socket closed if necessary in PLC_ERR_WAIT state.");
                

                /* wait until done. */
                if(err_delay_until > time_ms()) {
                    pdebug(DEBUG_DETAIL, "Waiting for at least %" PRId64 "ms.", (err_delay_until - time_ms()));
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
        // cond_wait(plc->wait_cond, MODBUS_IDLE_WAIT_TIMEOUT);
    }

    pdebug(DEBUG_INFO, "Handler thread exiting.");

    /* Release the PLC reference held by this thread since rc_alloc.
     * This should only happen AFTER all tags have been removed from the ring
     * (the terminate flag is set when last tag is removed).
     */
    pdebug(DEBUG_DETAIL, "Releasing PLC reference held by handler thread.");
    plc = rc_dec(plc);

    THREAD_RETURN(0);
}


void wake_plc_thread(modbus_plc_p plc) {
    modbus_plc_p plc_ref = NULL;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(plc) {
        /* Take a reference to the PLC to safely access it */
        plc_ref = rc_inc(plc);

        /* Check if PLC is terminating to avoid accessing freed mutex */
        if(plc_ref->flags.terminate) {
            pdebug(DEBUG_DETAIL, "PLC is terminating, skipping wake.");
            plc_ref = rc_dec(plc_ref);
            pdebug(DEBUG_DETAIL, "Done.");
            return;
        }

        pdebug(DEBUG_WARN, "MUTEX: Acquire. Waking PLC thread.");
        critical_block(plc_ref->mutex) {
            pdebug(DEBUG_DETAIL, "Waking PLC thread.");

            if(plc_ref->sock) {
               socket_wake(plc_ref->sock);
            } else {
                pdebug(DEBUG_DETAIL, "PLC socket pointer is NULL.");
            }
        }
        pdebug(DEBUG_WARN, "MUTEX: Release. PLC thread woken.");

        /* Release the reference */
        plc_ref = rc_dec(plc_ref);
    } else {
        pdebug(DEBUG_WARN, "PLC pointer is NULL!");
    }

    pdebug(DEBUG_DETAIL, "Done.");
}


int connect_plc(modbus_plc_p plc) {
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

        pdebug(DEBUG_WARN, "MUTEX: Acquire. Closing socket due to connection error.");
        critical_block(plc->mutex) { socket_destroy(&(plc->sock)); }
        pdebug(DEBUG_WARN, "MUTEX: Release. Socket closed due to connection error.");

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


/** 
 * Get the next tag in the ring.
 * If current_tag is NULL, get the first tag.
 * If there are no tags, return NULL.
 * The returned tag will have its reference count incremented.
 */
static modbus_tag_p get_next_tag(modbus_plc_p plc, modbus_tag_p current_tag) {
    modbus_tag_p next_tag = NULL;

    pdebug(DEBUG_DETAIL, "Starting with tag = %p.", (void *)current_tag);

    if(!plc || !plc->mutex) {
        pdebug(DEBUG_WARN, "PLC is being destroyed or is NULL!");
        return NULL;
    }

    /* loop around the ring to find the first tag that we can get a reference from */
    critical_block(plc->mutex) {
        pdebug(DEBUG_DETAIL, "Getting next tag in ring.");
        next_tag = !current_tag ? plc->tag_ring : (current_tag->next == plc->tag_ring ? NULL : current_tag->next);

        /* Skip tags being destroyed and detect wraparound */
        while(next_tag && !rc_inc(next_tag)) {
            pdebug(DEBUG_DETAIL, "Tag %d reference count is zero, skipping.", next_tag->tag_id);
            next_tag = next_tag->next;

            if(!next_tag || next_tag == plc->tag_ring) {
                pdebug(DEBUG_DETAIL, "Looped around the ring without finding a valid tag.");
                next_tag = NULL;
                break;
            }
        }
    }

    /* Decrement the reference on the current tag now that we're done with it. 
     * This must be done outside the mutex to avoid deadlocks.  This is safe because
     * the only way that current_tag is non-null is if we had previously acquired a reference to it.
     */
    if(current_tag) {
        pdebug(DEBUG_DETAIL, "Decrementing reference on current tag %d.", current_tag->tag_id);
        rc_dec(current_tag);
    }
    pdebug(DEBUG_DETAIL, "Done tag = %p.", (void *)next_tag);

    return next_tag;
}


int tickle_all_tags(modbus_plc_p plc) {
    int rc = PLCTAG_STATUS_OK;
    modbus_tag_p tag = NULL;
    int pending_count = 0;  /* Count of tags that entered PENDING state with read/write response */

    pdebug(DEBUG_DETAIL, "Starting.");


    /*
     * Walk around the ring of tags.
     *
     * Tickle each tag once.
     *
     * Track how many tags transition to waiting for a response.
     * After the iteration completes, rotate the ring head forward by that count
     * to ensure fair-ish processing.
     *
     * Note: get_next_tag() handles releasing the reference on the previous tag
     * while holding the mutex, ensuring no race conditions occur.
     */

    while((tag = get_next_tag(plc, tag)) != NULL) {
        /* at this point we have a valid reference to the tag */

        debug_set_tag_id(tag->tag_id);

        /* the tag mutex may be locked already, so avoid deadlock. */
        if(mutex_try_lock(tag->api_mutex) == PLCTAG_STATUS_OK) {
            rc = tickle_tag(plc, tag);
            if(rc == PLCTAG_STATUS_PENDING) {
                /* for responses, count the tag so we can rotate appropriately later */
                if(tag->op == TAG_OP_READ_RESPONSE || tag->op == TAG_OP_WRITE_RESPONSE) {
                    pdebug(DEBUG_DETAIL, "Tag %d operation %s waiting for response from PLC.", tag->tag_id, op_to_str(tag->op));
                    pending_count++;
                }

                rc = PLCTAG_STATUS_OK;
            } else if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error %s tickling tag! Pushing tag onto idle list.", plc_tag_decode_error(rc));
            }

            // plc_tag_generic_handle_event_callbacks((plc_tag_p)tag);

            mutex_unlock(tag->api_mutex);
        } else {
            pdebug(DEBUG_DETAIL, "Tag API mutex is already taken, skipping tickle.");
        }

        debug_set_tag_id(0);
    }

    /* Rotate ring forward by the number of tags that entered PENDING state.
     * This ensures tags waiting for responses are deprioritized fairly,
     * giving other tags a chance to run.
     */
    if(pending_count > 0) {
        pdebug(DEBUG_DETAIL, "Rotating ring forward by %d positions (pending tag count).", pending_count);
        critical_block(plc->mutex) {
            for(int i = 0; i < pending_count && plc->tag_ring && plc->tag_ring->next != plc->tag_ring; i++) {
                pdebug(DEBUG_DETAIL, "Rotating ring, current head is tag %" PRId32 ".", plc->tag_ring->tag_id);
                plc->tag_ring = plc->tag_ring->next;
            }
            if(plc->tag_ring) {
                pdebug(DEBUG_DETAIL, "Ring rotated, new head is tag %" PRId32 ".", plc->tag_ring->tag_id);
            }
        }
    }

    pdebug(DEBUG_DETAIL, "Done: %s", plc_tag_decode_error(rc));

    return rc;
}


static int tag_op_read_request(modbus_plc_p plc, modbus_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting read request operation for tag %d.", tag->tag_id);

    if(find_request_slot(plc, tag) == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Read request starting in slot %d for tag %d.", tag->request_slot, tag->tag_id);

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

            tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_READ_COMPLETED, (int8_t)rc);

            pdebug(DEBUG_DETAIL, "Read completed event raised for tag %d with status %s.", tag->tag_id, plc_tag_decode_error((int8_t)rc));

            rc = PLCTAG_STATUS_OK;
        }
    } else {
        pdebug(DEBUG_DETAIL, "Request already in flight or PLC not ready, waiting for next chance.");
        rc = PLCTAG_STATUS_PENDING;
    }

    return rc;
}


static int tag_op_read_response(modbus_plc_p plc, modbus_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting read response check operation for tag %d.", tag->tag_id);

    /* cross check the state. */
    if(plc->state == PLC_CONNECT_START || plc->state == PLC_CONNECT_WAIT || plc->state == PLC_ERR_WAIT) {
        pdebug(DEBUG_WARN, "PLC changed state, restarting request.");
        tag->op = TAG_OP_READ_REQUEST;
        return PLCTAG_STATUS_OK;
    }

    if(plc->flags.response_ready) {
        pdebug(DEBUG_DETAIL, "Read response ready for tag %d.", tag->tag_id);

        rc = check_read_response(plc, tag);
        switch(rc) {
            case PLCTAG_ERR_PARTIAL:
                /* partial response, keep going */
                pdebug(DEBUG_DETAIL, "Found our response, but we are not done.");

                /* remove the tag from the request slot. */
                clear_request_slot(plc, tag);

                plc->flags.response_ready = 0;
                tag->op = TAG_OP_READ_REQUEST;

                rc = PLCTAG_STATUS_PENDING;
                break;

            case PLCTAG_ERR_NO_MATCH:
                pdebug(DEBUG_SPEW, "Not our response.");
                rc = PLCTAG_STATUS_PENDING;
                break;

            case PLCTAG_STATUS_OK:
                /* fall through */
            default:
                /* set the status before we might change it. */
                tag->status = (int8_t)rc;

                if(rc == PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_DETAIL, "Found our response.");
                    plc->flags.response_ready = 0;
                } else {
                    pdebug(DEBUG_WARN, "Error %s checking read response!", plc_tag_decode_error(rc));
                    rc = PLCTAG_STATUS_OK;
                }

                /* remove the tag from the request slot. */
                clear_request_slot(plc, tag);

                plc->flags.response_ready = 0;
                tag->op = TAG_OP_IDLE;
                tag->read_in_flight = 0;
                tag->read_complete = 1;
                tag->status = (int8_t)rc;

                tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_READ_COMPLETED, (int8_t)rc);

                pdebug(DEBUG_DETAIL, "Read completed event raised for tag %d with status %s.", tag->tag_id, plc_tag_decode_error((int8_t)rc));

                break;
        }
    } else {
        pdebug(DEBUG_SPEW, "No response yet, Continue waiting.");
        rc = PLCTAG_STATUS_PENDING;
    }

    return rc;
}


static int tag_op_write_request(modbus_plc_p plc, modbus_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    if(find_request_slot(plc, tag) == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Write request starting in slot %d for tag %d.", tag->request_slot, tag->tag_id);

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

            tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_WRITE_COMPLETED, (int8_t)rc);

            pdebug(DEBUG_DETAIL, "Write completed event raised for tag %d.", tag->tag_id);

            rc = PLCTAG_STATUS_OK;
        }
    } else {
        pdebug(DEBUG_SPEW, "Request already in flight or PLC not ready, waiting for next chance.");
        rc = PLCTAG_STATUS_PENDING;
    }

    return rc;
}


static int tag_op_write_response(modbus_plc_p plc, modbus_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    if(plc->state == PLC_CONNECT_START || plc->state == PLC_CONNECT_WAIT || plc->state == PLC_ERR_WAIT) {
        pdebug(DEBUG_WARN, "PLC changed state, restarting request.");
        tag->op = TAG_OP_WRITE_REQUEST;
        return PLCTAG_STATUS_OK;
    }

    if(plc->flags.response_ready) {
        rc = check_write_response(plc, tag);

        switch(rc) {
            case PLCTAG_ERR_PARTIAL:
                /* partial response, keep going */
                pdebug(DEBUG_DETAIL, "Found part of our response, but we are not done.");

                plc->flags.response_ready = 0;
                tag->op = TAG_OP_WRITE_REQUEST;

                rc = PLCTAG_STATUS_PENDING;

                break;
            
            case PLCTAG_ERR_NO_MATCH:
                pdebug(DEBUG_SPEW, "Not our response.");
                rc = PLCTAG_STATUS_PENDING;
                break;

            case PLCTAG_STATUS_OK:
                pdebug(DEBUG_DETAIL, "Tag %d write response ready.", tag->tag_id);
                /* fall through */
            default:
                /* remove the tag from the request slot. */
                clear_request_slot(plc, tag);

                plc->flags.response_ready = 0;
                tag->op = TAG_OP_IDLE;
                tag->write_complete = 1;
                tag->write_in_flight = 0;
                tag->status = (int8_t)rc;

                tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_WRITE_COMPLETED, (int8_t)rc);

                pdebug(DEBUG_DETAIL, "Write completed event raised for tag %d.", tag->tag_id);

                break;
        }
    } else {
        pdebug(DEBUG_SPEW, "No response yet, Continue waiting.");
        rc = PLCTAG_STATUS_PENDING;
    }

    return rc;
}


static int check_tag_abort(modbus_plc_p plc, plc_tag_p base_tag) {
    int rc = PLCTAG_STATUS_OK;
    modbus_tag_p tag = (modbus_tag_p)base_tag;

    /* if an abort is requested */
    if(atomic_get_bool(&tag->abort_requested)) {
        pdebug(DEBUG_DETAIL, "Abort requested for tag.");

        rc = PLCTAG_STATUS_PENDING;

        /* clear the abort request */
        atomic_set_bool(&tag->abort_requested, false);

        /* make sure that this tag is no longer in a request slot. */
        clear_request_slot(plc, tag);

        tag->status = (int8_t)PLCTAG_ERR_ABORT;

        switch(tag->op) {
            case TAG_OP_READ_REQUEST:
            case TAG_OP_READ_RESPONSE:
                tag->read_in_flight = 0;
                tag->read_complete = 1;
                tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_READ_COMPLETED, (int8_t)PLCTAG_ERR_ABORT);
                break;

            case TAG_OP_WRITE_REQUEST:
            case TAG_OP_WRITE_RESPONSE:
                tag->write_in_flight = 0;
                tag->write_complete = 1;
                tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_WRITE_COMPLETED, (int8_t)PLCTAG_ERR_ABORT);
                break;

            default:
                /* nothing to do */
                break;
        }

        /* force to IDLE state */
        tag->op = TAG_OP_IDLE;

        tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_ABORTED, (int8_t)PLCTAG_ERR_ABORT);
    }

    return rc;
}


/**
 * @brief Check if the tag needs to be auto-written.
 *
 * This function checks if the tag is marked for automatic writing based on its
 * configuration and current state.  If the tag is set for auto-write and is dirty,
 * it will determine if the appropriate time has passed to trigger a write operation.
 * 
 * This will supersede any auto-read that may be pending.
 *
 * @param plc The PLC instance.
 * @param base_tag The base tag to check.
 * @return int The status of the operation.
 */
static int check_tag_auto_write(modbus_plc_p plc, plc_tag_p base_tag) {
    modbus_tag_p tag = (modbus_tag_p)base_tag;

    (void)plc;

    /* if auto write is turned on and the tag is dirty */
    if(tag->auto_sync_write_ms > 0 && tag->tag_is_dirty) {
        int64_t now = time_ms();

        /* initialize the next write time if needed */
        if(tag->auto_sync_next_write == 0) {
            tag->auto_sync_next_write = now + tag->auto_sync_write_ms;
        }

        /* if we have passed the wait time */
        if(tag->auto_sync_next_write <= now) {
            /* trigger the write */
            pdebug(DEBUG_DETAIL, "Auto write time reached, requesting auto write.");

            /* clean up state for next time. */
            tag->auto_sync_next_write = 0;
            tag->tag_is_dirty = false;
            tag->op = TAG_OP_WRITE_REQUEST;

            tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_WRITE_STARTED, PLCTAG_STATUS_OK);

            /* we will try to do a write. */
            return PLCTAG_STATUS_PENDING;
        }
    }

    return PLCTAG_STATUS_OK;
}


static int check_tag_auto_read(modbus_plc_p plc, plc_tag_p base_tag) {
    modbus_tag_p tag = (modbus_tag_p)base_tag;

    (void)plc;

    /* if auto read is turned on */
    if(tag->auto_sync_read_ms > 0) {
        int64_t now = time_ms();

        /* make sure that there is not auto write pending */
        if(tag->auto_sync_write_ms > 0 && tag->tag_is_dirty) {
            pdebug(DEBUG_DETAIL, "Auto write is pending, skipping auto read.");
            return PLCTAG_STATUS_OK;
        }

        /* if we have passed the wait time */
        if(tag->auto_sync_next_read <= now) {
            pdebug(DEBUG_DETAIL, "Auto read time reached, requesting auto read.");

            tag->auto_sync_next_read = tag->auto_sync_next_read == 0 ? now : tag->auto_sync_next_read + tag->auto_sync_read_ms;
            tag->op = TAG_OP_READ_REQUEST;

            tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_READ_STARTED, PLCTAG_STATUS_OK);

            /* we will try to do a read. */
            return PLCTAG_STATUS_PENDING;
        }
    }

    return PLCTAG_STATUS_OK;
}

/**
 * @brief tickle the tag to find out if it has work to do.
 * 
 * This will check for aborts, auto-reads, auto-writes, and existing operations.
 * 
 * It will wake the tag if the operation completes.
 * 
 * @param plc 
 * @param tag 
 * @return int - PLCTAG_STATUS_OK if done, PLCTAG_STATUS_PENDING if still working, other
 *    on error.
 */
int tickle_tag(modbus_plc_p plc, modbus_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    tag_op_type_t op = tag->op;
    bool event_raised = false;

    pdebug(DEBUG_DETAIL, "Starting with tag %d.", tag->tag_id);

    pdebug(DEBUG_DETAIL, "Current tag operation is %s.", op_to_str(tag->op));

    /* Check for aborts, auto writes, auto reads */
    if((rc = check_tag_abort(plc, (plc_tag_p)tag)) == PLCTAG_STATUS_PENDING) {
        pdebug(DEBUG_DETAIL, "Tag operation aborted.");
        op = TAG_OP_IDLE;
    } else if((rc = check_tag_auto_write(plc, (plc_tag_p)tag)) == PLCTAG_STATUS_PENDING) {
        pdebug(DEBUG_DETAIL, "Auto write requested.");
        op = TAG_OP_WRITE_REQUEST;
        event_raised = true;
    } else if((rc = check_tag_auto_read(plc, (plc_tag_p)tag)) == PLCTAG_STATUS_PENDING) {
        pdebug(DEBUG_DETAIL, "Auto read requested.");
        op = TAG_OP_READ_REQUEST;
        event_raised = true;
    } else {
        /* maybe some existing operation */
        op = tag->op;
    }

    if(op != tag->op) {
        pdebug(DEBUG_DETAIL, "Tag operation changed from %s to %s.", op_to_str(tag->op), op_to_str(op));
        tag->op = op;
    }

    switch(op) {
        case TAG_OP_IDLE:
            pdebug(DEBUG_SPEW, "Tag is idle.");
            rc = PLCTAG_STATUS_OK;
            break;

        case TAG_OP_READ_REQUEST: rc = tag_op_read_request(plc, tag); break;

        case TAG_OP_READ_RESPONSE: rc = tag_op_read_response(plc, tag); break;

        case TAG_OP_WRITE_REQUEST: rc = tag_op_write_request(plc, tag); break;

        case TAG_OP_WRITE_RESPONSE: rc = tag_op_write_response(plc, tag); break;

        default:
            pdebug(DEBUG_WARN, "Unknown tag operation %d!", op);

            tag->op = TAG_OP_IDLE;
            tag->status = (int8_t)PLCTAG_ERR_NOT_IMPLEMENTED;

            plc_tag_generic_wake_tag((plc_tag_p)tag);

            rc = PLCTAG_STATUS_OK;
            break;
    }

    if(op != tag->op) {
        pdebug(DEBUG_DETAIL, "Tag operation changed from %s to %s.", op_to_str(op), op_to_str(tag->op));
    }

    /* dispatch any events that were raised. */
    plc_tag_generic_handle_event_callbacks((plc_tag_p)tag);

    if(event_raised || tag->write_complete == 1 || tag->read_complete == 1) {
        pdebug(DEBUG_SPEW, "Tag operation complete.", op);
        plc_tag_generic_wake_tag((plc_tag_p)tag);
    }

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}


int find_request_slot(modbus_plc_p plc, modbus_tag_p tag) {
    pdebug(DEBUG_DETAIL, "Starting.");

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
    for(int slot = 0; slot < plc->max_requests_in_flight; slot++) {
        if(plc->tags_with_requests[slot] == 0) {
            pdebug(DEBUG_DETAIL, "Found request slot %d for tag %" PRId32 ".", slot, tag->tag_id);
            plc->tags_with_requests[slot] = tag->tag_id;
            tag->request_slot = slot;
            return PLCTAG_STATUS_OK;
        } else {
            pdebug(DEBUG_DETAIL, "Slot %d is in use by tag %" PRId32 ".", slot, plc->tags_with_requests[slot]);
        }
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_ERR_NO_RESOURCES;
}


void clear_request_slot(modbus_plc_p plc, modbus_tag_p tag) {
    pdebug(DEBUG_DETAIL, "Starting for tag %" PRId32 ".", tag->tag_id);

    if(!plc) {
        pdebug(DEBUG_WARN, "Connection pointer is NULL!");
        return;
    }

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag pointer is NULL!");
        return;
    }

    /* find the tag in the slots. */
    for(int slot = 0; slot < plc->max_requests_in_flight; slot++) {
        if(plc->tags_with_requests[slot] == tag->tag_id) {
            pdebug(DEBUG_DETAIL, "Found tag %" PRId32 " in slot %d.", tag->tag_id, slot);

            if(slot != tag->request_slot) { pdebug(DEBUG_DETAIL, "Tag was not in expected slot %d!", tag->request_slot); }

            plc->tags_with_requests[slot] = 0;
            tag->request_slot = -1;

            /* there might be another tag waiting for a request slot */
            wake_plc_thread(plc);
        }
    }

    pdebug(DEBUG_DETAIL, "Done for tag %" PRId32 ".", tag->tag_id);
}


int receive_response(modbus_plc_p plc) {
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

            pdebug(DEBUG_DETAIL, "Packet header read, data_needed=%d, packet_size=%d, read_data_len=%d", data_needed, packet_size,
                   plc->read_data_len);

            if(data_needed > PLC_READ_DATA_LEN) {
                pdebug(DEBUG_WARN, "Error, packet size, %d, greater than buffer size, %d!", data_needed, PLC_READ_DATA_LEN);
                return PLCTAG_ERR_TOO_LARGE;
            } else if(data_needed < 0) {
                pdebug(DEBUG_WARN, "Read more than a packet!  Expected %d bytes, but got %d bytes!",
                       (MODBUS_MBAP_SIZE + packet_size), plc->read_data_len);
                return PLCTAG_ERR_TOO_LARGE;
            }
        } else {
            data_needed = MODBUS_MBAP_SIZE - plc->read_data_len;
            pdebug(DEBUG_DETAIL, "Still reading packet header, data_needed=%d, read_data_len=%d", data_needed,
                   plc->read_data_len);
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
        pdebug(DEBUG_DETAIL, "Received partial packet of %d bytes of %d.", plc->read_data_len,
               (data_needed + plc->read_data_len));
        rc = PLCTAG_STATUS_PENDING;
    }

    /* if we have some data in the buffer, keep the connection open. */
    if(plc->read_data_len > 0) { plc->inactivity_timeout_ms = MODBUS_INACTIVITY_TIMEOUT + time_ms(); }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int send_request(modbus_plc_p plc) {
    int rc = 1;
    int data_left = plc->write_data_len - plc->write_data_offset;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* check socket, could be closed due to inactivity. */
    if(!plc->sock) {
        pdebug(DEBUG_DETAIL, "No socket or socket is closed.");
        return PLCTAG_ERR_BAD_CONNECTION;
    }

    /* if we have some data in the buffer, keep the connection open. */
    if(plc->write_data_len > 0) { plc->inactivity_timeout_ms = MODBUS_INACTIVITY_TIMEOUT + time_ms(); }

    /* is there anything to do? */
    if(!plc->flags.request_ready) {
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


int create_read_request(modbus_plc_p plc, modbus_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    uint16_t seq_id = (++(plc->seq_id) ? plc->seq_id : ++(plc->seq_id));  // disallow zero
    int registers_per_request = (MAX_MODBUS_RESPONSE_PAYLOAD * 8) / tag->elem_size;
    int base_register = tag->reg_base + (tag->request_num * registers_per_request);
    int register_count = tag->elem_count - (tag->request_num * registers_per_request);

    pdebug(DEBUG_DETAIL, "Starting.");

    pdebug(DEBUG_DETAIL, "seq_id=%d", seq_id);
    pdebug(DEBUG_DETAIL, "registers_per_request = %d", registers_per_request);
    pdebug(DEBUG_DETAIL, "base_register = %d", base_register);
    pdebug(DEBUG_DETAIL, "register_count = %d", register_count);

    /* clamp the number of registers we ask for to what will fit. */
    if(register_count > registers_per_request) { register_count = registers_per_request; }

    pdebug(DEBUG_INFO, "preparing read request for %d registers (of %d total) from base register %d.", register_count,
           tag->elem_count, base_register);

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
    plc->write_data[plc->write_data_len] = (uint8_t)((seq_id >> 8) & 0xFF);
    plc->write_data_len++;
    plc->write_data[plc->write_data_len] = (uint8_t)((seq_id >> 0) & 0xFF);
    plc->write_data_len++;

    /* protocol version is always zero */
    plc->write_data[plc->write_data_len] = 0;
    plc->write_data_len++;
    plc->write_data[plc->write_data_len] = 0;
    plc->write_data_len++;

    /* request packet length */
    plc->write_data[plc->write_data_len] = 0;
    plc->write_data_len++;
    plc->write_data[plc->write_data_len] = 6;
    plc->write_data_len++;

    /* device address */
    plc->write_data[plc->write_data_len] = plc->server_id;
    plc->write_data_len++;

    /* function code depends on the register type. */
    switch(tag->reg_type) {
        case MB_REG_COIL:
            plc->write_data[7] = MB_CMD_READ_COIL_MULTI;
            plc->write_data_len++;
            break;

        case MB_REG_DISCRETE_INPUT:
            plc->write_data[7] = MB_CMD_READ_DISCRETE_INPUT_MULTI;
            plc->write_data_len++;
            break;

        case MB_REG_HOLDING_REGISTER:
            plc->write_data[7] = MB_CMD_READ_HOLDING_REGISTER_MULTI;
            plc->write_data_len++;
            break;

        case MB_REG_INPUT_REGISTER:
            plc->write_data[7] = MB_CMD_READ_INPUT_REGISTER_MULTI;
            plc->write_data_len++;
            break;

        default:
            pdebug(DEBUG_WARN, "Unsupported register type %d!", tag->reg_type);
            return PLCTAG_ERR_UNSUPPORTED;
            break;
    }

    /* register base. */
    plc->write_data[plc->write_data_len] = (uint8_t)((base_register >> 8) & 0xFF);
    plc->write_data_len++;
    plc->write_data[plc->write_data_len] = (uint8_t)((base_register >> 0) & 0xFF);
    plc->write_data_len++;

    /* number of elements to read. */
    plc->write_data[plc->write_data_len] = (uint8_t)((register_count >> 8) & 0xFF);
    plc->write_data_len++;
    plc->write_data[plc->write_data_len] = (uint8_t)((register_count >> 0) & 0xFF);
    plc->write_data_len++;

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


int check_read_response(modbus_plc_p plc, modbus_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    uint16_t seq_id = (uint16_t)((uint16_t)plc->read_data[1] + (uint16_t)(plc->read_data[0] << 8));
    int partial_read = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(seq_id == tag->seq_id) {
        uint8_t has_error = plc->read_data[7] & (uint8_t)0x80;

        /* the operation is complete regardless of the outcome. */
        // tag->flags.operation_complete = 1;

        if(has_error) {
            rc = translate_modbus_error(plc->read_data[8]);

            pdebug(DEBUG_WARN, "Got read response %ud, with error %s, of length %d.", (int)(unsigned int)seq_id,
                   plc_tag_decode_error(rc), plc->read_data_len);
        } else {
            int registers_per_request = (MAX_MODBUS_RESPONSE_PAYLOAD * 8) / tag->elem_size;
            int register_offset = (tag->request_num * registers_per_request);
            int byte_offset = (register_offset * tag->elem_size) / 8;
            uint8_t payload_size = plc->read_data[8];
            int copy_size = ((tag->size - byte_offset) < payload_size ? (tag->size - byte_offset) : payload_size);

            /* no error. So copy the data. */
            pdebug(DEBUG_DETAIL, "Got read response %u of length %d with payload of size %d.", (int)(unsigned int)seq_id,
                   plc->read_data_len, payload_size);
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

int create_write_request(modbus_plc_p plc, modbus_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    uint16_t seq_id = (++(plc->seq_id) ? plc->seq_id : ++(plc->seq_id));  // disallow zero
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
    if(register_count > registers_per_request) { register_count = registers_per_request; }

    /* how many bytes, rounded up to the nearest byte. */
    request_payload_size = ((register_count * tag->elem_size) + 7) / 8;

    pdebug(DEBUG_DETAIL,
           "preparing write request for %d registers (of %d total) from base register %d of payload size %d in bytes.",
           register_count, tag->elem_count, base_register, request_payload_size);

    /* FIXME - remove this when we figure out how to push multiple requests. */
    plc->write_data_len = 0;

    /* build the request sequence ID */
    plc->write_data[plc->write_data_len] = (uint8_t)((seq_id >> 8) & 0xFF);
    plc->write_data_len++;
    plc->write_data[plc->write_data_len] = (uint8_t)((seq_id >> 0) & 0xFF);
    plc->write_data_len++;

    /* protocol version is always zero */
    plc->write_data[plc->write_data_len] = 0;
    plc->write_data_len++;
    plc->write_data[plc->write_data_len] = 0;
    plc->write_data_len++;

    /* request packet length */
    plc->write_data[plc->write_data_len] = (uint8_t)(((request_payload_size + 7) >> 8) & 0xFF);
    plc->write_data_len++;
    plc->write_data[plc->write_data_len] = (uint8_t)(((request_payload_size + 7) >> 0) & 0xFF);
    plc->write_data_len++;

    /* device address */
    plc->write_data[plc->write_data_len] = plc->server_id;
    plc->write_data_len++;

    /* function code depends on the register type. */
    switch(tag->reg_type) {
        case MB_REG_COIL:
            plc->write_data[7] = MB_CMD_WRITE_COIL_MULTI;
            plc->write_data_len++;
            break;

        case MB_REG_DISCRETE_INPUT:
            pdebug(DEBUG_WARN, "Done. You cannot write a discrete input!");
            return PLCTAG_ERR_UNSUPPORTED;
            break;

        case MB_REG_HOLDING_REGISTER:
            plc->write_data[7] = MB_CMD_WRITE_HOLDING_REGISTER_MULTI;
            plc->write_data_len++;
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
    plc->write_data[plc->write_data_len] = (uint8_t)((base_register >> 8) & 0xFF);
    plc->write_data_len++;
    plc->write_data[plc->write_data_len] = (uint8_t)((base_register >> 0) & 0xFF);
    plc->write_data_len++;

    /* number of elements to read. */
    plc->write_data[plc->write_data_len] = (uint8_t)((register_count >> 8) & 0xFF);
    plc->write_data_len++;
    plc->write_data[plc->write_data_len] = (uint8_t)((register_count >> 0) & 0xFF);
    plc->write_data_len++;

    /* number of bytes of data to write. */
    plc->write_data[plc->write_data_len] = (uint8_t)(unsigned int)(request_payload_size);
    plc->write_data_len++;

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

int check_write_response(modbus_plc_p plc, modbus_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    uint16_t seq_id = (uint16_t)((uint16_t)plc->read_data[1] + (uint16_t)(plc->read_data[0] << 8));
    int partial_write = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    if(seq_id == tag->seq_id) {
        uint8_t has_error = plc->read_data[7] & (uint8_t)0x80;

        /* this is our response, so the operation is complete regardless of the status. */
        // tag->flags.operation_complete = 1;

        if(has_error) {
            rc = translate_modbus_error(plc->read_data[8]);

            pdebug(DEBUG_WARN, "Got write response %ud, with error %s, of length %d.", (int)(unsigned int)seq_id,
                   plc_tag_decode_error(rc), plc->read_data_len);
        } else {
            int registers_per_request = (MAX_MODBUS_RESPONSE_PAYLOAD * 8) / tag->elem_size;
            int next_register_offset = ((tag->request_num + 1) * registers_per_request);
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


int translate_modbus_error(uint8_t err_code) {
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


int parse_register_name(attr attribs, modbus_reg_type_t *reg_type, int *reg_base) {
    int rc = PLCTAG_STATUS_OK;
    const char *reg_name = attr_get_str(attribs, "name", NULL);

    pdebug(DEBUG_INFO, "Starting.");

    if(!reg_name || str_length(reg_name) < 3) {
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

static void debug_ring(modbus_plc_p plc) {
    critical_block(plc->mutex) {
        modbus_tag_p cur = plc->tag_ring;

        pdebug(DEBUG_DETAIL, "Dumping tag ring:");

        if(!cur) {
            pdebug(DEBUG_DETAIL, "  (empty)");
            break;
        } 

        do {
            pdebug(DEBUG_DETAIL, "  Tag ID %" PRId32 ", %p.", cur->tag_id, cur);
            cur = cur->next;
        } while(cur && cur != plc->tag_ring);

        if(!cur) {
            pdebug(DEBUG_WARN, "  BROKEN RING DETECTED!");
        } else {
            pdebug(DEBUG_DETAIL, "End of ring.");
        }
    }
}


int add_tag(modbus_plc_p plc, modbus_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting to add tag %" PRIu32 ".", (uint32_t)tag->tag_id);

    pdebug(DEBUG_DETAIL, "Current ring before adding:");
    debug_ring(plc);

    critical_block(plc->mutex) {
        if(plc->tag_ring == NULL) {
            plc->tag_ring = tag;

            /* ring of one */
            tag->next = tag;
        } else {
            /* add to the ring. */
            tag->next = plc->tag_ring->next;
            plc->tag_ring->next = tag;
        }

        /* Increment tag count */
        atomic_add_int32(&plc->tag_count, 1);
    }

    pdebug(DEBUG_DETAIL, "New ring after adding:");
    debug_ring(plc);

    pdebug(DEBUG_DETAIL, "Tag added, count now %d.", atomic_get_int32(&plc->tag_count));
    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}

int remove_tag(modbus_plc_p plc, modbus_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting to remove tag %" PRIu32 ".", (uint32_t)tag->tag_id);

    pdebug(DEBUG_DETAIL, "Current ring before removing:");
    debug_ring(plc);

    critical_block(plc->mutex) {
        modbus_tag_p cur = plc->tag_ring;

        /* walk around the ring to find the _previous_ tag entry that points to this tag */
        while(cur && cur->next != tag && cur->next != plc->tag_ring) {
            pdebug(DEBUG_DETAIL, "Walking ring, at tag ID %u.", (unsigned int)cur->tag_id);
            cur = cur->next;
        }

        /* if we found it, then cur->next should be the tag we want to remove. */
        if(cur->next == tag) {
            pdebug(DEBUG_DETAIL, "Tag found, removing from ring.");

            cur->next = tag->next;

            /* are we removing the ring head? */
            if(plc->tag_ring == tag) {
                pdebug(DEBUG_DETAIL, "Removing ring head, updating ring head pointer.");
                plc->tag_ring = tag->next;

                /* if the ring was only one tag, then */
                if(plc->tag_ring == tag) {
                    /* only one tag in the ring. */
                    pdebug(DEBUG_DETAIL, "Ring is now empty.");
                    plc->tag_ring = NULL;
                }
            }

            /* Decrement tag count */
            atomic_add_int32(&plc->tag_count, -1);
            int32_t remaining = atomic_get_int32(&plc->tag_count);
            pdebug(DEBUG_DETAIL, "Tag removed, count now %d.", remaining);

            /* If no more tags, signal handler thread to terminate */
            if(remaining == 0) {
                pdebug(DEBUG_INFO, "Last tag removed from PLC, signaling handler thread to exit.");
                plc->flags.terminate = 1;
            }
        } else {
            /* not found */
            pdebug(DEBUG_INFO, "Tag not found in ring.");
            rc = PLCTAG_ERR_NOT_FOUND;
        }
    }

    pdebug(DEBUG_DETAIL, "New ring after removing:");
    debug_ring(plc);

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


/****** Tag Control Functions ******/

/* These must all be called with the API mutex on the tag held. */

int mb_abort(plc_tag_p p_tag) {
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

    /* Only access PLC if it hasn't been terminated */
    if(tag->plc && !tag->plc->flags.terminate) {
        clear_request_slot(tag->plc, tag);

        /* wake the PLC loop if we need to. */
        wake_plc_thread(tag->plc);
    } else {
        pdebug(DEBUG_DETAIL, "PLC is terminating or null, skipping abort operations.");
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


int mb_read_start(plc_tag_p p_tag) {
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


int mb_tag_status(plc_tag_p p_tag) {
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
int mb_tickler(plc_tag_p p_tag) {
    (void)p_tag;

    return PLCTAG_STATUS_OK;
}


int mb_write_start(plc_tag_p p_tag) {
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


int mb_wake_plc(plc_tag_p p_tag) {
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

int mb_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value) {
    int res = default_value;
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    tag->status = PLCTAG_STATUS_OK;

    /* match the attribute. */
    if(str_cmp_i(attrib_name, "elem_size") == 0) {
        res = (tag->elem_size + 7) / 8; /* return size in bytes! */
    } else if(str_cmp_i(attrib_name, "elem_count") == 0) {
        res = tag->elem_count;
    } else {
        pdebug(DEBUG_WARN, "Attribute \"%s\" is not supported.", attrib_name);
        tag->status = PLCTAG_ERR_UNSUPPORTED;
    }

    return res;
}


int mb_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value) {
    (void)attrib_name;
    (void)new_value;

    pdebug(DEBUG_WARN, "Attribute \"%s\" is unsupported!", attrib_name);

    raw_tag->status = PLCTAG_ERR_UNSUPPORTED;

    return PLCTAG_ERR_UNSUPPORTED;
}


/****** Library level functions. *******/

void mb_teardown(void) {
    pdebug(DEBUG_INFO, "Starting.");

    if(mb_mutex) {
        pdebug(DEBUG_DETAIL, "Signaling all Modbus PLCs to terminate.");

        /* Signal all PLC handler threads to terminate */
        critical_block(mb_mutex) {
            modbus_plc_p walker = plcs;
            while(walker) {
                pdebug(DEBUG_DETAIL, "Signaling PLC to terminate.");
                walker->flags.terminate = 1;
                wake_plc_thread(walker);
                walker = walker->next;
            }
        }

        pdebug(DEBUG_DETAIL, "Waiting for all Modbus PLCs to be destroyed.");

        /* Wait for all PLCs to be destroyed using condition variable */
        while(atomic_get_int32(&plc_count) > 0) {
            pdebug(DEBUG_DETAIL, "Waiting for %d PLC(s) to be destroyed.", atomic_get_int32(&plc_count));

            /* Wait for signal with timeout */
            int wait_rc = cond_wait(plc_cleanup_cond, 5000);  /* 5 second timeout */
            if(wait_rc == PLCTAG_ERR_TIMEOUT) {
                pdebug(DEBUG_WARN, "Timeout waiting for PLCs to be destroyed!");
                break;
            }
        }

        pdebug(DEBUG_DETAIL, "All Modbus PLCs destroyed.");
    }

    if(mb_mutex) {
        pdebug(DEBUG_DETAIL, "Destroying Modbus mutex.");
        mutex_destroy(&mb_mutex);
        mb_mutex = NULL;
    }
    pdebug(DEBUG_DETAIL, "Modbus mutex destroyed.");

    if(plc_cleanup_cond) {
        pdebug(DEBUG_DETAIL, "Destroying cleanup condition variable.");
        cond_destroy(&plc_cleanup_cond);
        plc_cleanup_cond = NULL;
    }
    pdebug(DEBUG_DETAIL, "Cleanup condition variable destroyed.");

    pdebug(DEBUG_INFO, "Done.");
}


int mb_init(void) {
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

    pdebug(DEBUG_DETAIL, "Setting up cleanup condition variable.");
    if(!plc_cleanup_cond) {
        rc = cond_create(&plc_cleanup_cond);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error %s creating cleanup condition!", plc_tag_decode_error(rc));
            return rc;
        }
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}

static const char *op_to_str(tag_op_type_t op) {
    switch(op) {
        case TAG_OP_IDLE: return "IDLE"; break;
        case TAG_OP_READ_REQUEST: return "READ_REQUEST"; break;
        case TAG_OP_READ_RESPONSE: return "READ_RESPONSE"; break;
        case TAG_OP_WRITE_REQUEST: return "WRITE_REQUEST"; break;
        case TAG_OP_WRITE_RESPONSE: return "WRITE_RESPONSE"; break;
        default: return "UNKNOWN_OP"; break;
    }
}

