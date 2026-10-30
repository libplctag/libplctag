/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
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
#include <libplctag/api/libplctag.h>
#include <libplctag/lib/conn_attribs.h>
#include <libplctag/lib/conn_timeouts.h>
#include <libplctag/lib/conn_watch.h>
#include <libplctag/lib/connection_tag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/lib/tag_driver.h>
#include <libplctag/modules/modbus/modbus.h>
#include <limits.h>
#include <platform.h>
#include <stdlib.h>
#include <utils/atomic_utils.h>
#include <utils/attr.h>
#include <utils/backoff.h>
#include <utils/debug.h>
#include <utils/rc.h>
#include <utils/vector.h>

/* data definitions */

/*
 * Connection retry backoff bounds.  The growth and jitter are backoff_wait_ms()'s, so
 * these are only the ends of the range.
 */
#define PLC_SOCKET_ERR_MAX_DELAY (5000)
#define PLC_SOCKET_ERR_START_DELAY (50)
#define PLC_SOCKET_ERR_DELAY_WAIT_INCREMENT (10)
#define MODBUS_DEFAULT_PORT (502)
#define PLC_READ_DATA_LEN (300)
#define PLC_WRITE_DATA_LEN (300)
#define MODBUS_MBAP_SIZE (6)
/* smallest response we can act on: the MBAP header, plus unit id, function code, and one
 * byte-count/exception-code byte. check_read_response()/check_write_response() read up to
 * this offset unconditionally. */
#define MODBUS_MIN_RESPONSE_SIZE (MODBUS_MBAP_SIZE + 3)
/* Modbus registers are addressed with 16 bits, so this is the most that can exist. */
#define MAX_MODBUS_ELEM_COUNT (65536)
#define MAX_MODBUS_REQUEST_PAYLOAD (246)
#define MAX_MODBUS_RESPONSE_PAYLOAD (250)
#define MAX_MODBUS_PDU_PAYLOAD (253) /* everything after the server address */
#define SOCKET_READ_TIMEOUT (20)       /* read timeout in milliseconds */
#define SOCKET_WRITE_TIMEOUT (20)      /* write timeout in milliseconds */
#define SOCKET_CONNECT_TIMEOUT (20)    /* connect timeout step in milliseconds */
#define MODBUS_IDLE_WAIT_TIMEOUT (100) /* idle wait timeout in milliseconds */
#define MAX_MODBUS_REQUESTS (16)       /* per the Modbus specification */

/* Microsecond timing for performance analysis */

typedef struct modbus_tag_t *modbus_tag_p;
typedef struct modbus_tag_list_t *modbus_tag_list_p;


typedef enum {
    PLC_CONNECT_START = 0,
    PLC_CONNECT_WAIT,
    PLC_READY,
    PLC_BUILD_REQUEST,
    PLC_SEND_REQUEST,
    PLC_RECEIVE_RESPONSE,
    PLC_IDLE_WAIT,
    PLC_ERR_WAIT,

    /*
     * Every path that abandons a socket goes through here, so the link going
     * down is reported exactly once and from one place, as AB and Omron do
     * with SESSION_CLOSE_SOCKET.  The entering state sets state_after_close.
     */
    PLC_CLOSE_SOCKET
} modbus_plc_state_t;


struct modbus_plc_t {
    struct modbus_plc_t *next;

    /* the tags that need the handler; see lib/tag_driver.h. */
    tag_driver_t driver;

    /* Count of tags currently attached to this PLC */
    atomic_int32_t tag_count;

    /* connection status and event ring - what connection tags observe */
    conn_event_ring_t watch;
    /* Timestamp tracking for inactivity detection */
    int64_t last_packet_time_ms;
    int64_t disconnect_at_time_ms; /* Calculated deadline for disconnection based on inactivity timeout */
    int32_t cached_inactivity_timeout_ms; /* Cached value for detecting timeout changes */

    /* hostname/ip and possibly port of the server. */
    char *server;
    sock_p sock;
    uint8_t server_id;
    int connection_group_id;

    /* State */

    /* Each was a 1-bit field packed into a shared storage unit, read/written from
     * different lock domains (the PLC's own mutex vs. no lock at all in
     * wake_plc_thread()) -- a write to one bit could race a concurrent read of a
     * sibling bit sharing the same byte. Individual atomics have no such sharing. */
    struct {
        atomic_bool terminate;
        atomic_bool response_ready;
        atomic_bool request_ready;
        // unsigned int request_in_flight:1;
    } flags;
    uint16_t seq_id;

    /* thread related state */
    thread_p handler_thread;
    mutex_p mutex;

    modbus_plc_state_t state;
    int max_requests_in_flight;

    /*
     * Requests on the wire, matched to replies by transaction id.  A slot whose tag was aborted
     * has tag NULL and stays used until its reply arrives or the socket closes.
     */
    struct {
        modbus_tag_p tag;
        uint16_t tid;
        bool used;
    } slots[MAX_MODBUS_REQUESTS];
    int32_t slots_used;
    int32_t lock_retries; /* handler thread only; see service_tags() */

    /* comms timeout/disconnect - readable/writable by tags via atomics */
    atomic_int32_t connection_inactivity_timeout_ms; /* milliseconds */
    atomic_int32_t connection_retry_max_delay_ms;    /* milliseconds */

    /* data */
    int read_data_len;
    uint8_t read_data[PLC_READ_DATA_LEN];

    int write_data_len;
    int write_data_offset;
    uint8_t write_data[PLC_WRITE_DATA_LEN];
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




struct modbus_tag_t {
    /* base tag parts. */
    TAG_BASE_STRUCT;

    /* register type. */
    modbus_reg_type_t reg_type;
    uint16_t reg_base;

    /* the PLC we are using */
    modbus_plc_p plc;

    uint16_t request_num;
    uint16_t seq_id;

    /*
     * Function code of the request in flight.  A server answering a holding-register read with
     * a coil response hands back bit-packed data that we would copy into the tag as words, so
     * the reply has to name the same function we asked for.
     */
    uint8_t req_fn_code;
};


/* Connection tag (@connection) — monitors Modbus TCP connection state */


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

                                          .write_pad_alignment_bytes = 0,
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

/* Track active handler threads for proper shutdown synchronization */
static atomic_int32_t handler_threads_active = ATOMIC_INT_STATIC_INIT;


/* device tag functions */
static plc_tag_p mb_connection_tag_create(attr attribs,
                                          void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                          void *userdata, plc_tag_p src_tag);
static void mb_plc_set_conn_status(modbus_plc_p plc, int32_t new_status);

/* helper functions */
static int create_tag_object(attr attribs, modbus_tag_p *tag);
static int find_or_create_plc(attr attribs, modbus_plc_p *plc, bool *out_is_new);
static int parse_register_name(attr attribs, modbus_reg_type_t *reg_type, int *reg_base);
static void modbus_tag_destructor(void *tag_arg);
static void modbus_plc_destructor(void *plc_arg);
static THREAD_FUNC(modbus_plc_handler);
static void wake_plc_thread(modbus_plc_p plc);
static int connect_plc(modbus_plc_p plc);
static int64_t service_tags(modbus_plc_p plc);
static void dispatch_reply_unsafe(modbus_plc_p plc);
static bool send_next_unsafe(modbus_plc_p plc, int64_t now);
static int32_t find_slot_unsafe(modbus_plc_p plc, modbus_tag_p tag);
static void forget_tag_unsafe(modbus_plc_p plc, modbus_tag_p tag);
static void queue_tag(modbus_tag_p tag);
static int receive_response(modbus_plc_p plc);
static int send_request(modbus_plc_p plc);
static int check_read_response(modbus_plc_p plc, modbus_tag_p tag);
static int create_read_request(modbus_plc_p plc, modbus_tag_p tag);
static int check_write_response(modbus_plc_p plc, modbus_tag_p tag);
static int create_write_request(modbus_plc_p plc, modbus_tag_p tag);
static int translate_modbus_error(uint8_t err_code);

/* tag list functions */
static int add_tag(modbus_plc_p plc, modbus_tag_p tag);
static int remove_tag(modbus_plc_p plc, modbus_tag_p tag);

/* tag vtable functions. */

/* control functions. */
static int mb_abort(plc_tag_p p_tag);
static int mb_activate(plc_tag_p p_tag);
static int mb_queue(plc_tag_p p_tag);
static int mb_submit(plc_tag_p p_tag);
static int mb_tag_data_written(plc_tag_p p_tag);
static int mb_tag_status(plc_tag_p p_tag);


/* data accessors */
static uint16_t next_seq_id(uint16_t current);
static struct tag_vtable_t modbus_vtable;


/****** main entry point *******/

int mb_tag_create(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                  void *userdata, plc_tag_p src_tag, plc_tag_p *tag_out) {
    int rc = PLCTAG_STATUS_OK;
    modbus_tag_p tag = NULL;

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "Starting.");

    if(str_cmp(attr_get_str(attribs, "name", ""), "@connection") == 0) {
        *tag_out = mb_connection_tag_create(attribs, tag_callback_func, userdata, src_tag);

        /* the connection tag types still report through the tag. */
        return *tag_out ? (int)(*tag_out)->status : PLCTAG_ERR_CREATE;
    }

    /* create the tag object. */
    rc = create_tag_object(attribs, &tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Unable to create new tag!  Error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    /* set up the generic parts. */
    rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Unable to initialize generic tag parts!");
        rc_dec(tag);
        return rc;
    }

    tag->protocol_type = TAG_PROTOCOL_MODBUS;

    /* find the PLC object. */
    if(src_tag) {
        switch(src_tag->protocol_type) {
            case TAG_PROTOCOL_MODBUS: {
                modbus_tag_p src = (modbus_tag_p)src_tag;
                tag->plc = rc_inc(src->plc);
                break;
            }

            case TAG_PROTOCOL_MB_CONNECTION: {
                tag->plc = rc_inc(((connection_tag_p)src_tag)->conn);
                break;
            }

            default: tag->plc = NULL; break;
        }

        rc = tag->plc ? PLCTAG_STATUS_OK : PLCTAG_ERR_NOT_FOUND;
    } else {
        rc = find_or_create_plc(attribs, &(tag->plc), NULL);
    }
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Unable to create new tag!  Error %s!", plc_tag_decode_error(rc));
        *tag_out = (plc_tag_p)tag;
        return rc;
    }

    /* Do NOT put the tag on the PLC's list or kick off a read here: the generic
     * layer (plc_tag_create_impl) still has several fields left to initialize on
     * this tag object. Publishing now would let the PLC's background handler
     * thread observe a half-initialized tag. plc_tag_create_impl calls
     * tag->vtable->activate() (mb_activate() below) once setup is complete. */

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "Done.");

    *tag_out = (plc_tag_p)tag;

    return PLCTAG_STATUS_OK;
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
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Element count should not be a negative value!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * elem_count comes from the attribute string, so cap it.  Modbus registers are
     * addressed with 16 bits, so more than 65536 of them is meaningless, and without
     * this the elem_count * reg_size below can overflow or ask for a huge allocation.
     */
    if(elem_count > MAX_MODBUS_ELEM_COUNT) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Element count %d is larger than the maximum of %d!", elem_count,
               MAX_MODBUS_ELEM_COUNT);
        return PLCTAG_ERR_TOO_LARGE;
    }

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "Starting.");

    *tag = NULL;

    /* get register type. */
    rc = parse_register_name(attribs, &reg_type, &reg_base);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Error parsing base register name!");
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
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Unsupported register type!");
            return PLCTAG_ERR_BAD_PARAM;
            break;
    }

    /* calculate the data size in bytes. */
    data_size = ((elem_count * reg_size) + 7) / 8;

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Tag data size is %d bytes.", data_size);

    /* allocate the tag */
    *tag = (modbus_tag_p)rc_alloc((int)(unsigned int)sizeof(struct modbus_tag_t) + data_size, modbus_tag_destructor);
    if(!*tag) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Unable to allocate Modbus tag!");
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
    tag_op_set((*tag), TAG_OP_IDLE);

    /* initialize the pending transaction ID */

    /* initialize the operation changed time for fairness sorting */

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


void modbus_tag_destructor(void *tag_arg) {
    modbus_tag_p tag = (modbus_tag_p)tag_arg;
    modbus_plc_p plc = NULL;

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, tag->tag_id, "Starting.");

    if(!tag) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, tag->tag_id, "Destructor called with null pointer!");
        return;
    }

    /* Take a reference to the PLC to safely check its state */
    if(tag->plc) { plc = rc_inc(tag->plc); }

    /* abort everything, but only if PLC is still valid and not terminating */
    if(plc && !atomic_get_bool(&plc->flags.terminate)) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "PLC is active, calling mb_abort.");
        mb_abort((plc_tag_p)tag);
    } else {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "PLC is terminating or null, skipping mb_abort.");
    }

    /* Release the temporary reference */
    if(plc) { plc = rc_dec(plc); }

    if(tag->plc) {
        /* unlink the tag from the PLC. */
        int rc = remove_tag(tag->plc, tag);
        if(rc == PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "Tag removed from the PLC successfully.");
        } else if(rc == PLCTAG_ERR_NOT_FOUND) {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, tag->tag_id, "Tag not found in the PLC's list.");
        } else {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, tag->tag_id, "Error %s while trying to remove the tag from the PLC's list!",
                   plc_tag_decode_error(rc));
        }

        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "rc_dec: Releasing the reference to the PLC.");
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

    if(tag->instance) {
        rc_dec(tag->instance);
        tag->instance = NULL;
    }

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, tag->tag_id, "Done.");
}


int find_or_create_plc(attr attribs, modbus_plc_p *plc, bool *out_is_new) {
    const char *server = attr_get_str(attribs, "gateway", NULL);
    int server_id = attr_get_int(attribs, "path", -1);
    int connection_group_id = attr_get_int(attribs, "connection_group_id", 0);
    int max_requests_in_flight = attr_get_int(attribs, "max_requests_in_flight", 1);
    int32_t connection_inactivity_timeout_ms = CONN_INACTIVITY_TIMEOUT_MAX_MS;
    int32_t connection_retry_max_delay_ms = PLC_SOCKET_ERR_MAX_DELAY;
    int is_new = 0;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "Starting.");

    /* clamp maximum requests in flight. */
    {
        int32_t requested_in_flight = (int32_t)max_requests_in_flight;

        if(attr_clamp_int32(&requested_in_flight, 1, MAX_MODBUS_REQUESTS)) {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0,
                   "max_requests_in_flight must be between 1 and %d, inclusive, was %d.  Using %" PRId32 ".", MAX_MODBUS_REQUESTS,
                   max_requests_in_flight, requested_in_flight);
        }

        max_requests_in_flight = (int)requested_in_flight;
    }

    connection_inactivity_timeout_ms = attr_get_int(attribs, "connection_inactivity_timeout_ms", CONN_INACTIVITY_TIMEOUT_MAX_MS);
    conn_clamp_ms_attrib(&connection_inactivity_timeout_ms, CONN_INACTIVITY_TIMEOUT_MIN_MS, CONN_INACTIVITY_TIMEOUT_MAX_MS,
                         "connection_inactivity_timeout_ms", 0, DEBUG_MODULE_MODBUS);

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Setting connection_inactivity_timeout_ms to %" PRId32 "ms.",
           connection_inactivity_timeout_ms);

    connection_retry_max_delay_ms = attr_get_int(attribs, "connection_retry_max_delay_ms", PLC_SOCKET_ERR_MAX_DELAY);
    conn_clamp_ms_attrib(&connection_retry_max_delay_ms, CONN_RETRY_MAX_DELAY_MIN_MS, CONN_RETRY_MAX_DELAY_LIMIT_MS,
                         "connection_retry_max_delay_ms", 0, DEBUG_MODULE_MODBUS);

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Setting connection_retry_max_delay_ms to %" PRId32 "ms.",
           connection_retry_max_delay_ms);

    if(server_id < 0 || server_id > 255) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Server ID, %d, out of bounds or missing!", server_id);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* see if we can find a matching server. */
    critical_block(mb_mutex) {
        /* Keep searching until we find a valid PLC or run out of existing plc objects */
        modbus_plc_p *walker = &plcs;

        /* we have to match and the reference count must be > 0 */
        while(*walker
              && ((*walker)->connection_group_id != connection_group_id
                  || (*walker)->server_id != (uint8_t)(unsigned int)server_id || str_cmp_i(server, (*walker)->server) != 0
                  || !rc_inc(*walker))) {

            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "walking past PLC: connection_group_id=%d, server_id=%d, server=%s",
                   (*walker)->connection_group_id, (*walker)->server_id, (*walker)->server);

            walker = &((*walker)->next);
        }

        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Finished walking PLC list walker=%p.", (void *)*walker);

        if(*walker) {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Found matching PLC: connection_group_id=%d, server_id=%d, server=%s",
                   (*walker)->connection_group_id, (*walker)->server_id, (*walker)->server);

            /* we have taken a reference above when walking the list */

            is_new = 0;
            rc = PLCTAG_STATUS_OK;

            *plc = *walker;

            /* leave the critical section, we found a match. */
            break;
        }

        /* we did not find a matching PLC, create a new one. */
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "No matching PLC found, creating a new one.");

        is_new = 1;

        /* No matching PLC found in list, will create a new one */
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "Creating new PLC connection.");

        pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "connection_group_id=%d, server_id=%d, server=%s", connection_group_id,
               server_id, server);

        /* Allocate and initialize the PLC object inside the mutex.
         * This prevents duplicate creation when multiple threads race to create the same PLC.
         * Note: rc_alloc() already zero-initializes the memory. */
        *plc = (modbus_plc_p)rc_alloc((int)(unsigned int)sizeof(struct modbus_plc_t), modbus_plc_destructor);
        if(*plc) {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Setting connection_group_id to %d.", connection_group_id);
            (*plc)->connection_group_id = connection_group_id;

            /* copy the server string so that we can find this again. */
            (*plc)->server = str_dup(server);
            if(!((*plc)->server)) {
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Unable to allocate Modbus PLC server string!");
                rc = PLCTAG_ERR_NO_MEM;
            } else {
                /* make sure we can be found. */
                (*plc)->server_id = (uint8_t)(unsigned int)server_id;

                if(tag_driver_init(&(*plc)->driver) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Unable to create the tag lists!");
                    rc = PLCTAG_ERR_NO_MEM;
                }

                /* initialize timestamp tracking */
                (*plc)->last_packet_time_ms = time_ms();

                /* create the PLC mutex to protect the tag list. */
                rc = mutex_create(&((*plc)->mutex));
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Unable to create new mutex, error %s!", plc_tag_decode_error(rc));
                    rc = PLCTAG_ERR_MUTEX_INIT;
                } else {
                    /* set up the maximum request depth. */
                    (*plc)->max_requests_in_flight = max_requests_in_flight;

                    /* Initialize PLC state before making it visible to other threads */
                    (*plc)->state = PLC_CONNECT_START;
                    atomic_init_int32(&(*plc)->connection_inactivity_timeout_ms, connection_inactivity_timeout_ms);
                    atomic_init_int32(&(*plc)->connection_retry_max_delay_ms, connection_retry_max_delay_ms);
                    conn_watch_init(&(*plc)->watch, PLCTAG_CONN_STATUS_DOWN);

                    /* Calculate initial disconnect deadline */
                    (*plc)->cached_inactivity_timeout_ms = atomic_get_int32(&(*plc)->connection_inactivity_timeout_ms);
                    (*plc)->disconnect_at_time_ms = (*plc)->last_packet_time_ms + (*plc)->cached_inactivity_timeout_ms;

                    /* Add the new PLC to the global list. We already have the mutex,
                     * so no duplicate can be created by another thread. The struct is
                     * fully initialized and the mutex exists, so other threads can
                     * safely find and use this PLC. */
                    (*plc)->next = plcs;
                    plcs = *plc;
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "New PLC added to the global PLC list.");
                }
            }
        } else {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Unable to allocate Modbus PLC object!");
            rc = PLCTAG_ERR_NO_MEM;
        }
    }

    /* if everything went well and it is new, set up the new PLC. */
    if(rc == PLCTAG_STATUS_OK && is_new) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "Initializing new PLC.");

        do {
            /*
             * With deferred cleanup, the handler thread does NOT need to hold an explicit
             * reference to the PLC. The tags hold the references through tag->plc pointers.
             * When the handler thread exits, it will not decrement the refcount, so the PLC
             * will stay alive as long as tags reference it. When the last tag is destroyed,
             * its destructor will release the final PLC reference and trigger PLC destruction
             * via deferred cleanup.
             */
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0,
                   "Handler thread will reference PLC through task parameter (no explicit rc_inc).");

            rc = thread_create(&((*plc)->handler_thread), modbus_plc_handler, 32768 /* ignored */, (void *)(*plc));
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Unable to create new handler thread, error %s!",
                       plc_tag_decode_error(rc));
                break;
            }

            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Created thread %p.", (*plc)->handler_thread);

            /* Increment PLC count for lifecycle tracking */
            atomic_add_int32(&plc_count, 1);

            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "PLC created, count now %d.", atomic_get_int32(&plc_count));
        } while(0);
    }

    if(rc != PLCTAG_STATUS_OK && *plc) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "PLC lookup or creation failed!");

        /* clean up. */
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "rc_dec: Releasing the reference to the PLC due to creation error!");
        *plc = rc_dec(*plc);
    }

    if(out_is_new) { *out_is_new = (is_new != 0); }

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "Done.");

    return rc;
}


/* never enter this from within the handler thread itself! */
void modbus_plc_destructor(void *plc_arg) {
    modbus_plc_p plc = (modbus_plc_p)plc_arg;

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "Starting.");

    if(!plc) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Destructor called with null pointer!");
        return;
    }

    /* remove the plc from the list. */
    critical_block(mb_mutex) {
        modbus_plc_p *walker = &plcs;

        /*
         * FIXME - this has an ABA problem.  We should compare the host, port and
         * connection group ID to be sure we have the right one.  Even then we
         * could have a new connection that matches the same parameters.
         */

        while(*walker && *walker != plc) { walker = &((*walker)->next); }

        if(*walker) {
            /* unlink the list. */
            *walker = plc->next;
            plc->next = NULL;
        } else {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "PLC not found in the list!");
        }
    }

    /* shut down the thread. */
    if(plc->handler_thread) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Terminating Modbus handler thread %p.", plc->handler_thread);

        /* set the flag to cause the thread to terminate. */
        atomic_set_bool(&plc->flags.terminate, true);

        /* Signal the socket to free the thread. This happens before thread_join()
         * below, so the handler thread may still be running (e.g. inside connect_plc()
         * creating plc->sock for the first time) -- plc->mutex is required here, not
         * optional, despite this being the destructor. */
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Waking Modbus handler thread %p.", plc->handler_thread);
        critical_block(plc->mutex) {
            if(plc->sock) {
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Waking socket directly.");
                socket_wake(plc->sock);
            }
        }

        /* wait for the thread to terminate and destroy it. */
        thread_join(plc->handler_thread);
        thread_destroy(&plc->handler_thread);

        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Modbus handler thread %p destroyed.", plc->handler_thread);

        plc->handler_thread = NULL;
    }

    /* every connection tag holds a reference to the PLC, so none is left watching. */
    conn_watch_destroy(&plc->watch);

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

    if(plc->driver.active && vector_length(plc->driver.active) > 0) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "There are tags still remaining in the active tag list, memory leak possible!");
    }
    tag_driver_destroy(&plc->driver);

    /* Decrement PLC count and signal cleanup when last one is destroyed. This must
     * share mb_mutex with mb_teardown()'s destruction of plc_cleanup_cond below:
     * holding the mutex here guarantees mb_teardown can never free the condvar out
     * from under a signal call that is still in flight. */
    int32_t remaining = 0;
    critical_block(mb_mutex) {
        atomic_add_int32(&plc_count, -1);
        remaining = atomic_get_int32(&plc_count);

        if(remaining == 0 && plc_cleanup_cond) {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "Last PLC destroyed, signaling cleanup condition.");
            cond_signal(plc_cleanup_cond);
        }
    }
    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "PLC destroyed, count now %d.", remaining);

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "Done.");
}


/**
 * @brief Reset all pending requests for a Modbus PLC.
 *
 * This function resets all pending requests for a Modbus PLC by clearing
 * the request slots and resetting the tags to their request state if they were waiting for
 * a response. This is typically called when the socket is disconnected or has an error.
 *
 * This closes the socket if it is open!
 *
 * This resets all the flags and data lengths to their initial state.
 *
 * @param plc Pointer to the Modbus PLC structure.
 * @return int Status code indicating success or failure.
 */
static int reset_plc(modbus_plc_p plc) {
    pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Starting reset_plc() - connection will be closed.");

    if(!plc) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Null PLC pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!plc->mutex) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "PLC mutex is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "MUTEX: Acquire. Resetting PLC.");
    critical_block(plc->mutex) {
        /* nothing is in flight on a closed socket: the requests go again, in their old order. */
        for(int32_t i = 0; i < MAX_MODBUS_REQUESTS; i++) {
            modbus_tag_p tag = plc->slots[i].tag;

            if(tag) { tag_driver_queue_unsafe(&plc->driver, (plc_tag_p)tag, tag->op_time); }

            plc->slots[i].tag = NULL;
            plc->slots[i].used = false;
        }

        plc->slots_used = 0;

        if(plc->sock) {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Closing socket due to error or disconnect.");
            socket_close(plc->sock);
        } else {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Socket already closed.");
        }

        /* set up the state. */
        atomic_set_bool(&plc->flags.response_ready, false);
        atomic_set_bool(&plc->flags.request_ready, false);
        plc->read_data_len = 0;
        plc->write_data_len = 0;
        plc->write_data_offset = 0;
    }
    pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "MUTEX: Release. PLC reset.");

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "Done.");

    /* Wake the PLC handler thread to process the reset state immediately */
    wake_plc_thread(plc);

    return PLCTAG_STATUS_OK;
}


THREAD_FUNC(modbus_plc_handler) {
    int rc = PLCTAG_STATUS_OK;
    modbus_plc_p plc = (modbus_plc_p)arg;
    backoff_t err_backoff;
    int64_t err_delay = 0;
    int64_t err_delay_until = 0;
    int sock_events = SOCK_EVENT_NONE;
    int waitable_events = SOCK_EVENT_NONE;
    int32_t timeout_ms = 0;

    backoff_init(&err_backoff, PLC_SOCKET_ERR_START_DELAY, PLC_SOCKET_ERR_MAX_DELAY);

    /* where PLC_CLOSE_SOCKET goes once the socket is down; set by whoever enters it */
    modbus_plc_state_t state_after_close = PLC_CONNECT_START;

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "Starting.");

    if(!plc) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Null PLC pointer passed!");
        THREAD_RETURN(0);
    }

    /* Increment the count of active handler threads */
    atomic_add_int32(&handler_threads_active, 1);

    while(!atomic_get_bool(&plc->flags.terminate) && atomic_get_bool(&lib_active)) {
        int64_t wait_time_ms = service_tags(plc);

        /* the last pass's connection events, to the @connection tags watching. */
        connection_tag_notify_watchers(&plc->watch, plc->mutex);

        switch(plc->state) {
            case PLC_CONNECT_START:
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "in PLC_CONNECT_START state.");

                /* Reset the disconnect deadline when entering connection state */
                plc->last_packet_time_ms = time_ms();
                timeout_ms = atomic_get_int32(&plc->connection_inactivity_timeout_ms);
                plc->cached_inactivity_timeout_ms = timeout_ms;
                plc->disconnect_at_time_ms = plc->last_packet_time_ms + timeout_ms;
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Resetting disconnect deadline to %" PRId64 " (timeout=%dms)",
                       plc->disconnect_at_time_ms, timeout_ms);

                mb_plc_set_conn_status(plc, PLCTAG_CONN_STATUS_CONNECTING);

                /* reset the PLC to initial state, including closing the socket */
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "Calling reset_plc() from PLC_CONNECT_START state.");
                reset_plc(plc);

                /* connect to the PLC */
                rc = connect_plc(plc);
                if(rc == PLCTAG_STATUS_PENDING) {
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0,
                           "Socket connection process started.  Going to PLC_CONNECT_WAIT state.");
                    plc->state = PLC_CONNECT_WAIT;
                } else if(rc == PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Successfully connected to the PLC.  Going to PLC_READY state.");

                    backoff_reset(&err_backoff);

                    /* Update timestamp for inactivity tracking now that we're connected */
                    plc->last_packet_time_ms = time_ms();
                    int32_t timeout_ms = atomic_get_int32(&plc->connection_inactivity_timeout_ms);
                    plc->cached_inactivity_timeout_ms = timeout_ms;
                    plc->disconnect_at_time_ms = plc->last_packet_time_ms + timeout_ms;
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0,
                           "Updated last_packet_time_ms=%" PRId64 ", disconnect_at=%" PRId64
                           " (connection succeeded immediately in PLC_CONNECT_START).",
                           plc->last_packet_time_ms, plc->disconnect_at_time_ms);

                    plc->state = PLC_READY;
                } else {
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Error %s received while starting socket connection.",
                           plc_tag_decode_error(rc));

                    /* exponential increase with jitter, up to the live tag attribute's maximum. */
                    backoff_set_max_ms(&err_backoff, atomic_get_int32(&plc->connection_retry_max_delay_ms));
                    err_delay = backoff_wait_ms(&err_backoff);
                    err_delay_until = time_ms() + err_delay;

                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0,
                           "Unable to connect to the PLC, will retry later! Going to PLC_ERR_WAIT state to wait %" PRId64 "ms.",
                           err_delay);

                    state_after_close = PLC_ERR_WAIT;
                    plc->state = PLC_CLOSE_SOCKET;
                }
                break;

            case PLC_CONNECT_WAIT:
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "in PLC_CONNECT_WAIT state.");
                mb_plc_set_conn_status(plc, PLCTAG_CONN_STATUS_CONNECTING);
                rc = socket_connect_tcp_check(plc->sock, SOCKET_CONNECT_TIMEOUT);
                if(rc == PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Socket connected, going to state PLC_READY.");

                    /* Update timestamp for inactivity tracking now that we're connected */
                    plc->last_packet_time_ms = time_ms();
                    timeout_ms = atomic_get_int32(&plc->connection_inactivity_timeout_ms);
                    plc->cached_inactivity_timeout_ms = timeout_ms;
                    plc->disconnect_at_time_ms = plc->last_packet_time_ms + timeout_ms;
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0,
                           "Updated last_packet_time_ms=%" PRId64 ", disconnect_at=%" PRId64
                           " (connection established in PLC_CONNECT_WAIT).",
                           plc->last_packet_time_ms, plc->disconnect_at_time_ms);

                    backoff_reset(&err_backoff);

                    plc->state = PLC_READY;
                } else if(rc == PLCTAG_ERR_TIMEOUT) {
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Still waiting for socket to connect.");

                    /* do not wait more.   The TCP connection check will wait in poll() (POSIX) or select() (Windows). */
                } else {
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Error %s received while waiting for socket connection.",
                           plc_tag_decode_error(rc));

                    /* exponential increase with jitter, up to the live tag attribute's maximum. */
                    backoff_set_max_ms(&err_backoff, atomic_get_int32(&plc->connection_retry_max_delay_ms));
                    err_delay = backoff_wait_ms(&err_backoff);
                    err_delay_until = time_ms() + err_delay;

                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0,
                           "Unable to connect to the PLC, will retry later! Going to PLC_ERR_WAIT state to wait %" PRId64 "ms.",
                           err_delay);

                    state_after_close = PLC_ERR_WAIT;
                    plc->state = PLC_CLOSE_SOCKET;
                }
                break;

            case PLC_READY:
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "in PLC_READY state.");
                mb_plc_set_conn_status(plc, PLCTAG_CONN_STATUS_UP);

                /* make sure that our timeout period has not changed */
                if(plc->cached_inactivity_timeout_ms != atomic_get_int32(&plc->connection_inactivity_timeout_ms)) {
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0,
                           "Inactivity timeout changed from %" PRId32 "ms to %" PRId32 "ms, updating disconnect deadline.",
                           plc->cached_inactivity_timeout_ms, atomic_get_int32(&plc->connection_inactivity_timeout_ms));
                    plc->cached_inactivity_timeout_ms = atomic_get_int32(&plc->connection_inactivity_timeout_ms);
                    plc->disconnect_at_time_ms = plc->last_packet_time_ms + plc->cached_inactivity_timeout_ms;
                }

                /* calculate what events we should be waiting for. */
                waitable_events = SOCK_EVENT_DEFAULT_MASK | SOCK_EVENT_CAN_READ;

                /* if there is a request queued for sending, send it. */
                if(atomic_get_bool(&plc->flags.request_ready)) { waitable_events |= SOCK_EVENT_CAN_WRITE; }

                /* wait no longer than the next tag deadline */
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "Waiting up to %" PRId64 "ms for socket events.", wait_time_ms);
                sock_events = socket_wait_event(plc->sock, waitable_events, (int)wait_time_ms);
                if(sock_events & SOCK_EVENT_TIMEOUT) {
                    int64_t current_time = time_ms();
                    int64_t idle_time = current_time - plc->last_packet_time_ms;
                    int32_t inactivity_timeout_ms = plc->cached_inactivity_timeout_ms;

                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0,
                           "Socket wait timed out. Idle for %" PRId64 "ms. Pending requests: %" PRId32, idle_time,
                           plc->slots_used);

                    /* Only disconnect if truly idle AND no pending responses expected */
                    if(idle_time >= inactivity_timeout_ms && plc->slots_used == 0) {
                        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0,
                               "Inactivity timeout reached after %" PRId64 "ms idle (threshold=%dms). current_time=%" PRId64
                               ", last_packet_time=%" PRId64 ". Calling reset_plc() and going to PLC_IDLE_WAIT.",
                               idle_time, inactivity_timeout_ms, current_time, plc->last_packet_time_ms);

                        mb_plc_set_conn_status(plc, PLCTAG_CONN_STATUS_DISCONNECTING);

                        /* close the socket, then wait for something to happen. */
                        state_after_close = PLC_IDLE_WAIT;
                        plc->state = PLC_CLOSE_SOCKET;
                    } else if(plc->slots_used > 0) {
                        /* Timeout while waiting for responses, just continue waiting */
                        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0,
                               "Socket timeout while waiting for %" PRId32 " pending response(s) (idle %" PRId64
                               "ms), continuing to wait.",
                               plc->slots_used, idle_time);
                    } else {
                        /* Timeout was for auto-sync, continue immediately */
                        pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "Auto-sync timeout, continuing.");
                    }
                }

                /* check for socket errors or disconnects. */
                if((sock_events & SOCK_EVENT_ERROR) || (sock_events & SOCK_EVENT_DISCONNECT)) {
                    if(sock_events & SOCK_EVENT_DISCONNECT) {
                        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Unexepected socket disconnect!");
                    } else {
                        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Unexpected socket error!");
                    }

                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Going to state PLC_CLOSE_SOCKET");

                    /* close the socket, report the link down, then reconnect immediately */
                    state_after_close = PLC_CONNECT_START;
                    plc->state = PLC_CLOSE_SOCKET;
                    break;
                }

                /* preference pushing requests to the PLC */
                if(sock_events & SOCK_EVENT_CAN_WRITE) {
                    if(atomic_get_bool(&plc->flags.request_ready)) {
                        pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0,
                               "There is a request ready to send and we can send, going to state PLC_SEND_REQUEST.");
                        plc->state = PLC_SEND_REQUEST;
                        break;
                    } else {
                        /* clear the buffer indexes just in case */
                        plc->write_data_len = 0;
                        plc->write_data_offset = 0;
                        pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "Request ready state changed while we waited for the socket.");
                    }
                }

                if(sock_events & SOCK_EVENT_CAN_READ) {
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "We can receive a response going to state PLC_RECEIVE_RESPONSE.");
                    plc->state = PLC_RECEIVE_RESPONSE;
                    break;
                }

                if(sock_events & SOCK_EVENT_TIMEOUT) {
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "Timed out waiting for something to happen.");
                }

                if(sock_events & SOCK_EVENT_WAKE_UP) { pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "Someone woke us up."); }

                break;

            case PLC_SEND_REQUEST:
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "in PLC_SEND_REQUEST state.");

                conn_watch_publish(&plc->watch, TAG_CONN_EVENT_SEND_REQUEST_STARTED, PLCTAG_STATUS_OK);
                rc = send_request(plc);
                if(rc == PLCTAG_STATUS_OK) {
                    conn_watch_publish(&plc->watch, TAG_CONN_EVENT_SEND_REQUEST_COMPLETED, PLCTAG_STATUS_OK);
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "Request sent, going to back to state PLC_READY.");

                    atomic_set_bool(&plc->flags.request_ready, false);
                    plc->write_data_len = 0;
                    plc->write_data_offset = 0;

                    plc->state = PLC_READY;
                } else if(rc == PLCTAG_STATUS_PENDING) {
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "Not all data written, will try again.");
                } else {
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Resetting PLC due to write error %s.", plc_tag_decode_error(rc));

                    /* close the socket, report the link down, then reconnect immediately */
                    state_after_close = PLC_CONNECT_START;
                    plc->state = PLC_CLOSE_SOCKET;
                }

                /* if we did not send all the packet, we stay in this state and keep trying. */


                break;


            case PLC_RECEIVE_RESPONSE:
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "in PLC_RECEIVE_RESPONSE state.");

                /* get a packet */
                conn_watch_publish(&plc->watch, TAG_CONN_EVENT_RECEIVE_RESPONSE_STARTED, PLCTAG_STATUS_OK);
                rc = receive_response(plc);
                if(rc == PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "Response ready, going back to PLC_READY state.");
                    conn_watch_publish(&plc->watch, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, PLCTAG_STATUS_OK);
                    atomic_set_bool(&plc->flags.response_ready, true);
                    plc->state = PLC_READY;
                } else if(rc == PLCTAG_STATUS_PENDING) {
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "Response not complete, continue reading data.");
                } else {
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Reconnecting due to read error %s.", plc_tag_decode_error(rc));

                    /* close the socket, report the link down, then reconnect immediately */
                    state_after_close = PLC_CONNECT_START;
                    plc->state = PLC_CLOSE_SOCKET;
                }

                /* in all cases we want to cycle through the state machine immediately. */

                break;


            case PLC_IDLE_WAIT:
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "in PLC_IDLE_WAIT state.");
                mb_plc_set_conn_status(plc, PLCTAG_CONN_STATUS_IDLE_WAIT);

                /* a due tag needs the connection; a due destroy does not. */
                bool has_active_tags = false;
                critical_block(plc->mutex) { has_active_tags = tag_driver_has_due_unsafe(&plc->driver, time_ms()); }
                if(has_active_tags) {
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0,
                           "PLC has active tags needing connection, transitioning to PLC_CONNECT_START.");
                    plc->state = PLC_CONNECT_START;
                    break;
                }

                /* wait until something happens. */
                sock_events = socket_wait_event(plc->sock, SOCK_EVENT_DEFAULT_MASK, MODBUS_IDLE_WAIT_TIMEOUT);

                if(sock_events & SOCK_EVENT_TIMEOUT) { pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "PLC idle wait timed out."); }

                break;

            case PLC_CLOSE_SOCKET:
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "in PLC_CLOSE_SOCKET state.");

                /* one place reports the link down, and one place tears the socket down */
                mb_plc_set_conn_status(plc, PLCTAG_CONN_STATUS_DOWN);
                reset_plc(plc);

                plc->state = state_after_close;
                break;

            case PLC_ERR_WAIT:
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "in PLC_ERR_WAIT state.");
                mb_plc_set_conn_status(plc, PLCTAG_CONN_STATUS_ERR_WAIT);

                /* wait until done. */
                if(err_delay_until > time_ms()) {
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "Waiting for at least %" PRId64 "ms.",
                           (err_delay_until - time_ms()));
                    socket_wait_event(plc->sock, SOCK_EVENT_WAKE_UP | SOCK_EVENT_TIMEOUT, (int)(err_delay_until - time_ms()));
                } else {
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "Error wait is over, going to state PLC_CONNECT_START.");
                    plc->state = PLC_CONNECT_START;
                }
                break;

            default:
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Unknown state %d!", plc->state);
                plc->state = PLC_CONNECT_START;
                break;
        }

        /* wait if needed, could be signalled already. */
        // cond_wait(plc->wait_cond, MODBUS_IDLE_WAIT_TIMEOUT);
    }

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "Handler thread exiting.");

    /* With deferred cleanup thread, we do NOT call rc_dec() here.
     * The tags hold references to the PLC through their tag->plc pointers.
     * When each tag is destroyed, its destructor calls rc_dec() on the PLC.
     * The last tag's destructor will trigger the PLC destructor via deferred cleanup.
     *
     * If we called rc_dec() here, we could trigger PLC destruction while teardown
     * code is still running and trying to access the PLC (race condition).
     */
    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0,
           "Handler thread exiting without decrementing PLC reference (deferred cleanup will handle it).");

    /* Decrement the count of active handler threads */
    atomic_add_int32(&handler_threads_active, -1);

    THREAD_RETURN(0);
}


void wake_plc_thread(modbus_plc_p plc) {
    modbus_plc_p plc_ref = NULL;

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Starting.");

    if(plc) {
        /* Take a reference to the PLC to safely access it */
        plc_ref = rc_inc(plc);

        if(!plc_ref) {
            /* PLC refcount already hit zero, it's being destroyed or already destroyed */
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "PLC reference count is zero, cannot wake (PLC is being destroyed).");
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Done.");
            return;
        }

        /* Check if PLC is terminating to avoid accessing freed mutex */
        if(atomic_get_bool(&plc_ref->flags.terminate)) {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "PLC is terminating, skipping wake.");
            plc_ref = rc_dec(plc_ref);
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Done.");
            return;
        }

        /*
         * Holding a reference guarantees the destructor (and so socket_destroy()
         * and mutex_destroy()) hasn't started, so plc_ref->mutex is still valid.
         * It's still required here, though: the handler thread may be concurrently
         * creating plc_ref->sock for the first time in connect_plc(), so reading it
         * must be synchronized with that write.
         */
        critical_block(plc_ref->mutex) {
            if(plc_ref->sock) {
                socket_wake(plc_ref->sock);
            } else {
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "PLC socket pointer is NULL.");
            }
        }

        /* Release the reference */
        plc_ref = rc_dec(plc_ref);
    } else {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "PLC pointer is NULL!");
    }

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Done.");
}


/**
 * @brief Connect to the PLC.
 *
 * This may be called to (re)establish a connection to the PLC. In that case,
 * we may already have a socket object and a parsed server/port.
 *
 * @param plc
 * @return int
 */
int connect_plc(modbus_plc_p plc) {
    int rc = PLCTAG_STATUS_OK;
    char **server_port = NULL;
    char *server = NULL;
    int port = MODBUS_DEFAULT_PORT;

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Starting.");


    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Parsing server host and port from server string.");

    server_port = str_split(plc->server, ":");
    if(!server_port) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Unable to split server and port string!");
        return PLCTAG_ERR_BAD_CONFIG;
    }

    if(server_port[0] == NULL) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Server string is malformed or empty!");
        mem_free(server_port);
        return PLCTAG_ERR_BAD_CONFIG;
    } else {
        server = server_port[0];
    }

    if(server_port[1] != NULL) {
        rc = str_to_int(server_port[1], &port);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Unable to extract port number from server string \"%s\"!", plc->server);
            mem_free(server_port);
            server_port = NULL;
            return PLCTAG_ERR_BAD_CONFIG;
        }
    } else {
        port = MODBUS_DEFAULT_PORT;
    }

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Using server \"%s\" and port %d.", server, port);

    if(!plc->sock) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Creating new socket.");

        /* plc->sock starts NULL and is only ever set here, once, for this PLC's whole
         * lifetime (reconnects reuse the same socket object via socket_close/reconnect).
         * wake_plc_thread() and the destructor's pre-join wake can read it from another
         * thread while this handler thread is still running, so the write must be
         * synchronized with those reads via plc->mutex. */
        critical_block(plc->mutex) { rc = socket_create(&(plc->sock)); }
        if(rc != PLCTAG_STATUS_OK) {
            /* done with the split string. */
            mem_free(server_port);
            server_port = NULL;

            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Unable to create socket object, error %s!", plc_tag_decode_error(rc));
            return rc;
        }
    }

    /* connect to the socket */
    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Connecting to %s on port %d...", server, port);
    rc = socket_connect_tcp_start(plc->sock, server, port);
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        /* done with the split string. */
        mem_free(server_port);

        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Unable to connect to the server \"%s\", got error %s!", plc->server,
               plc_tag_decode_error(rc));
        return rc;
    }

    /* done with the split string. */
    if(server_port) {
        mem_free(server_port);
        server_port = NULL;
    }

    /* clear the state for reading and writing. */
    atomic_set_bool(&plc->flags.request_ready, false);
    atomic_set_bool(&plc->flags.response_ready, false);
    plc->read_data_len = 0;
    plc->write_data_len = 0;
    plc->write_data_offset = 0;

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Done with status %s.", plc_tag_decode_error(rc));

    return rc;
}


/*
 * One pass over the tags: hand a reply that has arrived to its tag, run the driver's
 * housekeeping, and send the next due request if there is room for it.  Returns how long the
 * handler may wait for the socket before it has to look again.
 */
static int64_t service_tags(modbus_plc_p plc) {
    int64_t wait_ms = MODBUS_IDLE_WAIT_TIMEOUT;
    bool skipped = false;

    critical_block(plc->mutex) {
        int64_t now = time_ms();
        int64_t next = 0;

        if(atomic_get_bool(&plc->flags.response_ready)) { dispatch_reply_unsafe(plc); }

        tag_driver_tickle_due_unsafe(&plc->driver, now);

        if(plc->state == PLC_READY) { skipped = send_next_unsafe(plc, now); }

        /* a due tag with a free slot goes out on the next pass; with none, a reply frees one. */
        next = tag_driver_next_time_unsafe(&plc->driver);
        if(next > now) {
            wait_ms = (next - now < wait_ms) ? next - now : wait_ms;
        } else if(next && plc->slots_used < plc->max_requests_in_flight) {
            wait_ms = 1;
        }
    }

    /* a due tag held by an application thread: yield and look again, rather than wait 1ms. */
    if(!skipped) {
        plc->lock_retries = 0;
    } else if(++plc->lock_retries < TAG_DRIVER_LOCK_YIELDS) {
        sleep_ms(0);
        wait_ms = 0;
    }

    tag_driver_notify(&plc->driver);

    return wait_ms;
}


/*
 * Hand the reply in read_data to the tag whose request it answers.  Decoding needs the tag's
 * API mutex; when it is busy this lets go of plc->mutex while it waits, so whoever holds the
 * API mutex can take plc->mutex to abort the tag, as CIP's dispatch_replies_unsafe() does.
 */
static void dispatch_reply_unsafe(modbus_plc_p plc) {
    uint16_t tid = (uint16_t)((uint16_t)plc->read_data[1] + (uint16_t)(plc->read_data[0] << 8));
    int32_t slot = -1;
    modbus_tag_p tag = NULL;
    int rc = PLCTAG_STATUS_OK;
    int32_t tries = 0;

    for(int32_t i = 0; i < MAX_MODBUS_REQUESTS; i++) {
        if(plc->slots[i].used && plc->slots[i].tid == tid) { slot = i; }
    }

    if(slot < 0) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "No request matches response TID %u -- discarding.", tid);
        atomic_set_bool(&plc->flags.response_ready, false);
        plc->read_data_len = 0;
        return;
    }

    while((tag = plc->slots[slot].tag) && mutex_try_lock(tag->api_mutex) != PLCTAG_STATUS_OK) {
        mutex_unlock(plc->mutex);
        sleep_ms(++tries < TAG_DRIVER_LOCK_YIELDS ? 0 : 1);
        mutex_lock(plc->mutex);
    }

    plc->slots[slot].used = false;
    plc->slots[slot].tag = NULL;
    plc->slots_used--;

    /* aborted while the request was out. */
    if(!tag) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Discarding response TID %u to an aborted request.", tid);
        atomic_set_bool(&plc->flags.response_ready, false);
        plc->read_data_len = 0;
        return;
    }

    rc = tag_op_is_read(tag) ? check_read_response(plc, tag) : check_write_response(plc, tag);

    atomic_set_bool(&plc->flags.response_ready, false);
    plc->read_data_len = 0;

    if(rc == PLCTAG_ERR_PARTIAL) {
        tag_driver_queue_unsafe(&plc->driver, (plc_tag_p)tag, time_ms());
    } else {
        tag_op_end((plc_tag_p)tag, rc);
        tag_driver_done_unsafe(&plc->driver, (plc_tag_p)tag, time_ms());
    }

    mutex_unlock(tag->api_mutex);
}


/*
 * Build the request for the first due tag with an op, if the write buffer and a request slot
 * are free.  The tag leaves the list while its request is out; it keeps its op_time in case
 * the connection drops and it has to go again.  Returns true if a due tag was skipped because
 * its API mutex was busy and nothing was built.
 */
static bool send_next_unsafe(modbus_plc_p plc, int64_t now) {
    bool skipped = false;

    if(atomic_get_bool(&plc->flags.request_ready) || plc->slots_used >= plc->max_requests_in_flight) { return false; }

    for(int32_t i = 0; i < vector_length(plc->driver.active); i++) {
        modbus_tag_p tag = vector_get(plc->driver.active, i);
        bool is_read = false;
        int rc = PLCTAG_STATUS_OK;

        if(tag->op_time > now) { break; }

        /* armed for an auto-sync deadline, or waiting for its tickle: no request to send. */
        if(!tag_op_busy(tag)) { continue; }

        if(mutex_try_lock(tag->api_mutex) != PLCTAG_STATUS_OK) {
            skipped = true;
            continue;
        }

        is_read = tag_op_is_read(tag);
        rc = is_read ? create_read_request(plc, tag) : create_write_request(plc, tag);

        if(rc == PLCTAG_STATUS_OK) {
            int32_t slot = 0;

            while(plc->slots[slot].used) { slot++; }

            plc->slots[slot].used = true;
            plc->slots[slot].tag = tag;
            plc->slots[slot].tid = tag->seq_id;
            plc->slots_used++;

            /* the request holds the data now; a later set_* dirties the tag again. */
            if(!is_read) { tag->tag_is_dirty = 0; }

            tag_driver_forget_unsafe(&plc->driver, (plc_tag_p)tag);
        } else {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, tag->tag_id, "Error %s creating request!", plc_tag_decode_error(rc));
            tag_op_end((plc_tag_p)tag, rc);
            tag_driver_done_unsafe(&plc->driver, (plc_tag_p)tag, now);
        }

        mutex_unlock(tag->api_mutex);

        /* one write buffer: the next request waits for this one to go out. */
        return false;
    }

    return skipped;
}


/* the slot holding the tag's request, -1 if none.  Called with plc->mutex held. */
static int32_t find_slot_unsafe(modbus_plc_p plc, modbus_tag_p tag) {
    for(int32_t i = 0; i < MAX_MODBUS_REQUESTS; i++) {
        if(plc->slots[i].used && plc->slots[i].tag == tag) { return i; }
    }

    return -1;
}


/*
 * Take the tag off the list and out of any request slot.  A request on the wire stays
 * counted until its reply arrives and is discarded, or the connection drops.
 */
static void forget_tag_unsafe(modbus_plc_p plc, modbus_tag_p tag) {
    int32_t slot = find_slot_unsafe(plc, tag);

    tag_driver_forget_unsafe(&plc->driver, (plc_tag_p)tag);

    if(slot >= 0) { plc->slots[slot].tag = NULL; }
}


/* make the tag due now so the handler looks at it; one with a request out waits for its reply. */
static void queue_tag(modbus_tag_p tag) {
    critical_block(tag->plc->mutex) {
        if(find_slot_unsafe(tag->plc, tag) < 0) { tag_driver_queue_unsafe(&tag->plc->driver, (plc_tag_p)tag, time_ms()); }
    }

    wake_plc_thread(tag->plc);
}


int receive_response(modbus_plc_p plc) {
    int rc = 0;
    int data_needed = 0;

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "Starting.");

    /* socket could be closed due to inactivity. */
    if(!plc->sock) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "Socket is closed or missing.");
        return PLCTAG_STATUS_OK;
    }

    do {
        /* how much data do we need? */
        if(plc->read_data_len >= MODBUS_MBAP_SIZE) {
            int packet_size = plc->read_data[5] + (plc->read_data[4] << 8);
            data_needed = (MODBUS_MBAP_SIZE + packet_size) - plc->read_data_len;

            pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "Packet header read, data_needed=%d, packet_size=%d, read_data_len=%d",
                   data_needed, packet_size, plc->read_data_len);

            if((MODBUS_MBAP_SIZE + packet_size) > PLC_READ_DATA_LEN) {
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Error, packet size, %d, greater than buffer size, %d!",
                       (MODBUS_MBAP_SIZE + packet_size), PLC_READ_DATA_LEN);
                return PLCTAG_ERR_TOO_LARGE;
            } else if(data_needed < 0) {
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Read more than a packet!  Expected %d bytes, but got %d bytes!",
                       (MODBUS_MBAP_SIZE + packet_size), plc->read_data_len);
                return PLCTAG_ERR_TOO_LARGE;
            }
        } else {
            data_needed = MODBUS_MBAP_SIZE - plc->read_data_len;
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "Still reading packet header, data_needed=%d, read_data_len=%d",
                   data_needed, plc->read_data_len);
        }

        if(data_needed == 0) {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "Got all data needed.");
            break;
        }

        /* read the socket. */
        rc = socket_read(plc->sock, plc->read_data + plc->read_data_len, data_needed, SOCKET_READ_TIMEOUT);
        if(rc >= 0) {
            /* got data! Or got nothing, but no error. */
            plc->read_data_len += rc;

            pdebug_dump_bytes(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, plc->read_data, plc->read_data_len);
        } else if(rc == PLCTAG_ERR_TIMEOUT) {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "Done. Socket read timed out.");
            return PLCTAG_STATUS_PENDING;
        } else {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Error, %s, reading socket!", plc_tag_decode_error(rc));
            return rc;
        }

        pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "After reading the socket, total read=%d and data needed=%d.",
               plc->read_data_len, data_needed);
    } while(rc > 0);

    /* are we done? */
    if(data_needed == 0) {
        /* we got our packet. */
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Received full packet.");
        pdebug_dump_bytes(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, plc->read_data, plc->read_data_len);

        if(plc->read_data_len < MODBUS_MIN_RESPONSE_SIZE) {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Response of %d bytes is too short to be a valid Modbus reply!",
                   plc->read_data_len);
            plc->read_data_len = 0;
            return PLCTAG_ERR_TOO_SMALL;
        }

        /*
         * The MBAP header is fixed by the spec, so check it here where the whole packet is in
         * hand rather than leaving it to the per-tag handlers.  A non-zero protocol identifier
         * is not Modbus/TCP at all, and a reply carrying somebody else's unit ID is a reply to
         * somebody else -- neither is data we should hand to a tag.
         */
        if(plc->read_data[2] != 0 || plc->read_data[3] != 0) {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Response protocol identifier is %u, expected zero!",
                   (unsigned int)((plc->read_data[2] << 8) + plc->read_data[3]));
            plc->read_data_len = 0;
            return PLCTAG_ERR_BAD_DATA;
        }

        if(plc->read_data[6] != plc->server_id) {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Response is for unit %u but we are talking to unit %u!",
                   (unsigned int)plc->read_data[6], (unsigned int)plc->server_id);
            plc->read_data_len = 0;
            return PLCTAG_ERR_BAD_DATA;
        }

        /* Update packet timestamp for inactivity tracking */
        plc->last_packet_time_ms = time_ms();
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "Updated last_packet_time_ms=%" PRId64 " (received full packet).",
               plc->last_packet_time_ms);

        atomic_set_bool(&plc->flags.response_ready, true);

        rc = PLCTAG_STATUS_OK;
    } else {
        /* data_needed is greater than zero. */
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "Received partial packet of %d bytes of %d.", plc->read_data_len,
               (data_needed + plc->read_data_len));
        rc = PLCTAG_STATUS_PENDING;
    }

    /* if we have some data in the buffer, update the inactivity timer. */
    if(plc->read_data_len > 0) {
        plc->last_packet_time_ms = time_ms();
        int32_t timeout_val = atomic_get_int32(&plc->connection_inactivity_timeout_ms);
        plc->disconnect_at_time_ms = plc->last_packet_time_ms + timeout_val;
    }

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "Done.");

    return rc;
}


int send_request(modbus_plc_p plc) {
    int rc = 1;
    int data_left = plc->write_data_len - plc->write_data_offset;

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "Starting.");

    /* check socket, could be closed due to inactivity. */
    if(!plc->sock) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "No socket or socket is closed.");
        return PLCTAG_ERR_BAD_CONNECTION;
    }

    /* if we have some data in the buffer, update the inactivity timer. */
    if(plc->write_data_len > 0) {
        plc->last_packet_time_ms = time_ms();
        int32_t timeout_val = atomic_get_int32(&plc->connection_inactivity_timeout_ms);
        plc->disconnect_at_time_ms = plc->last_packet_time_ms + timeout_val;
    }

    /* is there anything to do? */
    if(!atomic_get_bool(&plc->flags.request_ready)) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "No packet to send!");
        return PLCTAG_ERR_NO_DATA;
    }

    /* try to send some data. */
    rc = socket_write(plc->sock, plc->write_data + plc->write_data_offset, data_left, SOCKET_WRITE_TIMEOUT);
    if(rc >= 0) {
        plc->write_data_offset += rc;
        data_left = plc->write_data_len - plc->write_data_offset;
    } else if(rc == PLCTAG_ERR_TIMEOUT) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "Done.  Timeout writing to socket.");
    } else {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Error, %s, writing to socket!", plc_tag_decode_error(rc));
        return rc;
    }

    /* clean up if full write was done. */
    if(data_left == 0) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Full packet written.");
        pdebug_dump_bytes(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, plc->write_data, plc->write_data_len);

        /* Update packet timestamp for inactivity tracking */
        plc->last_packet_time_ms = time_ms();
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "Updated last_packet_time_ms=%" PRId64 " (sent full packet).",
               plc->last_packet_time_ms);

        // plc->flags.request_ready = 0;
        plc->write_data_len = 0;
        plc->write_data_offset = 0;

        rc = PLCTAG_STATUS_OK;
    } else {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Partial packet written.");
        rc = PLCTAG_STATUS_PENDING;
    }

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, 0, "Done.");

    return rc;
}


/*
 * Advance the MBAP transaction identifier, skipping zero.
 *
 * Zero is reserved: tag->seq_id == 0 means no request outstanding, so a
 * live request must never carry it. The wrap is
 * done in uint16_t explicitly because a bare ++ on a uint16_t promotes to int
 * and narrows again on store, which -fsanitize=implicit-integer-truncation
 * reports once the value reaches 65535.
 */
static uint16_t next_seq_id(uint16_t current) {
    uint16_t next = (uint16_t)(current + 1);

    if(next == 0) { next = 1; }

    return next;
}


int create_read_request(modbus_plc_p plc, modbus_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    uint16_t seq_id = 0;
    int32_t registers_per_request = (MAX_MODBUS_RESPONSE_PAYLOAD * 8) / tag->elem_size;
    int32_t base_register = tag->reg_base + (tag->request_num * registers_per_request);
    int32_t register_count = tag->elem_count - (tag->request_num * registers_per_request);

    plc->seq_id = next_seq_id(plc->seq_id);
    seq_id = plc->seq_id;

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, tag->tag_id, "Starting.");

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, tag->tag_id, "seq_id=%d", seq_id);
    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, tag->tag_id, "registers_per_request = %d", registers_per_request);
    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, tag->tag_id, "base_register = %d", base_register);
    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, tag->tag_id, "register_count = %d", register_count);

    /* clamp the number of registers we ask for to what will fit. */
    attr_clamp_int32(&register_count, 0, registers_per_request);

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, tag->tag_id,
           "preparing read request for %d registers (of %d total) from base register %d.", register_count, tag->elem_count,
           base_register);

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
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, tag->tag_id, "Unsupported register type %d!", tag->reg_type);
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
    tag->req_fn_code = plc->write_data[7];
    atomic_set_bool(&plc->flags.request_ready, true);

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "Created read request:");
    pdebug_dump_bytes(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, plc->write_data, plc->write_data_len);

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "Done.");

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

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "Starting.");

    if(seq_id == tag->seq_id) {
        uint8_t has_error = plc->read_data[7] & (uint8_t)0x80;

        /* the operation is complete regardless of the outcome. */
        // tag->flags.operation_complete = 1;

        /*
         * The transaction ID only says the server is answering this request.  It does not say
         * it is answering the question we asked: the function code has to match too, or we
         * decode a coil bitmap as register words.  An exception reply sets the high bit and is
         * otherwise the same code.
         */
        if((plc->read_data[7] & (uint8_t)0x7F) != tag->req_fn_code) {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, tag->tag_id, "Got function code %u in response to function code %u!",
                   (unsigned int)(plc->read_data[7] & (uint8_t)0x7F), (unsigned int)tag->req_fn_code);

            plc->read_data_len = 0;
            atomic_set_bool(&plc->flags.response_ready, false);

            tag->seq_id = 0;
            tag->request_num = 0;
            tag->status = (int8_t)PLCTAG_ERR_BAD_DATA;

            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "Done.");

            return PLCTAG_ERR_BAD_DATA;
        }

        if(has_error) {
            rc = translate_modbus_error(plc->read_data[8]);

            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, tag->tag_id, "Got read response %ud, with error %s, of length %d.",
                   (int)(unsigned int)seq_id, plc_tag_decode_error(rc), plc->read_data_len);
        } else {
            int registers_per_request = (MAX_MODBUS_RESPONSE_PAYLOAD * 8) / tag->elem_size;
            int register_offset = (tag->request_num * registers_per_request);
            int byte_offset = (register_offset * tag->elem_size) / 8;
            uint8_t payload_size = plc->read_data[8];
            /* payload_size is wire-supplied; do not trust it past what we actually received. */
            int payload_available = plc->read_data_len - MODBUS_MIN_RESPONSE_SIZE;
            int copy_size = ((tag->size - byte_offset) < payload_size ? (tag->size - byte_offset) : payload_size);

            if(copy_size > payload_available) { copy_size = payload_available; }

            if(copy_size < 0) { copy_size = 0; }

            /* no error. So copy the data. */
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "Got read response %u of length %d with payload of size %d.",
                   (int)(unsigned int)seq_id, plc->read_data_len, payload_size);
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "registers_per_request = %d", registers_per_request);
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "register_offset = %d", register_offset);
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "byte_offset = %d", byte_offset);
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "copy_size = %d", copy_size);

            /*
             * byte_offset comes from our own request numbering, but forming tag->data + byte_offset
             * past the end of the buffer is UB even when copy_size has been clamped to zero.
             */
            if(byte_offset < 0 || byte_offset > tag->size) {
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, tag->tag_id, "Byte offset %d is outside a tag of %d bytes!", byte_offset,
                       tag->size);
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            } else {
                mem_copy(tag->data + byte_offset, &plc->read_data[9], copy_size);

                /* are we done? */
                if(tag->size > (byte_offset + copy_size)) {
                    /* Not yet. */
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "Not done reading entire tag.");
                    partial_read = 1;
                } else {
                    /* read is done. */
                    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "Read is complete.");
                    partial_read = 0;
                }

                rc = PLCTAG_STATUS_OK;
            }
        }

        /* either way, clean up the PLC buffer. */
        plc->read_data_len = 0;
        atomic_set_bool(&plc->flags.response_ready, false);

        /* clean up tag*/
        if(!partial_read) {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, tag->tag_id, "Read is complete.  Cleaning up tag state.");
            tag->seq_id = 0;
            tag->status = (int8_t)rc;
            tag->request_num = 0;
        } else {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, tag->tag_id,
                   "Read is partially complete.  We need to do at least one more request.");
            rc = PLCTAG_ERR_PARTIAL;
            tag->request_num++;
            tag->status = (int8_t)PLCTAG_STATUS_PENDING;
        }
    } else {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, tag->tag_id, "Not our response.");

        rc = PLCTAG_ERR_NO_MATCH;
    }

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, tag->tag_id, "Done: %s", plc_tag_decode_error(rc));

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
    uint16_t seq_id = 0;
    int32_t registers_per_request = (MAX_MODBUS_REQUEST_PAYLOAD * 8) / tag->elem_size;
    int32_t base_register = tag->reg_base + (tag->request_num * registers_per_request);
    int32_t register_count = tag->elem_count - (tag->request_num * registers_per_request);
    int32_t register_offset = (tag->request_num * registers_per_request);
    int32_t byte_offset = (register_offset * tag->elem_size) / 8;
    int32_t request_payload_size = 0;

    plc->seq_id = next_seq_id(plc->seq_id);
    seq_id = plc->seq_id;

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "Starting.");

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, tag->tag_id, "seq_id=%d", seq_id);
    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, tag->tag_id, "registers_per_request = %d", registers_per_request);
    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, tag->tag_id, "base_register = %d", base_register);
    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, tag->tag_id, "register_count = %d", register_count);
    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, tag->tag_id, "register_offset = %d", register_offset);
    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, tag->tag_id, "byte_offset = %d", byte_offset);

    /* clamp the number of registers we ask for to what will fit. */
    attr_clamp_int32(&register_count, 0, registers_per_request);

    /* how many bytes, rounded up to the nearest byte. */
    request_payload_size = ((register_count * tag->elem_size) + 7) / 8;

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id,
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
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, tag->tag_id, "Done. You cannot write a discrete input!");
            return PLCTAG_ERR_UNSUPPORTED;
            break;

        case MB_REG_HOLDING_REGISTER:
            plc->write_data[7] = MB_CMD_WRITE_HOLDING_REGISTER_MULTI;
            plc->write_data_len++;
            break;

        case MB_REG_INPUT_REGISTER:
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, tag->tag_id, "Done. You cannot write an analog input!");
            return PLCTAG_ERR_UNSUPPORTED;
            break;

        default:
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, tag->tag_id, "Done. Unsupported register type %d!", tag->reg_type);
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
    tag->req_fn_code = plc->write_data[7];
    atomic_set_bool(&plc->flags.request_ready, true);

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "Created write request:");
    pdebug_dump_bytes(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, plc->write_data, plc->write_data_len);

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "Done.");

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

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, tag->tag_id, "Starting.");

    if(seq_id == tag->seq_id) {
        uint8_t has_error = plc->read_data[7] & (uint8_t)0x80;

        /* this is our response, so the operation is complete regardless of the status. */
        // tag->flags.operation_complete = 1;

        /* as in the read case, the reply has to name the function we actually asked for. */
        if((plc->read_data[7] & (uint8_t)0x7F) != tag->req_fn_code) {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, tag->tag_id, "Got function code %u in response to function code %u!",
                   (unsigned int)(plc->read_data[7] & (uint8_t)0x7F), (unsigned int)tag->req_fn_code);

            plc->read_data_len = 0;
            atomic_set_bool(&plc->flags.response_ready, false);

            tag->seq_id = 0;
            tag->request_num = 0;
            tag->status = (int8_t)PLCTAG_ERR_BAD_DATA;

            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "Done.");

            return PLCTAG_ERR_BAD_DATA;
        }

        if(has_error) {
            rc = translate_modbus_error(plc->read_data[8]);

            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, tag->tag_id, "Got write response %ud, with error %s, of length %d.",
                   (int)(unsigned int)seq_id, plc_tag_decode_error(rc), plc->read_data_len);
        } else {
            int registers_per_request = (MAX_MODBUS_RESPONSE_PAYLOAD * 8) / tag->elem_size;
            int next_register_offset = ((tag->request_num + 1) * registers_per_request);
            int next_byte_offset = (next_register_offset * tag->elem_size) / 8;

            /* no error. So copy the data. */
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "registers_per_request = %d", registers_per_request);
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "next_register_offset = %d", next_register_offset);
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "next_byte_offset = %d", next_byte_offset);

            /* are we done? */
            if(tag->size > next_byte_offset) {
                /* Not yet. */
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, tag->tag_id, "Not done writing entire tag.");
                partial_write = 1;
            } else {
                /* read is done. */
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "Write is complete.");
                partial_write = 0;
            }

            rc = PLCTAG_STATUS_OK;
        }

        /* either way, clean up the PLC buffer. */
        plc->read_data_len = 0;
        atomic_set_bool(&plc->flags.response_ready, false);

        /* clean up tag*/
        if(!partial_write) {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "Write complete. Cleaning up tag state.");
            tag->seq_id = 0;
            tag->request_num = 0;
            tag->status = (int8_t)rc;
        } else {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id,
                   "Write partially complete.  We need to do at least one more write request.");
            rc = PLCTAG_ERR_PARTIAL;
            tag->request_num++;
            tag->status = (int8_t)PLCTAG_STATUS_PENDING;
        }
    } else {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, tag->tag_id, "Not our response.");

        rc = PLCTAG_STATUS_PENDING;
    }

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}


int translate_modbus_error(uint8_t err_code) {
    int rc = PLCTAG_STATUS_OK;

    switch(err_code) {
        case 0x01:
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "The received function code can not be processed!");
            rc = PLCTAG_ERR_UNSUPPORTED;
            break;

        case 0x02:
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "The data address specified in the request is not available!");
            rc = PLCTAG_ERR_NOT_FOUND;
            break;

        case 0x03:
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "The value contained in the query data field is an invalid value!");
            rc = PLCTAG_ERR_BAD_PARAM;
            break;

        case 0x04:
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0,
                   "An unrecoverable error occurred while the server attempted to perform the requested action!");
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;

        case 0x05:
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "The server will take a long time processing this request!");
            rc = PLCTAG_ERR_PARTIAL;
            break;

        case 0x06:
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "The server is busy!");
            rc = PLCTAG_ERR_BUSY;
            break;

        case 0x07:
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0,
                   "The server can not execute the program function specified in the request!");
            rc = PLCTAG_ERR_UNSUPPORTED;
            break;

        case 0x08:
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "The slave detected a parity error when reading the extended memory!");
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;

        default:
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Unknown error response %u received!", (int)(unsigned int)(err_code));
            rc = PLCTAG_ERR_UNSUPPORTED;
            break;
    }

    return rc;
}


int parse_register_name(attr attribs, modbus_reg_type_t *reg_type, int *reg_base) {
    int rc = PLCTAG_STATUS_OK;
    const char *reg_name = attr_get_str(attribs, "name", NULL);

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "Starting.");

    if(!reg_name || str_length(reg_name) < 3) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Incorrect or unsupported register name!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* see if we can parse the register number. */
    rc = str_to_int(&reg_name[2], reg_base);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Unable to parse register number!");
        *reg_base = 0;
        *reg_type = MB_REG_UNKNOWN;
        return rc;
    }

    /* get the register type. */
    if(str_cmp_i_n(reg_name, "co", 2) == 0) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Found coil type.");
        *reg_type = MB_REG_COIL;
    } else if(str_cmp_i_n(reg_name, "di", 2) == 0) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Found discrete input type.");
        *reg_type = MB_REG_DISCRETE_INPUT;
    } else if(str_cmp_i_n(reg_name, "hr", 2) == 0) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Found holding register type.");
        *reg_type = MB_REG_HOLDING_REGISTER;
    } else if(str_cmp_i_n(reg_name, "ir", 2) == 0) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Found input register type.");
        *reg_type = MB_REG_INPUT_REGISTER;
    } else {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Unknown register type, %s!", reg_name);
        *reg_base = 0;
        *reg_type = MB_REG_UNKNOWN;
        return PLCTAG_ERR_BAD_PARAM;
    }

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}

static int add_tag(modbus_plc_p plc, modbus_tag_p tag) {
    critical_block(plc->mutex) {
        /* Count all tags (active + idle) for terminate detection. */
        atomic_add_int32(&plc->tag_count, 1);

        /* the create read (mb_activate()) puts the tag on the list; auto-sync reads follow it. */
        if(tag->auto_sync_read_ms > 0) { tag->auto_sync_next_read = time_ms(); }
    }

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "Tag added, count now %d.", atomic_get_int32(&plc->tag_count));

    return PLCTAG_STATUS_OK;
}


static int remove_tag(modbus_plc_p plc, modbus_tag_p tag) {
    critical_block(plc->mutex) {
        forget_tag_unsafe(plc, tag);
        atomic_add_int32(&plc->tag_count, -1);
    }

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "Tag removed, count now %d.", atomic_get_int32(&plc->tag_count));

    return PLCTAG_STATUS_OK;
}


/****** Tag Control Functions ******/

/* These must all be called with the API mutex on the tag held. */

static int mb_abort(plc_tag_p p_tag) {
    modbus_tag_p tag = (modbus_tag_p)p_tag;

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "Starting.");

    if(!tag->plc || atomic_get_bool(&tag->plc->flags.terminate)) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "PLC is terminating or null, skipping abort operations.");
        return PLCTAG_STATUS_OK;
    }

    /* the handler cannot be encoding or decoding for the tag while the fields below change. */
    critical_block(tag->plc->mutex) { forget_tag_unsafe(tag->plc, tag); }

    /* the library queued the abort's events (tag_op_abort()) before calling this. */
    tag->seq_id = 0;
    tag->request_num = 0;
    tag->status = (int8_t)PLCTAG_STATUS_OK;
    tag_op_set(tag, TAG_OP_IDLE);

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}


/* wake: the library changed the tag; the handler takes it from here. */
static int mb_queue(plc_tag_p p_tag) {
    queue_tag((modbus_tag_p)p_tag);

    return PLCTAG_STATUS_PENDING;
}


/*
 * read and write: the library has started the op.  plc_tag_read()/plc_tag_write() queue the
 * tag with wake_plc once they let go of the API mutex; the handler's own auto-sync ops are
 * already on its list.
 */
static int mb_submit(plc_tag_p p_tag) {
    (void)p_tag;

    return PLCTAG_STATUS_PENDING;
}


static int mb_tag_status(plc_tag_p p_tag) {
    modbus_tag_p tag = (modbus_tag_p)p_tag;

    if(tag->status != PLCTAG_STATUS_OK) { return tag->status; }

    /* a tag armed for its next auto-sync read is IDLE: nothing the caller waits on is out. */
    return tag_op_busy(tag) ? PLCTAG_STATUS_PENDING : PLCTAG_STATUS_OK;
}


static int mb_activate(plc_tag_p p_tag) {
    modbus_tag_p tag = (modbus_tag_p)p_tag;

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "Starting.");

    /* Publish the tag to the PLC's handler thread now that plc_tag_create_impl has
     * finished initializing every generic field on it. */
    add_tag(tag->plc, tag);

    /* the create read: it pushes no events, and ending it queues CREATED. */
    tag_op_start((plc_tag_p)tag, true);
    queue_tag(tag);

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * New data for an auto-sync write.  A read would overwrite it, so a read queued or on the wire
 * ends here; its reply, if one comes, is discarded.  The driver schedules the write.
 */
static int mb_tag_data_written(plc_tag_p p_tag) {
    modbus_tag_p tag = (modbus_tag_p)p_tag;

    if(tag->auto_sync_write_ms <= 0) { return PLCTAG_STATUS_OK; }

    if(tag_op_is_read(tag)) {
        critical_block(tag->plc->mutex) { forget_tag_unsafe(tag->plc, tag); }
        tag_op_abort((plc_tag_p)tag);
    }

    queue_tag(tag);

    return PLCTAG_STATUS_OK;
}


/****** Data Accessor Functions ******/

/* Attribute accessors for the table below.  An accessor returns a status and never touches
 * tag->status -- the core records it. */

static int32_t mb_get_elem_size(plc_tag_p raw_tag, int32_t *result) {
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    /* the element size is held in bits, but the attribute reports bytes. */
    *result = (int32_t)((tag->elem_size + 7) / 8);

    return PLCTAG_STATUS_OK;
}


static int32_t mb_get_elem_count(plc_tag_p raw_tag, int32_t *result) {
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    *result = (int32_t)tag->elem_count;

    return PLCTAG_STATUS_OK;
}


static int32_t mb_get_connection_status(plc_tag_p raw_tag, int32_t *result) {
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    return conn_get_status(tag->plc ? &tag->plc->watch.status : NULL, result);
}


static int32_t mb_get_connection_inactivity_timeout_ms(plc_tag_p raw_tag, int32_t *result) {
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    return conn_get_inactivity_timeout_ms(tag->plc ? &tag->plc->connection_inactivity_timeout_ms : NULL, result);
}


static int32_t mb_set_connection_inactivity_timeout_ms(plc_tag_p raw_tag, int32_t value) {
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    return conn_set_inactivity_timeout_ms(tag->plc ? &tag->plc->connection_inactivity_timeout_ms : NULL, value, tag->tag_id,
                                          DEBUG_MODULE_MODBUS);
}


static int32_t mb_get_connection_retry_max_delay_ms(plc_tag_p raw_tag, int32_t *result) {
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    return conn_get_retry_max_delay_ms(tag->plc ? &tag->plc->connection_retry_max_delay_ms : NULL, PLC_SOCKET_ERR_MAX_DELAY,
                                       result);
}


static int32_t mb_set_connection_retry_max_delay_ms(plc_tag_p raw_tag, int32_t value) {
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    return conn_set_retry_max_delay_ms(tag->plc ? &tag->plc->connection_retry_max_delay_ms : NULL, value, tag->tag_id,
                                       DEBUG_MODULE_MODBUS);
}


/* Read-only after creation.  These live on the shared PLC, so they report
 * PLCTAG_ERR_NOT_FOUND until it exists. */
static int32_t mb_get_gateway(plc_tag_p raw_tag, uint8_t *buffer, int32_t buffer_length) {
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    if(!tag->plc) { return PLCTAG_ERR_NOT_FOUND; }

    return attr_copy_string(tag->plc->server, buffer, buffer_length);
}


static int32_t mb_get_gateway_size(plc_tag_p raw_tag) {
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    if(!tag->plc) { return PLCTAG_ERR_NOT_FOUND; }

    return attr_string_size(tag->plc->server);
}


/* Modbus has no CIP path; "path" is the unit id of the server. */
static int32_t mb_get_path(plc_tag_p raw_tag, int32_t *result) {
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    if(!tag->plc) { return PLCTAG_ERR_NOT_FOUND; }

    *result = (int32_t)tag->plc->server_id;

    return PLCTAG_STATUS_OK;
}


static int32_t mb_get_max_requests_in_flight(plc_tag_p raw_tag, int32_t *result) {
    modbus_tag_p tag = (modbus_tag_p)raw_tag;

    if(!tag->plc) { return PLCTAG_ERR_NOT_FOUND; }

    *result = (int32_t)tag->plc->max_requests_in_flight;

    return PLCTAG_STATUS_OK;
}


static const attr_def_t mb_attribs[] = {
    {.name = "elem_size",
     .type = ATTR_TYPE_INT,
     .description = "The size in bytes of a single element of this tag.",
     .get_int = mb_get_elem_size},

    {.name = "elem_count",
     .type = ATTR_TYPE_INT,
     .description = "The number of elements this tag holds.",
     .get_int = mb_get_elem_count},

    {.name = "connection_status",
     .type = ATTR_TYPE_INT,
     .description = "The state of the connection this tag uses, as a plc_tag_conn_status_t.",
     .get_int = mb_get_connection_status},

    {.name = "connection_inactivity_timeout_ms",
     .type = ATTR_TYPE_INT,
     .description = "Disconnect from the PLC after this many milliseconds without traffic.",
     .get_int = mb_get_connection_inactivity_timeout_ms,
     .set_int = mb_set_connection_inactivity_timeout_ms},

    {.name = "connection_retry_max_delay_ms",
     .type = ATTR_TYPE_INT,
     .description = "Longest delay between reconnect attempts after an error, in milliseconds.  Shared by every tag on "
                    "this connection.  A change applies from the next retry; a wait already running is not cut short.",
     .get_int = mb_get_connection_retry_max_delay_ms,
     .set_int = mb_set_connection_retry_max_delay_ms},

    {.name = "gateway",
     .type = ATTR_TYPE_STRING,
     .description = "The host name or address of the Modbus server this tag uses.",
     .get_bytes = mb_get_gateway,
     .get_bytes_size = mb_get_gateway_size},

    {.name = "path",
     .type = ATTR_TYPE_INT,
     .description = "The unit id of the Modbus server this tag uses.",
     .get_int = mb_get_path},

    {.name = "max_requests_in_flight",
     .type = ATTR_TYPE_INT,
     .description = "How many requests this tag's PLC will have outstanding at once.",
     .get_int = mb_get_max_requests_in_flight},

    {.name = NULL},
};

static struct tag_vtable_t modbus_vtable = {
    .abort = mb_abort,
    .read = mb_submit,
    .status = mb_tag_status,
    .write = mb_submit,
    .wake_plc = mb_queue,
    .activate = mb_activate,
    .tag_data_written = mb_tag_data_written,

    /* data accessors */
    .attribs = mb_attribs,
};


/****** Modbus connection tag (@connection) implementation *******/


static void mb_plc_set_conn_status(modbus_plc_p plc, int32_t new_status) {
    conn_watch_set_status(&plc->watch, plc->mutex, new_status);
}


static plc_tag_p mb_connection_tag_create(attr attribs,
                                          void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                          void *userdata, plc_tag_p src_tag) {
    connection_tag_args_t args = {
        .protocol_type = TAG_PROTOCOL_MB_CONNECTION, .debug_module = DEBUG_MODULE_MB_CONNECTION, .conn_rc = PLCTAG_STATUS_OK};
    modbus_plc_p plc = NULL;

    pdebug(DEBUG_MODULE_MB_CONNECTION, DEBUG_DETAIL, 0, "Starting.");

    if(src_tag) {
        switch(src_tag->protocol_type) {
            case TAG_PROTOCOL_MODBUS: plc = rc_inc(((modbus_tag_p)src_tag)->plc); break;

            case TAG_PROTOCOL_MB_CONNECTION: plc = rc_inc(((connection_tag_p)src_tag)->conn); break;

            default: plc = NULL; break;
        }

        args.conn_rc = (plc ? PLCTAG_STATUS_OK : PLCTAG_ERR_NOT_ALLOWED);
    } else {
        bool plc_is_new = false;

        args.conn_rc = find_or_create_plc(attribs, &plc, &plc_is_new);
        if(args.conn_rc != PLCTAG_STATUS_OK) { plc = NULL; }

        args.conn_is_new = plc_is_new;
    }

    if(plc) {
        args.conn = plc;
        args.watch = &plc->watch;
        args.conn_mutex = plc->mutex;
    }

    pdebug(DEBUG_MODULE_MB_CONNECTION, DEBUG_DETAIL, 0, "Done.");

    return connection_tag_create(attribs, &args, tag_callback_func, userdata);
}


/****** Library level functions. *******/

void mb_teardown(void) {
    int64_t start_time = 0;
    int64_t timeout_ms = 5000;
    int active_count = 0;
    int64_t elapsed = 0;

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "Starting.");

    if(mb_mutex) {
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Signaling all Modbus PLCs to terminate.");

        /* Signal all PLC handler threads to terminate */
        critical_block(mb_mutex) {
            modbus_plc_p walker = plcs;
            while(walker) {
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Signaling PLC to terminate.");
                atomic_set_bool(&walker->flags.terminate, true);
                wake_plc_thread(walker);
                walker = walker->next;
            }
        }

        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Waiting for all Modbus PLCs to be destroyed.");

        /* Wait for all PLCs to be destroyed using condition variable */
        while(atomic_get_int32(&plc_count) > 0) {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Waiting for %d PLC(s) to be destroyed.", atomic_get_int32(&plc_count));

            /* Wait for signal with timeout */
            int wait_rc = cond_wait(plc_cleanup_cond, 5000); /* 5 second timeout */
            if(wait_rc == PLCTAG_ERR_TIMEOUT) {
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Timeout waiting for PLCs to be destroyed!");
                break;
            }
        }

        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "All Modbus PLCs destroyed.");
    }

    /* Wait for all active handler threads to complete.
     * Use an atomic counter to track active threads.
     * Wait up to 5 seconds (5000 ms) with 20ms polling intervals.
     */
    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Waiting for handler threads to complete.");
    start_time = time_ms();

    while((active_count = atomic_get_int32(&handler_threads_active)) > 0) {
        elapsed = time_ms() - start_time;

        if(elapsed >= timeout_ms) {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Timeout waiting for %d handler threads to complete.", active_count);
            break;
        }

        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Waiting for %d handler threads to complete. Elapsed: %" PRId64 "ms",
               active_count, elapsed);
        sleep_ms(20);
    }

    if(active_count == 0) { pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "All handler threads completed."); }

    /* Destroy the condvar under mb_mutex: modbus_plc_destructor() holds mb_mutex for
     * its decrement-and-signal, so acquiring it here guarantees any signal call that
     * was in flight has fully completed before we free the condvar out from under it.
     * mb_mutex itself must therefore outlive this block; it is destroyed afterward. */
    if(mb_mutex) {
        critical_block(mb_mutex) {
            if(plc_cleanup_cond) {
                pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Destroying cleanup condition variable.");
                cond_destroy(&plc_cleanup_cond);
                plc_cleanup_cond = NULL;
            }
        }
        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Cleanup condition variable destroyed.");

        pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Destroying Modbus mutex.");
        mutex_destroy(&mb_mutex);
        mb_mutex = NULL;
    }
    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Modbus mutex destroyed.");

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "Done.");
}


int mb_init(void) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "Starting.");

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Setting up mutex.");
    if(!mb_mutex) {
        rc = mutex_create(&mb_mutex);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Error %s creating mutex!", plc_tag_decode_error(rc));
            return rc;
        }
    }

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_DETAIL, 0, "Setting up cleanup condition variable.");
    if(!plc_cleanup_cond) {
        rc = cond_create(&plc_cleanup_cond);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_MODBUS, DEBUG_WARN, 0, "Error %s creating cleanup condition!", plc_tag_decode_error(rc));
            return rc;
        }
    }

    pdebug(DEBUG_MODULE_MODBUS, DEBUG_INFO, 0, "Done.");

    return rc;
}
