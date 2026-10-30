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


#pragma once


#include <stdbool.h>
#include <stdint.h>

#include <libplctag/api/libplctag.h>
#include <platform.h>
#include <utils/atomic_utils.h>
#include <utils/attr.h>
#include <utils/debug.h>


typedef struct plc_tag_t *plc_tag_p;

/* Opaque handle to the library-scoped instance (tag hashtable and its lookup
 * mutex). Full definition lives in tag_registry.c. A tag holds
 * a reference for its whole lifetime (see plc_tag_create_impl()/each protocol
 * destructor), so any code reachable from a valid plc_tag_p may use
 * tag->instance->tags / tag->instance->tag_lookup_mutex directly: those objects
 * cannot be destroyed while a tag referencing them still exists. */
typedef struct tag_registry_t *tag_registry_p;

typedef int (*tag_vtable_func)(plc_tag_p tag);

/*
 * A module's tag constructor.
 *
 * The return value is the error channel: PLCTAG_STATUS_OK and *tag_out set, or an error
 * code.  A constructor that allocated a tag before it failed hands that tag back anyway so
 * the caller can release it, so *tag_out may be non-NULL on an error return.
 */
typedef int (*tag_create_function)(attr attributes,
                                   void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                   void *userdata, plc_tag_p src_tag, plc_tag_p *tag_out);

/*
 * What one encoded request will cost, filled in by the encoder.  A handler that bundles
 * several requests into one exchange budgets them with it.
 */
typedef struct {
    int32_t req_size;   /* bytes the encoder wrote */
    int32_t reply_size; /* expected reply bytes, 0 when not known yet */
    bool packable;      /* may share an exchange with other requests */
} tag_op_plan_t;

/*
 * Write one request into dest, which holds capacity bytes, and describe it in plan.  Returns
 * PLCTAG_ERR_TOO_SMALL when it does not fit.  Encoding has no side effects on the tag's
 * progress, so a request that did not fit can be encoded again later.
 */
typedef int (*tag_encode_func)(plc_tag_p tag, uint8_t *dest, int32_t capacity, tag_op_plan_t *plan);

/* Apply one reply to the tag.  Returns OK when the operation is done, PENDING when it needs another exchange. */
typedef int (*tag_decode_func)(plc_tag_p tag, uint8_t *reply, int32_t reply_size);

/*
 * Runtime attribute descriptors.  The core and each protocol publish a NULL-name-terminated
 * table.  Lookup is by name; the entry's type selects the live union branch.
 *
 * A NULL accessor is the permission: NULL setter is read-only, NULL getter is write-only,
 * both NULL suppresses the core entry of that name for that protocol.  Create-time-only
 * values are not in these tables.
 */

typedef enum {
    ATTR_TYPE_INT,
    ATTR_TYPE_BYTES,

    /* ATTR_TYPE_BYTES holding NUL-terminated text.  The reported size includes the terminator. */
    ATTR_TYPE_STRING
} attr_val_type_t;

typedef struct attr_def_t attr_def_t;

struct attr_def_t {
    const char *name;
    attr_val_type_t type;
    const char *description; /* source for the generated documentation table */

    union {
        int32_t (*get_int)(plc_tag_p tag, int32_t *result);
        int32_t (*get_bytes)(plc_tag_p tag, uint8_t *buffer, int32_t buffer_length);
    };

    union {
        int32_t (*set_int)(plc_tag_p tag, int32_t value);
        int32_t (*set_bytes)(plc_tag_p tag, const uint8_t *buffer, int32_t buffer_length);
    };

    /* ATTR_TYPE_BYTES only: the current length of the value in bytes.  An ATTR_TYPE_INT
     * entry leaves this NULL and the core reports the width of an integer. */
    int32_t (*get_bytes_size)(plc_tag_p tag);
};

/* Resolve a name against the tag's protocol table first, then the core table.
 * Returns NULL when neither table carries it. */
extern const attr_def_t *attr_find(plc_tag_p tag, const char *name);

/* Resolve a name against the library-scope table, used when the tag id is zero.  Those
 * accessors take a NULL tag. */
extern const attr_def_t *attr_find_lib(const char *name);

/* Helpers for an ATTR_TYPE_STRING accessor.  attr_copy_string() copies str and its
 * terminator into buffer and returns the number of bytes copied, or PLCTAG_ERR_TOO_SMALL.
 * attr_string_size() returns what plc_tag_get_attribute_size() should report for str. */
extern int32_t attr_copy_string(const char *str, uint8_t *buffer, int32_t buffer_length);
extern int32_t attr_string_size(const char *str);


/* we'll need to set these per protocol type. */
struct tag_vtable_t {
    tag_vtable_func abort;
    tag_vtable_func read;
    tag_vtable_func status;
    tag_vtable_func tickler;
    tag_vtable_func write;

    tag_vtable_func wake_plc;

    /*
     * Called once by plc_tag_create_impl() after every generic tag field has been
     * initialized, right before it returns. Protocols that publish the tag to their
     * own worker-thread infrastructure (e.g. Modbus's per-PLC handler thread) must
     * do so here rather than during the protocol's own tag_create_function, since
     * that runs before the generic layer finishes setting up the tag object --
     * publishing any earlier lets the worker thread observe a half-initialized tag.
     * NULL for tags with no worker thread to publish to.
     */
    tag_vtable_func activate;

    /*
     * Called from data-setter functions (plc_tag_set_int8 etc.) when
     * auto_sync_write_ms > 0 and the tag has just been marked dirty.
     * Called while api_mutex is held.  NULL if not implemented.
     */
    tag_vtable_func tag_data_written;

    /*
     * The protocol's wire format, for a handler that drives the tag's operations itself.  NULL
     * where the protocol does not use them or the tag does not support that direction.
     */
    tag_encode_func encode_read;
    tag_decode_func decode_read;
    tag_encode_func encode_write;
    tag_decode_func decode_write;

    /* Runtime attributes this protocol publishes, NULL-name-terminated.  Consulted before
     * the core table, so a protocol can override or suppress a core attribute. */
    const attr_def_t *attribs;
};

typedef struct tag_vtable_t *tag_vtable_p;

typedef enum {
    TAG_PROTOCOL_UNKNOWN = 0,
    TAG_PROTOCOL_SYSTEM = 1,
    TAG_PROTOCOL_AB = 2,
    TAG_PROTOCOL_AB_CONNECTION = 3,
    TAG_PROTOCOL_MODBUS = 4,
    TAG_PROTOCOL_OMRON = 5,
    TAG_PROTOCOL_MB_CONNECTION = 6,
    TAG_PROTOCOL_OMRON_CONNECTION = 7
} tag_protocol_t;


/* Shared connection event types — used by AB, Modbus, and Omron connection tags */

typedef enum {
    TAG_CONN_EVENT_SEND_REQUEST_STARTED = 1,
    TAG_CONN_EVENT_SEND_REQUEST_COMPLETED = 2,
    TAG_CONN_EVENT_RECEIVE_RESPONSE_STARTED = 3,
    TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED = 4,
} tag_conn_event_type_t;

typedef struct {
    int32_t event_type; /* tag_conn_event_type_t */
    int32_t status;
} tag_conn_event_t;


/* byte ordering */

struct tag_byte_order_s {
    /* set if we allocated this specifically for the tag. */
    unsigned int is_allocated : 1;

    /* string type and ordering. */
    unsigned int str_is_defined : 1;
    unsigned int str_is_counted : 1;
    unsigned int str_is_fixed_length : 1;
    unsigned int str_is_zero_terminated : 1;
    unsigned int str_is_byte_swapped : 1;

    unsigned int write_pad_alignment_bytes;
    unsigned int str_count_word_bytes;
    unsigned int str_max_capacity;
    unsigned int str_total_length;
    unsigned int str_pad_bytes;

    int int16_order[2];
    int int32_order[4];
    int int64_order[8];

    int float32_order[4];
    int float64_order[8];
};


typedef struct tag_byte_order_s tag_byte_order_t;


typedef enum {
    TAG_OP_IDLE = 0,
    TAG_OP_READ_REQUEST, /* a read is in progress, queued or on the wire */
    TAG_OP_WRITE_REQUEST,
    TAG_OP_DESTROY /* idle, and destroyed at op_time unless a read or write comes first */
} tag_op_state_t;


/*
 * ponytail: fixed 16 entries.  The tag's handler dispatches every pass, so a full ring means the
 * dispatcher is starved; the push logs ERROR and drops the event.  Grow it if a real workload
 * hits that.
 */
#define TAG_EVENT_RING_SIZE (16)
#define TAG_EVENT_RING_MASK (TAG_EVENT_RING_SIZE - 1)

typedef struct {
    uint8_t event; /* PLCTAG_EVENT_*, all below 256 */
    int8_t status;
} tag_event_t;


typedef void (*tag_callback_func)(int32_t tag_id, int event, int status);
typedef void (*tag_extended_callback_func)(int32_t tag_id, int event, int status, void *user_data);

/*
 * The base definition of the tag structure.  This is used
 * by the protocol-specific implementations.
 *
 * The base type only has a vtable for operations.
 */

/* NB: sorted by decreasing size and then alphabetically */

#define TAG_BASE_STRUCT                                                                 \
    int64_t auto_sync_next_read;                                                        \
    int64_t auto_sync_next_write;                                                       \
    /* When the protocol handler next needs to act on this tag; see tag_schedule.h. */   \
    int64_t op_time;                                                                    \
    /* atomic so that plc_tag_read() can test it before taking the API mutex. */       \
    atomic_int64_t read_cache_expire;                                                   \
    atomic_int64_t read_cache_ms;                                                       \
    uint8_t *data;                                                                      \
    tag_byte_order_t *byte_order;                                                       \
    cond_p tag_cond_wait;                                                               \
    mutex_p api_mutex;                                                                  \
    mutex_p ext_mutex;                                                                  \
    tag_extended_callback_func callback;                                                \
    /* callback is set; written with it, read without the API mutex. */                \
    atomic_bool has_callback;                                                           \
    /* what made this tag; a clone of it is made the same way. */                       \
    tag_create_function create_func;                                                    \
    tag_vtable_p vtable;                                                                \
    tag_registry_p instance;                                                            \
    void *userdata;                                                                     \
    int32_t auto_sync_read_ms;                                                          \
    int32_t auto_sync_write_ms;                                                         \
    /* 0 = off.  Under api_mutex. */                                                    \
    int32_t destroy_idle_sec;                                                           \
    /* out of the registry at its due time; tag_driver_notify() destroys it. */        \
    bool auto_destroyed;                                                                \
    int32_t size;                                                                       \
    int32_t tag_id;                                                                     \
    int32_t elem_count;                                                                 \
    int32_t elem_size; /* bytes, except Modbus: bits */                                 \
    int32_t connection_group_id;                                                        \
    int32_t bit;                                                                        \
    tag_protocol_t protocol_type;                                                       \
    atomic_bool abort_requested;                                                        \
    /* tag_op_state_t; see tag_op_state(). */                                          \
    atomic_int32_t op_state;                                                            \
    tag_event_t events[TAG_EVENT_RING_SIZE];                                            \
    /* On its handler's active list.  Written under the handler's own mutex. */         \
    bool in_active_vector;                                                              \
    /* CREATED has been queued. */                                                      \
    bool created;                                                                       \
    /* Inside the dispatch loop, so nested calls only queue.  Under api_mutex. */      \
    bool dispatching;                                                                   \
    /* Ops started, and the last one ended, so a waiter knows its own op.  Under api_mutex. */ \
    uint32_t op_seq;                                                                    \
    uint32_t op_end_seq;                                                                \
    int8_t op_end_status;                                                               \
    uint8_t event_head;  /* next event to dispatch */                                   \
    uint8_t event_count; /* events queued */                                            \
    int8_t status;                                                                      \
    uint8_t allow_field_resize : 1;                                                     \
    uint8_t is_bit : 1;                                                                 \
    uint8_t tag_is_dirty : 1


struct plc_tag_t {
    TAG_BASE_STRUCT;
};

#define PLC_TAG_P_NULL ((plc_tag_p)0)


/* the following may need to be used where the tag is already mapped or is not yet mapped */

extern atomic_bool lib_active;


/* Acquire a reference to the current library instance, or NULL if the library is
 * not running (never started, or a shutdown has closed the gate). Safe to call
 * from any thread at any time; the caller must rc_dec() the result when done,
 * unless it is being transferred into a tag's tag->instance field (in which case
 * the tag's own destructor is the release). */
/*
 * tag_range_is_valid
 *
 * Is a field of count bytes starting at offset entirely inside the tag buffer?
 *
 * The offset and the count both come from the calling application, so they are
 * assumed hostile.  Note what this deliberately does NOT do: it never computes
 * offset + count.  That sum can exceed INT_MAX, and signed overflow is undefined
 * behavior -- a bounds check that overflows to a negative value happily reports
 * that a wildly out-of-range access is fine.  Instead every operand is forced
 * non-negative first, which makes tag->size - offset provably in range, and the
 * comparison is done against the space remaining.
 */
static inline bool tag_range_is_valid(plc_tag_p tag, int offset, int count) {
    if(offset < 0 || count < 0 || tag->size < 0) { return false; }

    /* both operands are now in [0, INT32_MAX], so this subtraction cannot overflow. */
    return count <= tag->size - offset;
}


/* Remove a tag from play: abort, raise DESTROYED, drop the library reference. */
extern void destroy_tag_common(plc_tag_p tag);

extern void plc_tag_generic_handle_event_callbacks(plc_tag_p tag);
#define plc_tag_generic_wake_tag(tag) plc_tag_generic_wake_tag_impl(__func__, __LINE__, tag)
extern int plc_tag_generic_wake_tag_impl(const char *func, int line_num, plc_tag_p tag);
extern int plc_tag_generic_init_tag(plc_tag_p tag, attr attributes,
                                    void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                    void *userdata);


/*
 * Op state and events.
 *
 * The op state is what the tag is doing now.  Events are what happened, queued in order on the
 * tag's ring and dispatched by plc_tag_generic_handle_event_callbacks().  The four transitions
 * below are the only places that queue op and creation events.  All of them, and every write of
 * op_state, happen with the tag's API mutex held.  op_state is atomic so that a handler holding
 * only its connection mutex can read it.
 */
#define tag_op_state(tag) ((tag_op_state_t)atomic_get_int32(&(tag)->op_state))
#define tag_op_set(tag, state) atomic_set_int32(&(tag)->op_state, (int32_t)(state))
/* only a read or write is busy; DESTROY is idle with a deadline. */
#define tag_op_busy(tag) (tag_op_state(tag) == TAG_OP_READ_REQUEST || tag_op_state(tag) == TAG_OP_WRITE_REQUEST)
#define tag_op_is_read(tag) (tag_op_state(tag) == TAG_OP_READ_REQUEST)

/* queue one event; dropped when the tag has no callback. */
extern void tag_event_push(plc_tag_p tag, int event, int status);

/* IDLE -> READ_REQUEST or WRITE_REQUEST; queues READ_STARTED or WRITE_STARTED once the tag is created. */
extern void tag_op_start(plc_tag_p tag, bool is_read);

/*
 * The op is over with rc: sets the tag status, goes IDLE, queues the COMPLETED event and wakes
 * waiters.  An op that ends before the tag is created is its create read, and queues CREATED
 * instead.  Does nothing on an IDLE tag.
 */
extern void tag_op_end(plc_tag_p tag, int rc);

/* Go IDLE: CREATED(ABORT) if not yet created, COMPLETED(ABORT) if an op was running, then ABORTED. */
extern void tag_op_abort(plc_tag_p tag);

/* queue CREATED with rc, once. */
extern void tag_created(plc_tag_p tag, int rc);
