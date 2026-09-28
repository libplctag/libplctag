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


#include <libplctag/api/libplctag.h>
#include <platform.h>
#include <utils/atomic_utils.h>
#include <utils/attr.h>
#include <utils/debug.h>


typedef struct plc_tag_t *plc_tag_p;

/* Opaque handle to the library-scoped instance (tag hashtable, its lookup mutex,
 * and the tag tickler thread/condvar). Full definition lives in lib.c. A tag holds
 * a reference for its whole lifetime (see plc_tag_create_impl()/each protocol
 * destructor), so any code reachable from a valid plc_tag_p may use
 * tag->instance->tags / tag->instance->tag_lookup_mutex directly: those objects
 * cannot be destroyed while a tag referencing them still exists. */
typedef struct tag_registry_t *tag_registry_p;

typedef int (*tag_vtable_func)(plc_tag_p tag);

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

/* Render a byte order array, e.g. {0,1,2,3}, as the "0,1,2,3" text the tag string uses. */
extern int32_t attr_copy_byte_order(const int *order, size_t order_len, uint8_t *buffer, int32_t buffer_length);
extern int32_t attr_byte_order_size(const int *order, size_t order_len);


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
     * NULL for protocols that only rely on the generic tag_tickler_func()/tag lookup
     * hashtable, since add_tag_lookup() already runs after generic setup completes.
     */
    tag_vtable_func activate;

    /*
     * Called from data-setter functions (plc_tag_set_int8 etc.) when
     * auto_sync_write_ms > 0 and the tag has just been marked dirty.
     * Called while api_mutex is held.  NULL if not implemented.
     */
    tag_vtable_func tag_data_written;

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

    unsigned int str_pad_to_multiple_bytes;
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
    int64_t read_cache_expire;                                                          \
    int64_t read_cache_ms;                                                              \
    uint8_t *data;                                                                      \
    tag_byte_order_t *byte_order;                                                       \
    cond_p tag_cond_wait;                                                               \
    mutex_p api_mutex;                                                                  \
    mutex_p ext_mutex;                                                                  \
    tag_extended_callback_func callback;                                                \
    tag_vtable_p vtable;                                                                \
    tag_registry_p instance;                                                            \
    void *userdata;                                                                     \
    int32_t auto_sync_read_ms;                                                          \
    int32_t auto_sync_write_ms;                                                         \
    int32_t size;                                                                       \
    int32_t tag_id;                                                                     \
    int connection_group_id;                                                            \
    int bit;                                                                            \
    int protocol_type;                                                                  \
    atomic_bool abort_requested;                                                        \
    /* Read by tag_tickler_func() under the global tag_lookup_mutex, while every other  \
     * field below is written under this tag's own per-tag api_mutex -- a different     \
     * lock domain. Packing it into the bitfield run below would race with writes to    \
     * its sibling bits sharing the same storage byte(s), so it gets its own atomic. */ \
    atomic_bool skip_tickler;                                                           \
    int8_t event_creation_complete_status;                                              \
    int8_t event_deletion_started_status;                                               \
    int8_t event_operation_aborted_status;                                              \
    int8_t event_read_complete_status;                                                  \
    int8_t event_read_started_status;                                                   \
    int8_t event_write_complete_status;                                                 \
    int8_t event_write_started_status;                                                  \
    int8_t status;                                                                      \
    uint8_t allow_field_resize : 1;                                                     \
    uint8_t event_creation_complete : 1;                                                \
    uint8_t event_deletion_started : 1;                                                 \
    uint8_t event_operation_aborted : 1;                                                \
    uint8_t event_read_complete : 1;                                                    \
    uint8_t event_read_complete_enable : 1;                                             \
    uint8_t event_read_started : 1;                                                     \
    uint8_t event_write_complete : 1;                                                   \
    uint8_t event_write_complete_enable : 1;                                            \
    uint8_t event_write_started : 1;                                                    \
    uint8_t had_created_event : 1;                                                      \
    uint8_t is_bit : 1;                                                                 \
    uint8_t read_complete : 1;                                                          \
    uint8_t read_in_flight : 1;                                                         \
    uint8_t tag_is_dirty : 1;                                                           \
    uint8_t write_complete : 1;                                                         \
    uint8_t write_in_flight : 1


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

extern void plc_tag_generic_tickler(plc_tag_p tag);
extern void plc_tag_generic_handle_event_callbacks(plc_tag_p tag);
#define plc_tag_tickler_wake() plc_tag_tickler_wake_impl(__func__, __LINE__)
extern int plc_tag_tickler_wake_impl(const char *func, int line_num);
#define plc_tag_generic_wake_tag(tag) plc_tag_generic_wake_tag_impl(__func__, __LINE__, tag)
extern int plc_tag_generic_wake_tag_impl(const char *func, int line_num, plc_tag_p tag);
extern int plc_tag_generic_init_tag(plc_tag_p tag, attr attributes,
                                    void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                    void *userdata);

static inline void tag_raise_event(plc_tag_p tag, int event, int8_t status) {
    /* do not stack up events if there is no callback. */
    if(!tag->callback) { return; }

    switch(event) {
        case PLCTAG_EVENT_ABORTED:
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "PLCTAG_EVENT_ABORTED raised with status %s.",
                   plc_tag_decode_error(status));
            tag->event_operation_aborted = 1;
            tag->event_operation_aborted_status = status;
            if(!tag->had_created_event) {
                pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Raising synthesized created event on abort event.");
                tag->had_created_event = 1;
                tag->event_creation_complete = 1;
                tag->event_creation_complete_status = status;
            }
            break;

        case PLCTAG_EVENT_CREATED:
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "PLCTAG_EVENT_CREATED raised with status %s.",
                   plc_tag_decode_error(status));
            if(!tag->had_created_event) {
                tag->event_creation_complete = 1;
                tag->event_creation_complete_status = status;
                tag->had_created_event = 1;
            } else {
                pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "PLCTAG_EVENT_CREATED skipped due to duplication.");
            }
            break;

        case PLCTAG_EVENT_DESTROYED:
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "PLCTAG_EVENT_DESTROYED raised with status %s.",
                   plc_tag_decode_error(status));
            tag->event_deletion_started = 1;
            tag->event_deletion_started_status = status;
            break;

        case PLCTAG_EVENT_READ_COMPLETED:
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "PLCTAG_EVENT_READ_COMPLETED raised with status %s.",
                   plc_tag_decode_error(status));
            if(!tag->had_created_event) {
                pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Raising synthesized created event on read completed event.");
                tag->had_created_event = 1;
                tag->event_creation_complete = 1;
                tag->event_creation_complete_status = status;
            }

            if(tag->event_read_complete_enable) {
                tag->event_read_complete = 1;
                tag->event_read_complete_status = status;
                tag->event_read_complete_enable = 0;
                pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Disabled PLCTAG_EVENT_READ_COMPLETE.");
            }
            break;

        case PLCTAG_EVENT_READ_STARTED:
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "PLCTAG_EVENT_READ_STARTED raised with status %s.",
                   plc_tag_decode_error(status));
            tag->event_read_started = 1;
            tag->event_read_started_status = status;
            tag->event_read_complete_enable = 1;
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Enabled PLCTAG_EVENT_READ_COMPLETE.");
            break;

        case PLCTAG_EVENT_WRITE_COMPLETED:
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "PLCTAG_EVENT_WRITE_COMPLETED raised with status %s.",
                   plc_tag_decode_error(status));
            if(!tag->had_created_event) {
                pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id,
                       "Raising synthesized created event on write completed event.");
                tag->had_created_event = 1;
                tag->event_creation_complete = 1;
                tag->event_creation_complete_status = status;
            }

            if(tag->event_write_complete_enable) {
                tag->event_write_complete = 1;
                tag->event_write_complete_status = status;
                tag->event_write_complete_enable = 0;
                pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Disabled PLCTAG_EVENT_WRITE_COMPLETE.");
            }
            break;

        case PLCTAG_EVENT_WRITE_STARTED:
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "PLCTAG_EVENT_WRITE_STARTED raised with status %s.",
                   plc_tag_decode_error(status));
            tag->event_write_started = 1;
            tag->event_write_started_status = status;
            tag->event_write_complete_enable = 1;
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Enabled PLCTAG_EVENT_WRITE_COMPLETE.");
            break;

        default: pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "Unsupported event %d!", status); break;
    }
}
