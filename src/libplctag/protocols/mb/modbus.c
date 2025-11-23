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
#include <utils/vector.h>

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

/* Microsecond timing for performance analysis */
#ifdef _WIN32
/* windows.h already included by platform.h */
static inline int64_t time_us(void) {
    FILETIME ft;
    int64_t res;
    GetSystemTimeAsFileTime(&ft);
    /* FILETIME is in 100ns increments since Jan 1, 1601 */
    res = (int64_t)(ft.dwLowDateTime) + ((int64_t)(ft.dwHighDateTime) << 32);
    /* Convert to microseconds. Magic offset is for Jan 1, 1970 Unix epoch. */
    res = (res - 116444736000000000) / 10;
    return res;
}
#else
#include <sys/time.h>
static inline int64_t time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((int64_t)tv.tv_sec * 1000000) + (int64_t)tv.tv_usec;
}
#endif

typedef struct modbus_tag_t *modbus_tag_p;
typedef struct modbus_tag_list_t *modbus_tag_list_p;

struct modbus_plc_t {
    struct modbus_plc_t *next;

    /* Vector of tags for this PLC */
    vector_p tag_vector;

    /* Count of tags currently attached to this PLC */
    atomic_int32_t tag_count;

    /* Timestamp tracking for inactivity detection */
    int64_t last_packet_time_ms;
    int64_t next_auto_sync_time_ms;

    /* hostname/ip and possibly port of the server. */
    char *server;
    sock_p sock;
    uint8_t server_id;
    int connection_group_id;

    /* State */

    /* FIXME - make these atomic booleans */
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
        PLC_IDLE_WAIT,
        PLC_ERR_WAIT
    } state;
    int max_requests_in_flight;
    int pending_request_count;

    /* Fairness counter - incremented each time a tag is serviced */
    int64_t fairness_counter;

    /* Timing statistics - queue time (request to send) and network time (send to response) */
    int64_t queue_time_min;
    int64_t queue_time_max;
    int64_t queue_time_sum;
    int64_t queue_time_sum_sq;
    int64_t network_time_min;
    int64_t network_time_max;
    int64_t network_time_sum;
    int64_t network_time_sum_sq;
    int64_t timing_sample_count;
    int64_t last_stats_report_time;
    int64_t last_request_sent_time;  /* timestamp when last request was sent */

    /* Cycle timing - measures where time is spent in the main loop (microseconds) */
    int64_t cycle_tickle_time_sum_us;
    int64_t cycle_wait_time_sum_us;
    int64_t cycle_send_time_sum_us;
    int64_t cycle_count;

    /* Tickle breakdown timing (microseconds) */
    int64_t tickle_sort_time_sum_us;
    int64_t tickle_iter_time_sum_us;
    int64_t tickle_overhead_time_sum_us;

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

    /* transaction ID of current pending request (0 = none) */
    uint16_t pending_transaction_id;

    /* timestamp when the operation last changed (for fairness sorting) */
    int64_t op_changed_time;

    /* fairness ticket - set when tag is serviced, used for round-robin ordering */
    int64_t fairness_ticket;

    /* reads_completed counter - used to ensure all tags get at least one read before any gets a second */
    int reads_completed;

    /* request timing - tracks time from entering REQUEST state to receiving response */
    int64_t request_start_time;
    int64_t request_sent_time;  /* timestamp when request was sent over wire */

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

/* Track active handler threads for proper shutdown synchronization */
static atomic_int32_t handler_threads_active = ATOMIC_INT_STATIC_INIT;


/* helper functions */
static int create_tag_object(attr attribs, modbus_tag_p *tag);
static int find_or_create_plc(attr attribs, modbus_plc_p *plc);
static int parse_register_name(attr attribs, modbus_reg_type_t *reg_type, int *reg_base);
static void modbus_tag_destructor(void *tag_arg);
static void modbus_plc_destructor(void *plc_arg);
static THREAD_FUNC(modbus_plc_handler);
static void wake_plc_thread(modbus_plc_p plc);
static int connect_plc(modbus_plc_p plc);
static int tickle_all_tags(modbus_plc_p plc, int64_t *out_wait_time_ms);
static int tickle_tag(modbus_plc_p plc, modbus_tag_p tag, int64_t now, int64_t *min_wait_time);
static int receive_response(modbus_plc_p plc);
static int send_request(modbus_plc_p plc);
static int check_read_response(modbus_plc_p plc, modbus_tag_p tag);
static int create_read_request(modbus_plc_p plc, modbus_tag_p tag);
static int check_write_response(modbus_plc_p plc, modbus_tag_p tag);
static int create_write_request(modbus_plc_p plc, modbus_tag_p tag);
static int translate_modbus_error(uint8_t err_code);
static const char *op_to_str(tag_op_type_t op);

/* tag list functions */
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

    /* initialize the pending transaction ID */
    (*tag)->pending_transaction_id = 0;

    /* initialize the operation changed time for fairness sorting */
    (*tag)->op_changed_time = time_ms();

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
        /* Keep searching until we find a valid PLC or run out of existing plc objects */
        modbus_plc_p *walker = &plcs;

        /* we have to match and the reference count must be > 0 */
        while(*walker && ((*walker)->connection_group_id != connection_group_id
                || (*walker)->server_id != (uint8_t)(unsigned int)server_id 
                || str_cmp_i(server, (*walker)->server) != 0 
                || !rc_inc(*walker))) {

            pdebug(DEBUG_DETAIL, "walking past PLC: connection_group_id=%d, server_id=%d, server=%s", 
                                (*walker)->connection_group_id,
                                (*walker)->server_id, 
                                (*walker)->server);

            walker = &((*walker)->next);
        }

        pdebug(DEBUG_DETAIL, "Finished walking PLC list walker=%p.", (void *)*walker);

        if(*walker) {
            pdebug(DEBUG_DETAIL, "Found matching PLC: connection_group_id=%d, server_id=%d, server=%s", 
                                (*walker)->connection_group_id,                
                                (*walker)->server_id, 
                                (*walker)->server);

            /* we have taken a reference above when walking the list */

            is_new = 0;
            rc = PLCTAG_STATUS_OK;

            *plc = *walker;

            /* leave the critical section, we found a match. */
            break;
        }

        /* we did not find a matching PLC, create a new one. */
        pdebug(DEBUG_DETAIL, "No matching PLC found, creating a new one.");

        is_new = 1;

        /* No matching PLC found in list, will create a new one */
        pdebug(DEBUG_DETAIL, "Creating new PLC connection.");

        pdebug(DEBUG_DETAIL, "connection_group_id=%d, server_id=%d, server=%s", connection_group_id, server_id, server);

        /* Allocate and initialize the PLC object inside the mutex.
            * This prevents duplicate creation when multiple threads race to create the same PLC.
            * Note: rc_alloc() already zero-initializes the memory. */
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

                /* create the tag vector */
                (*plc)->tag_vector = vector_create(16, 16);
                if(!(*plc)->tag_vector) {
                    pdebug(DEBUG_WARN, "Unable to create tag vector!");
                    rc = PLCTAG_ERR_NO_MEM;
                }

                /* initialize timestamp tracking */
                (*plc)->last_packet_time_ms = time_ms();
                (*plc)->next_auto_sync_time_ms = 0;

                /* create the PLC mutex to protect the tag list. */
                rc = mutex_create(&((*plc)->mutex));
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Unable to create new mutex, error %s!", plc_tag_decode_error(rc));
                    rc = PLCTAG_ERR_MUTEX_INIT;
                } else {
                    /* set up the maximum request depth. */
                    (*plc)->max_requests_in_flight = max_requests_in_flight;

                    /* Initialize PLC state before making it visible to other threads */
                    (*plc)->state = PLC_CONNECT_START;
                    (*plc)->inactivity_timeout_ms = MODBUS_INACTIVITY_TIMEOUT + time_ms();

                    /* Add the new PLC to the global list. We already have the mutex,
                        * so no duplicate can be created by another thread. The struct is
                        * fully initialized and the mutex exists, so other threads can
                        * safely find and use this PLC. */
                    (*plc)->next = plcs;
                    plcs = *plc;
                    pdebug(DEBUG_DETAIL, "New PLC added to the global PLC list.");
                }
            }
        } else {
            pdebug(DEBUG_WARN, "Unable to allocate Modbus PLC object!");
            rc = PLCTAG_ERR_NO_MEM;
        }
    }

    /* if everything went well and it is new, set up the new PLC. */
    if(rc == PLCTAG_STATUS_OK && is_new) {
        pdebug(DEBUG_INFO, "Initializing new PLC.");

        do {
            /* 
             * With deferred cleanup, the handler thread does NOT need to hold an explicit
             * reference to the PLC. The tags hold the references through tag->plc pointers.
             * When the handler thread exits, it will not decrement the refcount, so the PLC
             * will stay alive as long as tags reference it. When the last tag is destroyed,
             * its destructor will release the final PLC reference and trigger PLC destruction
             * via deferred cleanup.
             */
            pdebug(DEBUG_DETAIL, "Handler thread will reference PLC through task parameter (no explicit rc_inc).");

            rc = thread_create(&((*plc)->handler_thread), modbus_plc_handler, 32768 /* ignored */, (void *)(*plc));
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unable to create new handler thread, error %s!", plc_tag_decode_error(rc));
                break;
            }

            pdebug(DEBUG_DETAIL, "Created thread %p.", (*plc)->handler_thread);

            /* Increment PLC count for lifecycle tracking */
            atomic_add_int32(&plc_count, 1);

            pdebug(DEBUG_DETAIL, "PLC created, count now %d.", atomic_get_int32(&plc_count));
        } while(0);
    }

    if(rc != PLCTAG_STATUS_OK && *plc) {
        pdebug(DEBUG_WARN, "PLC lookup or creation failed!");

        /* clean up. */
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing the reference to the PLC due to creation error!");
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

    /* destroy the tag vector */
    if(plc->tag_vector) {
        if(vector_length(plc->tag_vector) > 0) {
            pdebug(DEBUG_WARN, "There are tags still remaining in the tag list, memory leak possible!");
        }
        vector_destroy(plc->tag_vector);
        plc->tag_vector = NULL;
    }

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
    pdebug(DEBUG_INFO, "Starting.");

    if(!plc) {
        pdebug(DEBUG_WARN, "Null PLC pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!plc->mutex) {
        pdebug(DEBUG_WARN, "PLC mutex is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_WARN, "MUTEX: Acquire. Resetting PLC.");
    critical_block(plc->mutex) {
        /* Clear all pending requests and reset tags to allow retry since the server may have been restarted */
        pdebug(DEBUG_DETAIL, "Clearing all pending transaction IDs due to socket disconnect. In-flight was: %d",
               plc->pending_request_count);
        int tag_count = vector_length(plc->tag_vector);
        for(int i = 0; i < tag_count; i++) {
            modbus_tag_p cur = vector_get(plc->tag_vector, i);
            if(!cur) continue;

            if(cur->op == TAG_OP_READ_RESPONSE || cur->op == TAG_OP_WRITE_RESPONSE) {
                pdebug(DEBUG_DETAIL, "Resetting tag %" PRId32 " from %s to request state due to socket disconnect.",
                        cur->tag_id, op_to_str(cur->op));

                /* If this tag had a pending request, clear it and decrement counter */
                if(cur->pending_transaction_id != 0) {
                    cur->pending_transaction_id = 0;
                    if(plc->pending_request_count > 0) {
                        plc->pending_request_count--;
                    }
                }

                /* Reset to the corresponding REQUEST state to allow retry on reconnect */
                if(cur->op == TAG_OP_READ_RESPONSE) {
                    cur->op = TAG_OP_READ_REQUEST;
                } else {
                    cur->op = TAG_OP_WRITE_REQUEST;
                }
            }
        }

        pdebug(DEBUG_DETAIL, "After reset, in-flight count is: %d", plc->pending_request_count);

        if(plc->sock) {
            pdebug(DEBUG_DETAIL, "Closing socket due to error or disconnect.");
            socket_close(plc->sock);
        } else {
            pdebug(DEBUG_DETAIL, "Socket already closed.");
        }

        /* set up the state. */
        plc->flags.response_ready = 0;
        plc->flags.request_ready = 0;
        plc->read_data_len = 0;
        plc->write_data_len = 0;
        plc->write_data_offset = 0;
    }
    pdebug(DEBUG_WARN, "MUTEX: Release. PLC reset.");

    pdebug(DEBUG_INFO, "Done.");

    /* Wake the PLC handler thread to process the reset state immediately */
    wake_plc_thread(plc);

    return PLCTAG_STATUS_OK;
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

    /* Increment the count of active handler threads */
    atomic_add_int32(&handler_threads_active, 1);

    while(!plc->flags.terminate && atomic_get_bool(&lib_active)) {
        int64_t wait_time_ms = MODBUS_IDLE_WAIT_TIMEOUT;
        int64_t tickle_start_us, tickle_end_us;

        tickle_start_us = time_us();
        rc = tickle_all_tags(plc, &wait_time_ms);
        tickle_end_us = time_us();
        plc->cycle_tickle_time_sum_us += (tickle_end_us - tickle_start_us);

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error %s tickling tags!", plc_tag_decode_error(rc));
            /* FIXME - what should we do here? */
        }

        /* if there is still a response marked ready, clean it up.
         * We must protect this with the mutex because responses can be received
         * while tickle_all_tags() is running. A tag's response might arrive
         * after its tag_op_read_response() check but before tickle_all_tags()
         * completes, so we need atomic access to the flag. */
        critical_block(plc->mutex) {
            if(plc->flags.response_ready) {
                pdebug(DEBUG_DETAIL, "Orphan response found.");
                plc->flags.response_ready = 0;
                plc->read_data_len = 0;
            }
        }

        switch(plc->state) {
            case PLC_CONNECT_START:
                pdebug(DEBUG_DETAIL, "in PLC_CONNECT_START state.");

                /* reset the PLC to initial state, including closing the socket */
                reset_plc(plc);

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

                /* wait using calculated time from tickle_all_tags */
                pdebug(DEBUG_DETAIL, "Waiting up to %" PRId64 "ms for socket events.", wait_time_ms);
                {
                    int64_t wait_start_us = time_us();
                    sock_events = socket_wait_event(plc->sock, waitable_events, (int)wait_time_ms);
                    plc->cycle_wait_time_sum_us += (time_us() - wait_start_us);
                }
                if(sock_events & SOCK_EVENT_TIMEOUT) {
                    int64_t current_time = time_ms();
                    int64_t idle_time = current_time - plc->last_packet_time_ms;

                    pdebug(DEBUG_DETAIL, "Socket wait timed out. Idle for %" PRId64 "ms.", idle_time);

                    /* Only disconnect if truly idle for full timeout period */
                    if(idle_time >= MODBUS_IDLE_WAIT_TIMEOUT) {
                        pdebug(DEBUG_DETAIL, "Inactivity timeout reached, going to PLC_IDLE_WAIT.");

                        /* reset the PLC state */
                        reset_plc(plc);

                        /* go to the state where we wait for something to happen. */
                        plc->state = PLC_IDLE_WAIT;
                    } else {
                        /* Timeout was for auto-sync, continue immediately */
                        pdebug(DEBUG_DETAIL, "Auto-sync timeout, continuing.");
                    }
                }

                /* check for socket errors or disconnects. */
                if((sock_events & SOCK_EVENT_ERROR) || (sock_events & SOCK_EVENT_DISCONNECT)) {
                    if(sock_events & SOCK_EVENT_DISCONNECT) {
                        pdebug(DEBUG_WARN, "Unexepected socket disconnect!");
                    } else {
                        pdebug(DEBUG_WARN, "Unexpected socket error!");
                    }

                    pdebug(DEBUG_WARN, "Going to state PLC_CONNECT_START");

                    /* try to reconnect immediately */
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

                {
                    int64_t send_start_us = time_us();
                    rc = send_request(plc);
                    plc->cycle_send_time_sum_us += (time_us() - send_start_us);
                }
                if(rc == PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_DETAIL, "Request sent, going to back to state PLC_READY.");
                    plc->cycle_count++;

                    plc->flags.request_ready = 0;
                    plc->write_data_len = 0;
                    plc->write_data_offset = 0;

                    plc->state = PLC_READY;
                } else if(rc == PLCTAG_STATUS_PENDING) {
                    pdebug(DEBUG_DETAIL, "Not all data written, will try again.");
                } else {
                    pdebug(DEBUG_WARN, "Resetting PLC due to write error %s.", plc_tag_decode_error(rc));

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
                    pdebug(DEBUG_WARN, "Reconnecting due to read error %s.", plc_tag_decode_error(rc));

                    /* try to reconnect immediately. */
                    plc->state = PLC_CONNECT_START;
                }

                /* in all cases we want to cycle through the state machine immediately. */

                break;


            case PLC_IDLE_WAIT:
                pdebug(DEBUG_DETAIL, "in PLC_IDLE_WAIT state.");

                /* wait until something happens. */
                sock_events = socket_wait_event(plc->sock, SOCK_EVENT_DEFAULT_MASK, MODBUS_IDLE_WAIT_TIMEOUT);

                if(sock_events & SOCK_EVENT_WAKE_UP) {
                    pdebug(DEBUG_DETAIL, "PLC woke up.");
                    plc->state = PLC_CONNECT_START;
                } else if(sock_events & SOCK_EVENT_TIMEOUT) {
                    pdebug(DEBUG_DETAIL, "PLC idle wait timed out.");
                }

                break;

            case PLC_ERR_WAIT:
                pdebug(DEBUG_DETAIL, "in PLC_ERR_WAIT state.");

                /* wait until done. */
                if(err_delay_until > time_ms()) {
                    pdebug(DEBUG_DETAIL, "Waiting for at least %" PRId64 "ms.", (err_delay_until - time_ms()));
                    socket_wait_event(plc->sock, SOCK_EVENT_WAKE_UP |SOCK_EVENT_TIMEOUT, (int)(err_delay_until - time_ms()));
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

    /* With deferred cleanup thread, we do NOT call rc_dec() here.
     * The tags hold references to the PLC through their tag->plc pointers.
     * When each tag is destroyed, its destructor calls rc_dec() on the PLC.
     * The last tag's destructor will trigger the PLC destructor via deferred cleanup.
     *
     * If we called rc_dec() here, we could trigger PLC destruction while teardown
     * code is still running and trying to access the PLC (race condition).
     */
    pdebug(DEBUG_DETAIL, "Handler thread exiting without decrementing PLC reference (deferred cleanup will handle it).");

    /* Decrement the count of active handler threads */
    atomic_add_int32(&handler_threads_active, -1);

    THREAD_RETURN(0);
}


void wake_plc_thread(modbus_plc_p plc) {
    modbus_plc_p plc_ref = NULL;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(plc) {
        /* Take a reference to the PLC to safely access it */
        plc_ref = rc_inc(plc);

        if(!plc_ref) {
            /* PLC refcount already hit zero, it's being destroyed or already destroyed */
            pdebug(DEBUG_DETAIL, "PLC reference count is zero, cannot wake (PLC is being destroyed).");
            pdebug(DEBUG_DETAIL, "Done.");
            return;
        }

        /* Check if PLC is terminating to avoid accessing freed mutex */
        if(plc_ref->flags.terminate) {
            pdebug(DEBUG_DETAIL, "PLC is terminating, skipping wake.");
            plc_ref = rc_dec(plc_ref);
            pdebug(DEBUG_DETAIL, "Done.");
            return;
        }

        /*
         * No mutex needed here - reference counting ensures the socket won't be
         * destroyed while we hold a reference. socket_destroy() is only called
         * in the PLC destructor, which can't run until all references are released.
         * Multiple threads calling socket_wake() concurrently is safe.
         */
        if(plc_ref->sock) {
            socket_wake(plc_ref->sock);
        } else {
            pdebug(DEBUG_DETAIL, "PLC socket pointer is NULL.");
        }

        /* Release the reference */
        plc_ref = rc_dec(plc_ref);
    } else {
        pdebug(DEBUG_WARN, "PLC pointer is NULL!");
    }

    pdebug(DEBUG_DETAIL, "Done.");
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

    pdebug(DEBUG_DETAIL, "Starting.");


    pdebug(DEBUG_DETAIL, "Parsing server host and port from server string.");

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

    if(!plc->sock) {
        pdebug(DEBUG_DETAIL, "Creating new socket.");
        rc = socket_create(&(plc->sock));
        if(rc != PLCTAG_STATUS_OK) {
            /* done with the split string. */
            mem_free(server_port);
            server_port = NULL;

            pdebug(DEBUG_WARN, "Unable to create socket object, error %s!", plc_tag_decode_error(rc));
            return rc;
        }
    }

    /* connect to the socket */
    pdebug(DEBUG_DETAIL, "Connecting to %s on port %d...", server, port);
    rc = socket_connect_tcp_start(plc->sock, server, port);
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        /* done with the split string. */
        mem_free(server_port);

        pdebug(DEBUG_WARN, "Unable to connect to the server \"%s\", got error %s!", plc->server, plc_tag_decode_error(rc));
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



int tickle_all_tags(modbus_plc_p plc, int64_t *out_wait_time_ms) {
    int rc = PLCTAG_STATUS_OK;
    modbus_tag_p tag = NULL;
    int processed_count = 0;
    int64_t min_wait_time = MODBUS_IDLE_WAIT_TIMEOUT;
    int64_t now = time_ms();
    int64_t iter_start_us;

    pdebug(DEBUG_DETAIL, "Starting.");

    /*
     * Process all tags within the PLC mutex for safety and fairness.
     * Since wake_plc_thread() no longer requires the mutex, callbacks
     * from tickle_tag() won't cause deadlock.
     *
     * Round-robin fairness: always process the first tag, then move it
     * to the end of the list. This ensures each tag gets equal opportunity
     * regardless of throughput limitations.
     */
    iter_start_us = time_us();
    critical_block(plc->mutex) {
        int last_tag_index = vector_length(plc->tag_vector);

        for(int i = 0; i < last_tag_index; i++) {
            /* Get tag at current index */
            modbus_tag_p candidate = vector_get(plc->tag_vector, i);

            /* Try to increment reference - skip if being destroyed */
            if(candidate && rc_inc(candidate)) {
                tag = candidate;

                debug_set_tag_id(tag->tag_id);

                /* the tag mutex may be locked already, so avoid deadlock. */
                if(mutex_try_lock(tag->api_mutex) == PLCTAG_STATUS_OK) {
                    tag_op_type_t prev_op = tag->op;

                    /* tickle_tag updates min_wait_time via check_tag_auto_read/write */
                    rc = tickle_tag(plc, tag, now, &min_wait_time);
                    if(rc == PLCTAG_STATUS_PENDING) {
                        rc = PLCTAG_STATUS_OK;
                    } else if(rc != PLCTAG_STATUS_OK) {
                        pdebug(DEBUG_WARN, "Error %s tickling tag!", plc_tag_decode_error(rc));
                    }

                    /*
                    * Move the tag to the end of the list for round-robin fairness.
                    * This ensures tags that just started an operation give other tags a chance.
                    * Only do this when starting a new operation (IDLE -> READ_REQUEST or WRITE_REQUEST).
                    */
                    if(prev_op == TAG_OP_IDLE && (tag->op == TAG_OP_READ_REQUEST || tag->op == TAG_OP_WRITE_REQUEST)) {
                        pdebug(DEBUG_DETAIL, "Moving tag %d to end of list for fairness (started %s).",
                            tag->tag_id, op_to_str(tag->op));

                        /* remove the tag from the current location */
                        vector_remove(plc->tag_vector, i);

                        /* append it to the end */
                        vector_insert(plc->tag_vector, vector_length(plc->tag_vector), tag);

                        i--; /* Adjust index since we removed current tag */

                        last_tag_index--; /* move down the last tag index, so we do not process the tag twice */
                    }


                    mutex_unlock(tag->api_mutex);
                } else {
                    pdebug(DEBUG_SPEW, "Tag API mutex is already taken, skipping tickle.");
                }

                debug_set_tag_id(0);

                /* Release our reference */
                tag = rc_dec(tag);
            }

            processed_count++;
        }

        /* After iterating all tags, check if response_ready flag is still set.
         * If it is, it means no tag matched the transaction ID in the response.
         * In that case, discard the response and clear the flag for the next response.
         * IMPORTANT: We must also decrement pending_request_count since this response
         * corresponds to a request that was sent (and incremented the count). */
        if(plc->flags.response_ready) {
            pdebug(DEBUG_WARN, "No tag matched transaction ID %u. Discarding.", 
                   (plc->read_data_len >= 2) ? (uint16_t)((uint16_t)plc->read_data[1] + (uint16_t)(plc->read_data[0] << 8)) : 0);
            plc->flags.response_ready = 0;
            plc->read_data_len = 0;

            /* Decrement pending request count since we're discarding this response */
            if(plc->pending_request_count > 0) {
                plc->pending_request_count--;
                pdebug(DEBUG_WARN, "Decremented pending_request_count to %d after discarding unmatched response.",
                       plc->pending_request_count);
            }
        }

        /* Store auto-sync tracking info */
        plc->next_auto_sync_time_ms = (min_wait_time < MODBUS_IDLE_WAIT_TIMEOUT) ? (now + min_wait_time) : 0;
    }
    plc->tickle_iter_time_sum_us += (time_us() - iter_start_us);

    pdebug(DEBUG_SPEW, "Processed %d tags.", processed_count);

    /* Return calculated wait time */
    *out_wait_time_ms = min_wait_time;

    pdebug(DEBUG_SPEW, "Done: %s, wait time %" PRId64 "ms", plc_tag_decode_error(rc), min_wait_time);

    return rc;
}


static int tag_op_read_request(modbus_plc_p plc, modbus_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting read request operation for tag %d. In-flight: %d/%d.",
           tag->tag_id, plc->pending_request_count, plc->max_requests_in_flight);

    /* Pre-flight checks */
    if(plc->flags.request_ready) {
        pdebug(DEBUG_SPEW, "Request already queued for sending.");
        return PLCTAG_STATUS_PENDING;
    }

    if(plc->state != PLC_READY) {
        pdebug(DEBUG_SPEW, "PLC not ready.");
        return PLCTAG_STATUS_PENDING;
    }

    if(tag->tag_id == 0) {
        pdebug(DEBUG_SPEW, "Tag not ready.");
        return PLCTAG_STATUS_PENDING;
    }

    /* Check if we've hit the concurrency limit */
    if(plc->pending_request_count >= plc->max_requests_in_flight) {
        pdebug(DEBUG_SPEW, "Request concurrency limit reached (%d/%d). Waiting for response.",
               plc->pending_request_count, plc->max_requests_in_flight);
        return PLCTAG_STATUS_PENDING;
    }

    /* Create the read request (this sets tag->seq_id) */
    rc = create_read_request(plc, tag);
    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_SPEW, "Read request created with transaction_id=%d.", tag->seq_id);

        /* Store the transaction ID in the tag for matching responses */
        tag->pending_transaction_id = tag->seq_id;

        /* Increment in-flight counter */
        plc->pending_request_count++;
        pdebug(DEBUG_SPEW, "Incremented request count to %d/%d.",
               plc->pending_request_count, plc->max_requests_in_flight);

        /* Assign fairness ticket - this tag just got serviced, goes to back of queue */
        tag->fairness_ticket = plc->fairness_counter++;

        tag->op = TAG_OP_READ_RESPONSE;
        tag->op_changed_time = time_ms();
        plc->flags.request_ready = 1;

        rc = PLCTAG_STATUS_PENDING;
    } else {
        pdebug(DEBUG_WARN, "Error %s creating read request!", plc_tag_decode_error(rc));

        tag->op = TAG_OP_IDLE;
        tag->op_changed_time = time_ms();
        tag->read_complete = 1;
        tag->read_in_flight = 0;
        tag->status = (int8_t)rc;

        tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_READ_COMPLETED, (int8_t)rc);

        pdebug(DEBUG_SPEW, "Read completed event raised for tag %d with status %s.",
               tag->tag_id, plc_tag_decode_error((int8_t)rc));

        rc = PLCTAG_STATUS_OK;
    }

    return rc;
}


static int tag_op_read_response(modbus_plc_p plc, modbus_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    int response_ready = 0;

    pdebug(DEBUG_SPEW, "Starting read response check operation for tag %d.", tag->tag_id);

    /* cross check the state. */
    if(plc->state == PLC_CONNECT_START || plc->state == PLC_CONNECT_WAIT || plc->state == PLC_ERR_WAIT) {
        pdebug(DEBUG_WARN, "PLC changed state, restarting request.");
        tag->op = TAG_OP_READ_REQUEST;
        tag->op_changed_time = time_ms();
        tag->request_start_time = time_ms();
        return PLCTAG_STATUS_OK;
    }

    /* Check response_ready flag - the PLC mutex is already held by the caller
     * (tickle_all_tags) so we can access this directly. */
    response_ready = plc->flags.response_ready;

    if(response_ready) {
        pdebug(DEBUG_DETAIL, "Read response ready for tag %d.", tag->tag_id);

        rc = check_read_response(plc, tag);
        switch(rc) {
            case PLCTAG_ERR_PARTIAL:
            /* FIXME - this is probably not correct.  Why would we reset the response_ready flag on a partial response? */
                /* partial response, keep going */
                pdebug(DEBUG_DETAIL, "Found our response, but we are not done.");

                /* Decrement in-flight counter */
                if(plc->pending_request_count > 0) {
                    plc->pending_request_count--;
                    pdebug(DEBUG_DETAIL, "Decremented request count to %d/%d.",
                           plc->pending_request_count, plc->max_requests_in_flight);
                } else {
                    pdebug(DEBUG_WARN, "Attempted to decrement request count below 0!");
                }

                /* PLC mutex already held by caller (tickle_all_tags) */
                plc->flags.response_ready = 0;

                tag->pending_transaction_id = 0;
                tag->op = TAG_OP_READ_REQUEST;
                tag->op_changed_time = time_ms();
                tag->request_start_time = time_ms();

                rc = PLCTAG_STATUS_PENDING;
                break;

            case PLCTAG_ERR_NO_MATCH:
                pdebug(DEBUG_SPEW, "Not our response.");
                /* Do NOT clear response_ready - let other tags check for their response */
                rc = PLCTAG_STATUS_PENDING;
                break;

            case PLCTAG_STATUS_OK:
                /* fall through */
            default:
                /* set the status before we might change it. */
                tag->status = (int8_t)rc;

                if(rc == PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_DETAIL, "Found our response.");
                } else {
                    pdebug(DEBUG_WARN, "Error %s checking read response!", plc_tag_decode_error(rc));
                    rc = PLCTAG_STATUS_OK;
                }

                /* Decrement in-flight counter */
                if(plc->pending_request_count > 0) {
                    plc->pending_request_count--;
                    pdebug(DEBUG_DETAIL, "Decremented request count to %d/%d.",
                           plc->pending_request_count, plc->max_requests_in_flight);
                } else {
                    pdebug(DEBUG_WARN, "Attempted to decrement request count below 0!");
                }

                /* PLC mutex already held by caller (tickle_all_tags) */
                plc->flags.response_ready = 0;

                tag->pending_transaction_id = 0;
                tag->op = TAG_OP_IDLE;
                tag->op_changed_time = time_ms();
                tag->read_in_flight = 0;
                tag->read_complete = 1;
                tag->reads_completed++;
                tag->status = (int8_t)rc;

                /* Calculate and accumulate timing statistics */
                {
                    int64_t now = time_ms();
                    int64_t queue_time = plc->last_request_sent_time - tag->request_start_time;
                    int64_t network_time = now - plc->last_request_sent_time;

                    /* Update statistics */
                    if(plc->timing_sample_count == 0) {
                        /* First sample - initialize min/max */
                        plc->queue_time_min = queue_time;
                        plc->queue_time_max = queue_time;
                        plc->network_time_min = network_time;
                        plc->network_time_max = network_time;
                    } else {
                        if(queue_time < plc->queue_time_min) plc->queue_time_min = queue_time;
                        if(queue_time > plc->queue_time_max) plc->queue_time_max = queue_time;
                        if(network_time < plc->network_time_min) plc->network_time_min = network_time;
                        if(network_time > plc->network_time_max) plc->network_time_max = network_time;
                    }
                    plc->queue_time_sum += queue_time;
                    plc->queue_time_sum_sq += queue_time * queue_time;
                    plc->network_time_sum += network_time;
                    plc->network_time_sum_sq += network_time * network_time;
                    plc->timing_sample_count++;

                    /* Report statistics every second */
                    if(now - plc->last_stats_report_time >= 1000) {
                        int64_t n = plc->timing_sample_count;
                        if(n > 0) {
                            int64_t q_avg = plc->queue_time_sum / n;
                            int64_t n_avg = plc->network_time_sum / n;
                            /* Variance = E[X^2] - E[X]^2 */
                            int64_t q_var = (plc->queue_time_sum_sq / n) - (q_avg * q_avg);
                            int64_t n_var = (plc->network_time_sum_sq / n) - (n_avg * n_avg);

                            pdebug(DEBUG_INFO, "TIMING STATS (%" PRId64 " samples): Queue: min=%" PRId64 " max=%" PRId64 " avg=%" PRId64 " var=%" PRId64 "ms | Network: min=%" PRId64 " max=%" PRId64 " avg=%" PRId64 " var=%" PRId64 "ms",
                                   n, plc->queue_time_min, plc->queue_time_max, q_avg, q_var,
                                   plc->network_time_min, plc->network_time_max, n_avg, n_var);

                            /* Report cycle timing breakdown in microseconds */
                            if(plc->cycle_count > 0) {
                                int64_t avg_tickle_us = plc->cycle_tickle_time_sum_us / plc->cycle_count;
                                int64_t avg_wait_us = plc->cycle_wait_time_sum_us / plc->cycle_count;
                                int64_t avg_send_us = plc->cycle_send_time_sum_us / plc->cycle_count;
                                pdebug(DEBUG_INFO, "CYCLE BREAKDOWN (%" PRId64 " cycles): tickle=%" PRId64 "us wait=%" PRId64 "us send=%" PRId64 "us total=%" PRId64 "us",
                                       plc->cycle_count, avg_tickle_us, avg_wait_us, avg_send_us, avg_tickle_us + avg_wait_us + avg_send_us);

                                /* Report tickle breakdown: sort vs iteration */
                                int64_t avg_sort_us = plc->tickle_sort_time_sum_us / plc->cycle_count;
                                int64_t avg_iter_us = plc->tickle_iter_time_sum_us / plc->cycle_count;
                                pdebug(DEBUG_INFO, "TICKLE BREAKDOWN: sort=%" PRId64 "us iter=%" PRId64 "us (sort %.1f%%, iter %.1f%%)",
                                       avg_sort_us, avg_iter_us,
                                       avg_tickle_us > 0 ? (100.0 * avg_sort_us / avg_tickle_us) : 0.0,
                                       avg_tickle_us > 0 ? (100.0 * avg_iter_us / avg_tickle_us) : 0.0);
                            }
                        }
                        plc->last_stats_report_time = now;
                    }
                }

                tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_READ_COMPLETED, (int8_t)rc);

                pdebug(DEBUG_DETAIL, "Read completed event raised for tag %d with status %s.",
                       tag->tag_id, plc_tag_decode_error((int8_t)rc));

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

    pdebug(DEBUG_DETAIL, "Starting write request operation for tag %d. In-flight: %d/%d.",
           tag->tag_id, plc->pending_request_count, plc->max_requests_in_flight);

    /* Pre-flight checks */
    if(plc->flags.request_ready) {
        pdebug(DEBUG_DETAIL, "Request already queued for sending.");
        return PLCTAG_STATUS_PENDING;
    }

    if(plc->state != PLC_READY) {
        pdebug(DEBUG_DETAIL, "PLC not ready.");
        return PLCTAG_STATUS_PENDING;
    }

    if(tag->tag_id == 0) {
        pdebug(DEBUG_DETAIL, "Tag not ready.");
        return PLCTAG_STATUS_PENDING;
    }

    /* Check if we've hit the concurrency limit */
    if(plc->pending_request_count >= plc->max_requests_in_flight) {
        pdebug(DEBUG_DETAIL, "Request concurrency limit reached (%d/%d). Waiting for response.",
               plc->pending_request_count, plc->max_requests_in_flight);
        return PLCTAG_STATUS_PENDING;
    }

    /* Create the write request (this sets tag->seq_id) */
    rc = create_write_request(plc, tag);
    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Write request created with transaction_id=%d.", tag->seq_id);

        /* Store the transaction ID in the tag for matching responses */
        tag->pending_transaction_id = tag->seq_id;

        /* Increment in-flight counter */
        plc->pending_request_count++;
        pdebug(DEBUG_SPEW, "Incremented request count to %d/%d.",
               plc->pending_request_count, plc->max_requests_in_flight);

        /* Assign fairness ticket - this tag just got serviced, goes to back of queue */
        tag->fairness_ticket = plc->fairness_counter++;

        tag->op = TAG_OP_WRITE_RESPONSE;
        tag->op_changed_time = time_ms();
        plc->flags.request_ready = 1;

        rc = PLCTAG_STATUS_PENDING;
    } else {
        pdebug(DEBUG_WARN, "Error %s creating write request!", plc_tag_decode_error(rc));

        tag->op = TAG_OP_IDLE;
        tag->op_changed_time = time_ms();
        tag->write_complete = 1;
        tag->write_in_flight = 0;
        tag->status = (int8_t)rc;

        tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_WRITE_COMPLETED, (int8_t)rc);

        pdebug(DEBUG_DETAIL, "Write completed event raised for tag %d with status %s.",
               tag->tag_id, plc_tag_decode_error((int8_t)rc));

        rc = PLCTAG_STATUS_OK;
    }

    return rc;
}


static int tag_op_write_response(modbus_plc_p plc, modbus_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    int response_ready = 0;

    if(plc->state == PLC_CONNECT_START || plc->state == PLC_CONNECT_WAIT || plc->state == PLC_ERR_WAIT) {
        pdebug(DEBUG_WARN, "PLC changed state, restarting request.");
        tag->op = TAG_OP_WRITE_REQUEST;
        tag->op_changed_time = time_ms();
        return PLCTAG_STATUS_OK;
    }

    /* Check response_ready flag - the PLC mutex is already held by the caller
     * (tickle_all_tags) so we can access this directly. */
    response_ready = plc->flags.response_ready;

    if(response_ready) {
        rc = check_write_response(plc, tag);

        switch(rc) {
            case PLCTAG_ERR_PARTIAL:
                /* partial response, keep going */
                pdebug(DEBUG_DETAIL, "Found part of our response, but we are not done.");

                /* Decrement in-flight counter */
                if(plc->pending_request_count > 0) {
                    plc->pending_request_count--;
                    pdebug(DEBUG_DETAIL, "Decremented request count to %d/%d.",
                           plc->pending_request_count, plc->max_requests_in_flight);
                } else {
                    pdebug(DEBUG_WARN, "Attempted to decrement request count below 0!");
                }

                /* PLC mutex already held by caller (tickle_all_tags) */
                plc->flags.response_ready = 0;

                tag->pending_transaction_id = 0;
                tag->op = TAG_OP_WRITE_REQUEST;
                tag->op_changed_time = time_ms();

                rc = PLCTAG_STATUS_PENDING;

                break;

            case PLCTAG_ERR_NO_MATCH:
                pdebug(DEBUG_SPEW, "Not our response.");
                /* Do NOT clear response_ready - let other tags check for their response */
                rc = PLCTAG_STATUS_PENDING;
                break;

            case PLCTAG_STATUS_OK:
                pdebug(DEBUG_DETAIL, "Tag %d write response ready.", tag->tag_id);
                /* fall through */
            default:
                /* Decrement in-flight counter */
                if(plc->pending_request_count > 0) {
                    plc->pending_request_count--;
                    pdebug(DEBUG_DETAIL, "Decremented request count to %d/%d.",
                           plc->pending_request_count, plc->max_requests_in_flight);
                } else {
                    pdebug(DEBUG_WARN, "Attempted to decrement request count below 0!");
                }

                /* PLC mutex already held by caller (tickle_all_tags) */
                plc->flags.response_ready = 0;

                tag->pending_transaction_id = 0;
                tag->op = TAG_OP_IDLE;
                tag->op_changed_time = time_ms();
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

        /* If the tag had a pending request, decrement counter */
        if(tag->pending_transaction_id != 0) {
            if(plc->pending_request_count > 0) {
                plc->pending_request_count--;
                pdebug(DEBUG_DETAIL, "Abort: Decremented request count to %d/%d.",
                       plc->pending_request_count, plc->max_requests_in_flight);
            } else {
                pdebug(DEBUG_WARN, "Abort: Attempted to decrement request count below 0!");
            }
        }

        tag->pending_transaction_id = 0;
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
        tag->op_changed_time = time_ms();

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
 * Also updates the minimum wait time for socket event timeout calculation.
 *
 * @param plc The PLC instance (unused).
 * @param base_tag The base tag to check.
 * @param now The current time in milliseconds.
 * @param min_wait_time Pointer to minimum wait time to update.
 * @return int The status of the operation.
 */
static int check_tag_auto_write(modbus_plc_p plc, plc_tag_p base_tag, int64_t now, int64_t *min_wait_time) {
    modbus_tag_p tag = (modbus_tag_p)base_tag;

    (void)plc;

    /* if auto write is turned on and the tag is dirty */
    if(tag->auto_sync_write_ms > 0 && tag->tag_is_dirty) {
        /* Don't start a new write if there's already an operation in progress.
         * This prevents orphaning pending responses when we overwrite seq_id. */
        if(tag->op != TAG_OP_IDLE) {
            pdebug(DEBUG_SPEW, "Tag already has operation in progress (%s), skipping auto write.",
                   op_to_str(tag->op));
            return PLCTAG_STATUS_OK;
        }

        /* initialize the next write time if needed */
        if(tag->auto_sync_next_write == 0) {
            tag->auto_sync_next_write = now + tag->auto_sync_write_ms;
        }

        /* Calculate wait time for socket event timeout */
        if(tag->auto_sync_next_write > now) {
            int64_t write_wait = tag->auto_sync_next_write - now;
            if(write_wait < *min_wait_time) {
                *min_wait_time = write_wait;
            }
        }

        /* if we have passed the wait time */
        if(tag->auto_sync_next_write <= now) {
            /* trigger the write */
            pdebug(DEBUG_DETAIL, "Auto write time reached, requesting auto write.");

            /* clean up state for next time. */
            tag->auto_sync_next_write = 0;
            tag->tag_is_dirty = false;
            tag->op = TAG_OP_WRITE_REQUEST;
            tag->op_changed_time = now;

            tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_WRITE_STARTED, PLCTAG_STATUS_OK);

            /* we will try to do a write. */
            return PLCTAG_STATUS_PENDING;
        }
    }

    return PLCTAG_STATUS_OK;
}


/**
 * @brief Check if the tag needs to be auto-read.
 *
 * This function checks if the tag is configured for automatic reading and
 * determines if the appropriate time has passed to trigger a read operation.
 *
 * Also updates the minimum wait time for socket event timeout calculation.
 *
 * @param plc The PLC instance (unused).
 * @param base_tag The base tag to check.
 * @param now The current time in milliseconds.
 * @param min_wait_time Pointer to minimum wait time to update.
 * @return int The status of the operation.
 */
static int check_tag_auto_read(modbus_plc_p plc, plc_tag_p base_tag, int64_t now, int64_t *min_wait_time) {
    modbus_tag_p tag = (modbus_tag_p)base_tag;

    (void)plc;

    /* if auto read is turned on */
    if(tag->auto_sync_read_ms > 0) {
        /* Don't start a new read if there's already an operation in progress.
         * This prevents orphaning pending responses when we overwrite seq_id. */
        if(tag->op != TAG_OP_IDLE) {
            pdebug(DEBUG_SPEW, "Tag already has operation in progress (%s), skipping auto read.",
                   op_to_str(tag->op));
            return PLCTAG_STATUS_OK;
        }

        /* make sure that there is not auto write pending */
        if(tag->auto_sync_write_ms > 0 && tag->tag_is_dirty) {
            pdebug(DEBUG_SPEW, "Auto write is pending, skipping auto read.");
            return PLCTAG_STATUS_OK;
        }

        /* Calculate wait time for socket event timeout */
        if(tag->auto_sync_next_read > now) {
            int64_t read_wait = tag->auto_sync_next_read - now;
            if(read_wait < *min_wait_time) {
                *min_wait_time = read_wait;
            }
        }

        /* if we have passed the wait time */
        if(tag->auto_sync_next_read <= now) {
            pdebug(DEBUG_SPEW, "Auto read time reached, requesting auto read.");

            tag->auto_sync_next_read = tag->auto_sync_next_read == 0 ? now : tag->auto_sync_next_read + tag->auto_sync_read_ms;
            tag->op = TAG_OP_READ_REQUEST;
            tag->op_changed_time = now;
            tag->request_start_time = now;

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
 * @param plc The PLC instance.
 * @param tag The tag to tickle.
 * @param now The current time in milliseconds.
 * @param min_wait_time Pointer to minimum wait time to update (for socket timeout).
 * @return int - PLCTAG_STATUS_OK if done, PLCTAG_STATUS_PENDING if still working, other
 *    on error.
 */
int tickle_tag(modbus_plc_p plc, modbus_tag_p tag, int64_t now, int64_t *min_wait_time) {
    int rc = PLCTAG_STATUS_OK;
    tag_op_type_t op = tag->op;
    bool event_raised = false;

    pdebug(DEBUG_SPEW, "Starting with tag %d.", tag->tag_id);

    pdebug(DEBUG_SPEW, "Current tag operation is %s.", op_to_str(tag->op));

    /* Check for aborts, auto writes, auto reads */
    if((rc = check_tag_abort(plc, (plc_tag_p)tag)) == PLCTAG_STATUS_PENDING) {
        pdebug(DEBUG_SPEW, "Tag operation aborted.");
        op = TAG_OP_IDLE;
    } else if((rc = check_tag_auto_write(plc, (plc_tag_p)tag, now, min_wait_time)) == PLCTAG_STATUS_PENDING) {
        pdebug(DEBUG_SPEW, "Auto write requested.");
        op = TAG_OP_WRITE_REQUEST;
        event_raised = true;
    } else if((rc = check_tag_auto_read(plc, (plc_tag_p)tag, now, min_wait_time)) == PLCTAG_STATUS_PENDING) {
        pdebug(DEBUG_SPEW, "Auto read requested.");
        op = TAG_OP_READ_REQUEST;
        event_raised = true;
    } else {
        /* maybe some existing operation */
        op = tag->op;
    }

    if(op != tag->op) {
        pdebug(DEBUG_SPEW, "Tag operation changed from %s to %s.", op_to_str(tag->op), op_to_str(op));
        tag->op = op;
        tag->op_changed_time = time_ms();
    }

    switch(op) {
        case TAG_OP_IDLE:
            pdebug(DEBUG_SPEW, "Tag is idle.");
            rc = PLCTAG_STATUS_OK;
            break;

        case TAG_OP_READ_REQUEST: rc = tag_op_read_request(plc, tag); break;

        case TAG_OP_READ_RESPONSE:
            rc = tag_op_read_response(plc, tag);
            break;

        case TAG_OP_WRITE_REQUEST: rc = tag_op_write_request(plc, tag); break;

        case TAG_OP_WRITE_RESPONSE:
            rc = tag_op_write_response(plc, tag);
            break;

        default:
            pdebug(DEBUG_WARN, "Unknown tag operation %d!", op);

            tag->op = TAG_OP_IDLE;
            tag->op_changed_time = time_ms();
            tag->status = (int8_t)PLCTAG_ERR_NOT_IMPLEMENTED;

            plc_tag_generic_wake_tag((plc_tag_p)tag);

            rc = PLCTAG_STATUS_OK;
            break;
    }

    if(op != tag->op) {
        pdebug(DEBUG_SPEW, "Tag operation changed from %s to %s.", op_to_str(op), op_to_str(tag->op));
        tag->op_changed_time = time_ms();
    }

    /* dispatch any events that were raised. */
    plc_tag_generic_handle_event_callbacks((plc_tag_p)tag);

    if(event_raised || tag->write_complete == 1 || tag->read_complete == 1) {
        pdebug(DEBUG_SPEW, "Tag operation complete.");
        plc_tag_generic_wake_tag((plc_tag_p)tag);
    }

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
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

    /* are we done? */
    if(data_needed == 0) {
        /* we got our packet. */
        pdebug(DEBUG_DETAIL, "Received full packet.");
        pdebug_dump_bytes(DEBUG_DETAIL, plc->read_data, plc->read_data_len);

        /* Update packet timestamp for inactivity tracking */
        plc->last_packet_time_ms = time_ms();

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

        /* Update packet timestamp for inactivity tracking */
        plc->last_packet_time_ms = time_ms();

        /* Record when request was sent for timing statistics */
        plc->last_request_sent_time = plc->last_packet_time_ms;

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

static void debug_vector(modbus_plc_p plc) {
    critical_block(plc->mutex) {
        int count = vector_length(plc->tag_vector);

        pdebug(DEBUG_DETAIL, "Dumping tag vector:");

        if(count == 0) {
            pdebug(DEBUG_DETAIL, "  (empty)");
            break;
        }

        for(int i = 0; i < count; i++) {
            modbus_tag_p tag = vector_get(plc->tag_vector, i);
            if(tag) {
                pdebug(DEBUG_DETAIL, "  [%d] Tag ID %" PRId32 ", %p.", i, tag->tag_id, tag);
            } else {
                pdebug(DEBUG_WARN, "  [%d] NULL tag pointer!", i);
            }
        }

        pdebug(DEBUG_DETAIL, "End of vector.");
    }
}


int add_tag(modbus_plc_p plc, modbus_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting to add tag %" PRIu32 ".", (uint32_t)tag->tag_id);

    pdebug(DEBUG_DETAIL, "Current vector before adding:");
    debug_vector(plc);

    critical_block(plc->mutex) {
        rc = vector_insert(plc->tag_vector, vector_length(plc->tag_vector), tag);
        if(rc == PLCTAG_STATUS_OK) {
            /* Increment tag count */
            atomic_add_int32(&plc->tag_count, 1);

            /* Initialize fairness ticket - new tags start at the back of the queue */
            tag->fairness_ticket = plc->fairness_counter++;
        } else {
            pdebug(DEBUG_WARN, "Failed to add tag to vector: %s", plc_tag_decode_error(rc));
        }
    }

    pdebug(DEBUG_DETAIL, "New vector after adding:");
    debug_vector(plc);

    pdebug(DEBUG_DETAIL, "Tag added, count now %d.", atomic_get_int32(&plc->tag_count));
    pdebug(DEBUG_DETAIL, "Done.");

    /* Wake handler to process new tag immediately */
    wake_plc_thread(plc);

    return rc;
}

int remove_tag(modbus_plc_p plc, modbus_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting to remove tag %" PRIu32 ".", (uint32_t)tag->tag_id);

    pdebug(DEBUG_DETAIL, "Current vector before removing:");
    debug_vector(plc);

    critical_block(plc->mutex) {
        int count = vector_length(plc->tag_vector);
        bool found = false;

        for(int i = 0; i < count; i++) {
            if(vector_get(plc->tag_vector, i) == tag) {
                pdebug(DEBUG_DETAIL, "Tag found at index %d, removing from vector.", i);
                vector_remove(plc->tag_vector, i);
                found = true;

                /* Decrement tag count */
                atomic_add_int32(&plc->tag_count, -1);
                int32_t remaining = atomic_get_int32(&plc->tag_count);
                pdebug(DEBUG_DETAIL, "Tag removed, count now %d.", remaining);

                /* If no more tags, signal handler thread to terminate */
                if(remaining == 0) {
                    pdebug(DEBUG_INFO, "Last tag removed from PLC, signaling handler thread to exit.");
                    plc->flags.terminate = 1;
                }
                break;
            }
        }

        if(!found) {
            /* not found */
            pdebug(DEBUG_INFO, "Tag not found in vector.");
            rc = PLCTAG_ERR_NOT_FOUND;
        }
    }

    pdebug(DEBUG_DETAIL, "New vector after removing:");
    debug_vector(plc);

    pdebug(DEBUG_DETAIL, "Done.");

    /* Wake handler to rescan immediately */
    wake_plc_thread(plc);

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
    tag->op_changed_time = time_ms();

    /* Only access PLC if it hasn't been terminated */
    if(tag->plc && !tag->plc->flags.terminate) {
        /* Clear pending transaction ID if the tag had one */
        if(tag->pending_transaction_id != 0) {
            tag->pending_transaction_id = 0;
            if(tag->plc->pending_request_count > 0) {
                tag->plc->pending_request_count--;
            }
        }

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
        tag->op_changed_time = time_ms();
        tag->request_start_time = time_ms();
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
        tag->op_changed_time = time_ms();
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
    int64_t start_time = 0;
    int64_t timeout_ms = 5000;
    int active_count = 0;
    int64_t elapsed = 0;

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

    /* Wait for all active handler threads to complete.
     * Use an atomic counter to track active threads.
     * Wait up to 5 seconds (5000 ms) with 20ms polling intervals.
     */
    pdebug(DEBUG_DETAIL, "Waiting for handler threads to complete.");
    start_time = time_ms();

    while((active_count = atomic_get_int32(&handler_threads_active)) > 0) {
        elapsed = time_ms() - start_time;

        if(elapsed >= timeout_ms) {
            pdebug(DEBUG_WARN, "Timeout waiting for %d handler threads to complete.", active_count);
            break;
        }

        pdebug(DEBUG_DETAIL, "Waiting for %d handler threads to complete. Elapsed: %" PRId64 "ms", active_count, elapsed);
        sleep_ms(20);
    }

    if(active_count == 0) {
        pdebug(DEBUG_INFO, "All handler threads completed.");
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

