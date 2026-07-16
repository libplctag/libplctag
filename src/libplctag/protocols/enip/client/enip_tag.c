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

/*
 * Tag vtable and create path (design doc §9, §11.2, §14.7).
 *
 * Create primes the tag via OPEN_PROBE (+ OPEN_BULK for elem_count > 1,
 * §11.2/§11.3). Read and write are both windowed the same way for
 * elem_count > 1 (§11.5).
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/protocols/enip/client/enip_cip.h>
#include <libplctag/protocols/enip/client/enip_session.h>
#include <libplctag/protocols/enip/client/enip_tag.h>
#include <platform.h>
#include <utils/arena.h>
#include <utils/attr.h>
#include <utils/bytes.h>
#include <utils/cbor.h>
#include <utils/debug.h>
#include <utils/rc.h>

static int32_t enip_tag_abort(plc_tag_p tag);
static int32_t enip_tag_read(plc_tag_p tag);
static int32_t enip_tag_write(plc_tag_p tag);
static int32_t enip_tag_status(plc_tag_p tag);
static int32_t enip_tag_data_written(plc_tag_p tag);
static int enip_tag_get_int_attrib(plc_tag_p tag, const char *attrib_name, int default_value);
static int enip_tag_set_int_attrib(plc_tag_p tag, const char *attrib_name, int new_value);
static int enip_tag_get_byte_array_attrib(plc_tag_p tag, const char *attrib_name, uint8_t *buffer, int buffer_length);
static void enip_tag_destructor(void *tag_arg);

/* CIP types are little-endian on the wire (design doc §9). */
static tag_byte_order_t enip_tag_byte_order = {.is_allocated = 0,

                                                .int16_order = {0, 1},
                                                .int32_order = {0, 1, 2, 3},
                                                .int64_order = {0, 1, 2, 3, 4, 5, 6, 7},
                                                .float32_order = {0, 1, 2, 3},
                                                .float64_order = {0, 1, 2, 3, 4, 5, 6, 7},

                                                .str_is_defined = 1,
                                                .str_is_counted = 1,
                                                .str_is_fixed_length = 1,
                                                .str_is_zero_terminated = 0,
                                                .str_is_byte_swapped = 0,

                                                .str_pad_to_multiple_bytes = 1,
                                                .str_count_word_bytes = 4,
                                                .str_max_capacity = 82,
                                                .str_total_length = 88,
                                                .str_pad_bytes = 2};

struct tag_vtable_t enip_tag_vtable = {
    .abort = enip_tag_abort,
    .read = enip_tag_read,
    .status = enip_tag_status,
    .tickler = NULL,
    .write = enip_tag_write,

    .wake_plc = NULL,
    .tag_data_written = enip_tag_data_written,

    .get_int_attrib = enip_tag_get_int_attrib,
    .set_int_attrib = enip_tag_set_int_attrib,
    .get_byte_array_attrib = enip_tag_get_byte_array_attrib,
};

/* ============================================================================
 * Vtable functions
 * ============================================================================ */

static int32_t enip_tag_abort(plc_tag_p tag) {
    enip_tag_p t = (enip_tag_p)tag;

    if(!t->conn) { return PLCTAG_STATUS_OK; }

    return enip_session_unschedule(t->conn, t);
}

static int32_t enip_tag_read(plc_tag_p tag) {
    enip_tag_p t = (enip_tag_p)tag;

    if(!t->conn) { return PLCTAG_ERR_BAD_GATEWAY; }

    if(!t->ready) {
        /* still opening (or open failed) -- report the create-time status. */
        return (t->status == (int8_t)PLCTAG_STATUS_OK) ? PLCTAG_STATUS_PENDING : t->status;
    }

    t->read_complete = 0;

    if(t->elem_count > 1 || t->kind == ENIP_TAG_KIND_PCCC) { t->read_off = 0; }
    if(t->fragmented_elem) { t->frag_offset = 0; } /* §16a.6: fresh fragment cursor for this read */

    tag_raise_event(tag, PLCTAG_EVENT_READ_STARTED, (int8_t)PLCTAG_STATUS_OK);

    return enip_session_schedule(t->conn, t, ENIP_OP_READ, time_ms());
}

static int32_t enip_tag_write(plc_tag_p tag) {
    enip_tag_p t = (enip_tag_p)tag;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, t->tag_id,
           "write entry: ready=%d scheduled=%d op=%d elem_count=%" PRIu32 " write_window_elems=%" PRIu32 " read_off=%" PRIu32,
           (int)t->ready, (int)t->scheduled, (int)t->op, t->elem_count, t->write_window_elems, t->read_off);

    if(!t->conn) { return PLCTAG_ERR_BAD_GATEWAY; }

    if(!t->ready) {
        /* still opening (or open failed) -- report the create-time status. */
        return (t->status == (int8_t)PLCTAG_STATUS_OK) ? PLCTAG_STATUS_PENDING : t->status;
    }

    t->write_complete = 0;

    if(t->elem_count > 1 || t->kind == ENIP_TAG_KIND_PCCC) { t->read_off = 0; }
    if(t->fragmented_elem) { t->frag_offset = 0; } /* §16a.6: fresh fragment cursor for this write */

    tag_raise_event(tag, PLCTAG_EVENT_WRITE_STARTED, (int8_t)PLCTAG_STATUS_OK);

    return enip_session_schedule(t->conn, t, ENIP_OP_WRITE, time_ms());
}

static int32_t enip_tag_status(plc_tag_p tag) {
    enip_tag_p t = (enip_tag_p)tag;

    if(t->op != ENIP_OP_IDLE) { return PLCTAG_STATUS_PENDING; }

    return t->status;
}

/* Called under api_mutex from the data-setter functions when
 * auto_sync_write_ms > 0 (design doc §9/§14.8). Arms auto_sync_next_write
 * immediately so the generic tickler's next pass (within ~100ms) sees a
 * due time already set, instead of needing one tick just to start the
 * countdown. */
/* Called by plc_tag_get_int_attribute under tag->api_mutex (the generic layer
 * has already handled size/read_cache_ms/etc). */
static int enip_tag_get_int_attrib(plc_tag_p tag, const char *attrib_name, int default_value) {
    enip_tag_p t = (enip_tag_p)tag;

    tag->status = (int8_t)PLCTAG_STATUS_OK;

    if(str_cmp_i(attrib_name, "elem_size") == 0) { return (int)t->elem_size; }

    if(str_cmp_i(attrib_name, "elem_count") == 0) { return (int)t->elem_count; }

    if(str_cmp_i(attrib_name, "connection_status") == 0) {
        return t->conn ? enip_session_get_status(t->conn) : (int)PLCTAG_CONN_STATUS_DOWN;
    }

    if(str_cmp_i(attrib_name, "connection_inactivity_timeout_ms") == 0) {
        return t->conn ? enip_session_get_inactivity_timeout(t->conn) : default_value;
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unsupported attribute \"%s\"!", attrib_name);
    tag->status = (int8_t)PLCTAG_ERR_UNSUPPORTED;

    return default_value;
}

static int enip_tag_set_int_attrib(plc_tag_p tag, const char *attrib_name, int new_value) {
    enip_tag_p t = (enip_tag_p)tag;

    if(str_cmp_i(attrib_name, "connection_inactivity_timeout_ms") == 0) {
        if(!t->conn) {
            tag->status = (int8_t)PLCTAG_ERR_BAD_GATEWAY;
            return PLCTAG_ERR_BAD_GATEWAY;
        }

        int rc = enip_session_set_inactivity_timeout(t->conn, new_value);
        tag->status = (int8_t)rc;
        return rc;
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unsupported attribute \"%s\"!", attrib_name);
    tag->status = (int8_t)PLCTAG_ERR_UNSUPPORTED;

    return PLCTAG_ERR_UNSUPPORTED;
}

/* Returns the raw CIP type-header bytes captured at OPEN_PROBE (atomic: the
 * 2-byte type code; structure: the 4-byte abbreviated-struct header). Used by
 * the "metadata" tag type to report the on-wire type. */
static int enip_tag_get_byte_array_attrib(plc_tag_p tag, const char *attrib_name, uint8_t *buffer, int buffer_length) {
    enip_tag_p t = (enip_tag_p)tag;

    if(str_cmp_i(attrib_name, "raw_tag_type_bytes") != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unsupported byte-array attribute \"%s\"!", attrib_name);
        tag->status = (int8_t)PLCTAG_ERR_UNSUPPORTED;
        return PLCTAG_ERR_UNSUPPORTED;
    }

    if(t->type_header_len == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Tag type info not yet available (tag not opened).");
        tag->status = (int8_t)PLCTAG_ERR_NOT_FOUND;
        return PLCTAG_ERR_NOT_FOUND;
    }

    if((int)t->type_header_len > buffer_length) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Type info (%u bytes) larger than buffer (%d bytes).",
               (unsigned int)t->type_header_len, buffer_length);
        tag->status = (int8_t)PLCTAG_ERR_TOO_SMALL;
        return PLCTAG_ERR_TOO_SMALL;
    }

    bytes_pack_into(bytes_from_buf(buffer, (size_t)buffer_length), BYTES_LE,
                   bytes_from_buf(t->type_header, t->type_header_len));
    tag->status = (int8_t)PLCTAG_STATUS_OK;

    return (int)t->type_header_len;
}

/* ============================================================================
 * @connection special tag (design doc §14.2): a status-only tag that mirrors
 * the connection's conn-status ring as PLCTAG_EVENT_CONN_STATUS_* events.
 * ============================================================================ */

static int32_t enip_conn_tag_noop(plc_tag_p tag) {
    (void)tag;
    return PLCTAG_STATUS_OK;
}

static int32_t enip_conn_tag_status(plc_tag_p tag) { return tag->status; }

static int enip_conn_tag_get_int_attrib(plc_tag_p tag, const char *attrib_name, int default_value) {
    enip_tag_p t = (enip_tag_p)tag;

    tag->status = (int8_t)PLCTAG_STATUS_OK;

    if(str_cmp_i(attrib_name, "connection_status") == 0) { return (int)t->last_conn_state; }

    if(str_cmp_i(attrib_name, "connection_inactivity_timeout_ms") == 0) {
        return t->conn ? enip_session_get_inactivity_timeout(t->conn) : default_value;
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unsupported attribute \"%s\"!", attrib_name);
    tag->status = (int8_t)PLCTAG_ERR_UNSUPPORTED;

    return default_value;
}

/* Called by the generic tickler under api_mutex. Waits for the CREATED event to
 * be dispatched, then (on the first run) synthesises the late-join state and
 * drains any new conn-status transitions, firing one callback per transition. */
static int32_t enip_conn_tag_tickler(plc_tag_p tag) {
    enip_tag_p t = (enip_tag_p)tag;

    if(!t->conn) { return PLCTAG_STATUS_OK; }

    /* event_creation_complete is still set until plc_tag_create_ex dispatches
     * CREATED; firing connection events before that would reorder them. */
    if(tag->event_creation_complete) { return PLCTAG_STATUS_OK; }

    if(t->first_tickler_run) {
        t->first_tickler_run = 0;

        /* Late join: the session was already past DOWN at create time, so report
         * the snapshot state since no ring entry exists for it. */
        if(t->last_conn_state != (int32_t)PLCTAG_CONN_STATUS_DOWN && tag->callback) {
            tag->callback(tag->tag_id, (int)(t->last_conn_state + PLCTAG_EVENT_CONN_STATUS_OFFSET), PLCTAG_STATUS_OK,
                          tag->userdata);
        }
    }

    int32_t status = 0;
    while(enip_session_next_conn_status(t->conn, &t->conn_status_read_idx, &status)) {
        t->last_conn_state = status;
        if(tag->callback) {
            tag->callback(tag->tag_id, (int)(status + PLCTAG_EVENT_CONN_STATUS_OFFSET), PLCTAG_STATUS_OK, tag->userdata);
        }
    }

    return PLCTAG_STATUS_OK;
}

static struct tag_vtable_t enip_connection_tag_vtable = {
    .abort = enip_conn_tag_noop,
    .read = NULL,
    .status = enip_conn_tag_status,
    .tickler = enip_conn_tag_tickler,
    .write = NULL,
    .wake_plc = NULL,
    .tag_data_written = NULL,
    .get_int_attrib = enip_conn_tag_get_int_attrib,
    .set_int_attrib = NULL,
    .get_byte_array_attrib = NULL,
};

/* ============================================================================
 * @identity special tag: read-only, surfaces the connection's cached CIP
 * Identity payload (raw Get_Attributes_All response, LE byte order). The
 * session queries identity once during bring-up; this tag just copies the
 * cached bytes once they are available.
 * ============================================================================ */

/* Copy the cached identity payload into tag->data; PENDING until it exists. */
static int32_t enip_identity_tag_copy(plc_tag_p tag) {
    enip_tag_p t = (enip_tag_p)tag;
    uint8_t *data = NULL;
    uint16_t len = 0;

    if(!enip_session_get_identity(t->conn, &data, &len)) { return PLCTAG_STATUS_PENDING; }

    uint8_t *buf = mem_realloc(tag->data, (int)len);
    if(!buf) { return PLCTAG_ERR_NO_MEM; }

    tag->data = buf;
    tag->size = (int)len;
    bytes_pack_into(bytes_from_buf(tag->data, (size_t)len), BYTES_LE, bytes_from_buf(data, len));

    return PLCTAG_STATUS_OK;
}

static int32_t enip_identity_tag_abort(plc_tag_p tag) {
    tag->read_in_flight = 0;
    return PLCTAG_STATUS_OK;
}

static int32_t enip_identity_tag_status(plc_tag_p tag) { return tag->status; }

static int32_t enip_identity_tag_read(plc_tag_p tag) {
    enip_tag_p t = (enip_tag_p)tag;

    if(!t->conn) { return PLCTAG_ERR_BAD_GATEWAY; }

    tag->read_complete = 0;
    tag->read_in_flight = 1;
    tag->status = (int8_t)PLCTAG_STATUS_PENDING;
    tag_raise_event(tag, PLCTAG_EVENT_READ_STARTED, (int8_t)PLCTAG_STATUS_OK);

    /* Mark due so the IO thread's service pass runs the tickler and completes it. */
    enip_session_schedule(t->conn, t, ENIP_OP_IDLE, time_ms());

    return PLCTAG_STATUS_PENDING;
}

/* Called by the generic tickler under api_mutex. Completes a pending read when
 * the session has cached the identity payload. */
static int32_t enip_identity_tag_tickler(plc_tag_p tag) {
    enip_tag_p t = (enip_tag_p)tag;

    if(!t->conn || !tag->read_in_flight) { return PLCTAG_STATUS_OK; }

    int32_t rc = enip_identity_tag_copy(tag);
    if(rc == PLCTAG_STATUS_PENDING) { return PLCTAG_STATUS_OK; }

    tag->read_in_flight = 0;
    tag->read_complete = 1;
    tag->status = (int8_t)rc;
    tag_raise_event(tag, PLCTAG_EVENT_READ_COMPLETED, (int8_t)rc);

    return PLCTAG_STATUS_OK;
}

/* ============================================================================
 * @identity structured presentation (format/schema subsystem; design doc
 * ENIP-METADATA-AND-DISCOVERY-DESIGN.md §0). "cbor" is rendered on demand
 * from the cached raw Get_Attributes_All payload (tag->data) -- nothing
 * structured is stored. Read-only: no set_formatted_data/set_schema.
 * ============================================================================ */

#define ENIP_IDENTITY_SCHEMA_NAME "identity"
#define ENIP_IDENTITY_SCHEMA_VERSION ((uint64_t)1)

/* CIP SHORT_STRING product names are ASCII, a valid UTF-8 subset, so they can
 * be written directly as a CBOR text string with no re-encoding. */
#define CBOR_LIT(s) (s), (sizeof(s) - 1)

/* Parsed view of the raw Get_Attributes_All payload cached in tag->data
 * (identity_encode_get_attrs_all's layout, common/identity.c). product_name
 * points directly into tag->data (not NUL-terminated, not copied). Returns
 * false if the payload is not yet cached or is malformed. */
typedef struct {
    uint16_t vendor_id, device_type, product_code, status;
    uint8_t revision_major, revision_minor;
    uint32_t serial;
    const char *product_name;
    uint8_t product_name_len;
} enip_identity_fields_t;

static bool enip_identity_parse_fields(plc_tag_p tag, enip_identity_fields_t *out) {
    if(!tag->data || tag->size < 14) { return false; }

    Bytes raw = bytes_from_buf(tag->data, (size_t)tag->size);
    uint8_t name_len = 0;

    Bytes rest = bytes_unpack(raw, BYTES_LE, &out->vendor_id, &out->device_type, &out->product_code, &out->revision_major,
                              &out->revision_minor, &out->status, &out->serial, &name_len);
    if(bytes_is_null(rest) || rest.len < (size_t)name_len) { return false; }

    out->product_name = (const char *)rest.data;
    out->product_name_len = name_len;

    return true;
}

/* record map: 8 string-keyed fields (§0's per-record shape for "identity"). */
static size_t enip_identity_record_cbor_size(const enip_identity_fields_t *f) {
    size_t sz = cbor_size_map_header(8);
    sz += cbor_size_text(sizeof("vendor_id") - 1) + cbor_size_uint(f->vendor_id);
    sz += cbor_size_text(sizeof("device_type") - 1) + cbor_size_uint(f->device_type);
    sz += cbor_size_text(sizeof("product_code") - 1) + cbor_size_uint(f->product_code);
    sz += cbor_size_text(sizeof("revision_major") - 1) + cbor_size_uint(f->revision_major);
    sz += cbor_size_text(sizeof("revision_minor") - 1) + cbor_size_uint(f->revision_minor);
    sz += cbor_size_text(sizeof("status") - 1) + cbor_size_uint(f->status);
    sz += cbor_size_text(sizeof("serial") - 1) + cbor_size_uint(f->serial);
    sz += cbor_size_text(sizeof("product_name") - 1) + cbor_size_text(f->product_name_len);
    return sz;
}

static bool enip_identity_record_cbor_write(Bytes dest, size_t *pos, const enip_identity_fields_t *f) {
    if(!cbor_write_map_header(dest, pos, 8)) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT("vendor_id"))) { return false; }
    if(!cbor_write_uint(dest, pos, f->vendor_id)) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT("device_type"))) { return false; }
    if(!cbor_write_uint(dest, pos, f->device_type)) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT("product_code"))) { return false; }
    if(!cbor_write_uint(dest, pos, f->product_code)) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT("revision_major"))) { return false; }
    if(!cbor_write_uint(dest, pos, f->revision_major)) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT("revision_minor"))) { return false; }
    if(!cbor_write_uint(dest, pos, f->revision_minor)) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT("status"))) { return false; }
    if(!cbor_write_uint(dest, pos, f->status)) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT("serial"))) { return false; }
    if(!cbor_write_uint(dest, pos, f->serial)) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT("product_name"))) { return false; }
    if(!cbor_write_text(dest, pos, f->product_name, f->product_name_len)) { return false; }
    return true;
}

/* top-level envelope: {"schema","schema-version","records":[<one record>]}. */
static size_t enip_identity_envelope_cbor_size(const enip_identity_fields_t *f) {
    size_t sz = cbor_size_map_header(3);
    sz += cbor_size_text(sizeof("schema") - 1) + cbor_size_text(sizeof(ENIP_IDENTITY_SCHEMA_NAME) - 1);
    sz += cbor_size_text(sizeof("schema-version") - 1) + cbor_size_uint(ENIP_IDENTITY_SCHEMA_VERSION);
    sz += cbor_size_text(sizeof("records") - 1) + cbor_size_array_header(1);
    sz += enip_identity_record_cbor_size(f);
    return sz;
}

static bool enip_identity_envelope_cbor_write(Bytes dest, size_t *pos, const enip_identity_fields_t *f) {
    if(!cbor_write_map_header(dest, pos, 3)) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT("schema"))) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT(ENIP_IDENTITY_SCHEMA_NAME))) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT("schema-version"))) { return false; }
    if(!cbor_write_uint(dest, pos, ENIP_IDENTITY_SCHEMA_VERSION)) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT("records"))) { return false; }
    if(!cbor_write_array_header(dest, pos, 1)) { return false; }
    return enip_identity_record_cbor_write(dest, pos, f);
}

static int enip_identity_get_formatted_data_size(plc_tag_p tag, plc_tag_format_type_t format) {
    if(format != PLCTAG_FORMAT_CBOR) { return PLCTAG_ERR_UNSUPPORTED; }

    enip_identity_fields_t f;
    if(!enip_identity_parse_fields(tag, &f)) { return PLCTAG_ERR_NO_DATA; }

    return (int)enip_identity_envelope_cbor_size(&f);
}

static int enip_identity_get_formatted_data(plc_tag_p tag, plc_tag_format_type_t format, uint8_t *buffer, int buffer_length) {
    if(format != PLCTAG_FORMAT_CBOR) { return PLCTAG_ERR_UNSUPPORTED; }

    enip_identity_fields_t f;
    if(!enip_identity_parse_fields(tag, &f)) { return PLCTAG_ERR_NO_DATA; }

    if(enip_identity_envelope_cbor_size(&f) > (size_t)buffer_length) { return PLCTAG_ERR_TOO_SMALL; }

    Bytes dest = bytes_from_buf(buffer, (size_t)buffer_length);
    size_t pos = 0;
    if(!enip_identity_envelope_cbor_write(dest, &pos, &f)) { return PLCTAG_ERR_TOO_SMALL; }

    return PLCTAG_STATUS_OK;
}

/* Field-name reflection of the "identity" schema -- {"schema","schema-version",
 * "fields":[...]}. Not tag-specific (every @identity tag has the same schema),
 * so this ignores `tag`. */
static const char *const ENIP_IDENTITY_FIELD_NAMES[] = {"vendor_id",      "device_type",    "product_code", "revision_major",
                                                         "revision_minor", "status",         "serial",       "product_name"};
#define ENIP_IDENTITY_FIELD_COUNT ((size_t)(sizeof(ENIP_IDENTITY_FIELD_NAMES) / sizeof(ENIP_IDENTITY_FIELD_NAMES[0])))

static size_t enip_identity_schema_cbor_size(void) {
    size_t sz = cbor_size_map_header(3);
    sz += cbor_size_text(sizeof("schema") - 1) + cbor_size_text(sizeof(ENIP_IDENTITY_SCHEMA_NAME) - 1);
    sz += cbor_size_text(sizeof("schema-version") - 1) + cbor_size_uint(ENIP_IDENTITY_SCHEMA_VERSION);
    sz += cbor_size_text(sizeof("fields") - 1) + cbor_size_array_header(ENIP_IDENTITY_FIELD_COUNT);
    for(size_t i = 0; i < ENIP_IDENTITY_FIELD_COUNT; i++) {
        sz += cbor_size_text((size_t)str_length(ENIP_IDENTITY_FIELD_NAMES[i]));
    }
    return sz;
}

static bool enip_identity_schema_cbor_write(Bytes dest, size_t *pos) {
    if(!cbor_write_map_header(dest, pos, 3)) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT("schema"))) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT(ENIP_IDENTITY_SCHEMA_NAME))) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT("schema-version"))) { return false; }
    if(!cbor_write_uint(dest, pos, ENIP_IDENTITY_SCHEMA_VERSION)) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT("fields"))) { return false; }
    if(!cbor_write_array_header(dest, pos, ENIP_IDENTITY_FIELD_COUNT)) { return false; }
    for(size_t i = 0; i < ENIP_IDENTITY_FIELD_COUNT; i++) {
        if(!cbor_write_text(dest, pos, ENIP_IDENTITY_FIELD_NAMES[i], (size_t)str_length(ENIP_IDENTITY_FIELD_NAMES[i]))) {
            return false;
        }
    }
    return true;
}

static int enip_identity_get_schema_size(plc_tag_p tag, plc_tag_format_type_t format) {
    (void)tag;
    if(format != PLCTAG_FORMAT_CBOR) { return PLCTAG_ERR_UNSUPPORTED; }
    return (int)enip_identity_schema_cbor_size();
}

static int enip_identity_get_schema(plc_tag_p tag, plc_tag_format_type_t format, uint8_t *buffer, int buffer_length) {
    (void)tag;
    if(format != PLCTAG_FORMAT_CBOR) { return PLCTAG_ERR_UNSUPPORTED; }

    if(enip_identity_schema_cbor_size() > (size_t)buffer_length) { return PLCTAG_ERR_TOO_SMALL; }

    Bytes dest = bytes_from_buf(buffer, (size_t)buffer_length);
    size_t pos = 0;
    if(!enip_identity_schema_cbor_write(dest, &pos)) { return PLCTAG_ERR_TOO_SMALL; }

    return PLCTAG_STATUS_OK;
}

static struct tag_vtable_t enip_identity_tag_vtable = {
    .abort = enip_identity_tag_abort,
    .read = enip_identity_tag_read,
    .status = enip_identity_tag_status,
    .tickler = enip_identity_tag_tickler,
    .write = NULL,
    .wake_plc = NULL,
    .tag_data_written = NULL,
    .get_int_attrib = NULL,
    .set_int_attrib = NULL,
    .get_byte_array_attrib = NULL,
    .get_formatted_data_size = enip_identity_get_formatted_data_size,
    .get_formatted_data = enip_identity_get_formatted_data,
    .set_formatted_data = NULL, /* read-only tag */
    .get_schema_size = enip_identity_get_schema_size,
    .get_schema = enip_identity_get_schema,
    .set_schema = NULL, /* built-in schema, not user-settable */
};

/* ============================================================================
 * @tags / @udt listing tags (ControlLogix-class only, gated in the IO thread):
 * read-only tags that walk CIP class 0x6B (symbol/tag list) or class 0x6C
 * (UDT/template definition) and accumulate the raw reply payload. They share the
 * data-variant scheduler fields and ride the network-op path via ENIP_OP_LIST /
 * ENIP_OP_UDT_META / ENIP_OP_UDT_FIELDS (see enip_session.c).
 * ============================================================================ */

static int32_t enip_listing_tag_read(plc_tag_p tag) {
    enip_tag_p t = (enip_tag_p)tag;

    if(!t->conn) { return PLCTAG_ERR_BAD_GATEWAY; }

    t->read_complete = 0;
    t->read_off = 0;
    t->size = 0;

    /* @tags restarts the instance scan at 0; @udt keeps its fixed template id
     * (list_next_id, set at create) and begins with the metadata request. */
    uint8_t op;
    if(t->kind == ENIP_TAG_KIND_UDT) {
        op = ENIP_OP_UDT_META;
        t->list_total = 0;
    } else {
        op = ENIP_OP_LIST;
        t->list_next_id = 0;
    }

    tag_raise_event(tag, PLCTAG_EVENT_READ_STARTED, (int8_t)PLCTAG_STATUS_OK);

    return enip_session_schedule(t->conn, t, op, time_ms());
}

/* ============================================================================
 * @tags PCCC File 0 listing: structured presentation (format/schema
 * subsystem; design doc ENIP-METADATA-AND-DISCOVERY-DESIGN.md §0). Rendered
 * on demand from the raw concatenated File 0 records accumulated by
 * enip_pccc_apply_listing (client/enip_session.c) -- record width depends on
 * the connection's identity-classified PLC family (4 bytes/PLC-5, 6 bytes/
 * SLC+MicroLogix; see that function's doc comment for the provenance and
 * known limitations of this record layout). Read-only, like @identity.
 * Rockwell/OMRON @tags and @udt have no CBOR schema yet (a future task);
 * PLCTAG_FORMAT_CBOR on those returns PLCTAG_ERR_UNSUPPORTED, same as today.
 * ============================================================================ */

#define PCCC_LISTING_SCHEMA_NAME "pccc-file-list"
#define PCCC_LISTING_SCHEMA_VERSION ((uint64_t)1)

static const char *pccc_file_type_name(uint8_t code) {
    switch((pccc_file_t)code) {
        case PCCC_FILE_BIT: return "BIT";
        case PCCC_FILE_TIMER: return "TIMER";
        case PCCC_FILE_COUNTER: return "COUNTER";
        case PCCC_FILE_CONTROL: return "CONTROL";
        case PCCC_FILE_INT: return "INT";
        case PCCC_FILE_FLOAT: return "FLOAT";
        case PCCC_FILE_OUTPUT: return "OUTPUT";
        case PCCC_FILE_INPUT: return "INPUT";
        case PCCC_FILE_STATUS: return "STATUS";
        case PCCC_FILE_ASCII: return "ASCII";
        case PCCC_FILE_BCD: return "BCD";
        case PCCC_FILE_STRING: return "STRING";
        case PCCC_FILE_LONG_INT: return "LONG_INT";
        case PCCC_FILE_MESSAGE: return "MESSAGE";
        case PCCC_FILE_PID: return "PID";
        default: return "UNKNOWN";
    }
}

/* tag->kind == ENIP_TAG_KIND_LISTING and its connection is a PCCC family --
 * the only case this codec handles. Sets *is_plc5_out; used by every getter
 * below to avoid repeating the connection lookup. */
static bool pccc_listing_is_pccc(plc_tag_p tag, bool *is_plc5_out) {
    enip_tag_p t = (enip_tag_p)tag;
    if(t->kind != ENIP_TAG_KIND_LISTING || !t->conn) { return false; }

    enip_plc_type_t pt = enip_session_get_plc_type(t->conn);
    if(!enip_plc_is_pccc(pt)) { return false; }

    *is_plc5_out = (pt == ENIP_PLC_PLC5);
    return true;
}

/* PLC-5 File 0 record decode (4 bytes: attribute(1), file_number(1),
 * total_words(2 LE)), per a vendor protocol technical report (not
 * independently verified against real hardware in this tree). The PLC-5 has
 * no explicit type-ID byte; type is inferred from a fixed file-number
 * convention (0/1/2) plus the attribute byte's structure-class nibble
 * (bits 0-3) and radix bits (bits 4-5), corroborated by dividing total_words
 * by the resolved structure's word footprint. The resolved type is expressed
 * as a pccc_file_t code -- the same vocabulary enip_pccc_addr.c already uses
 * to parse logical addresses like N7:0 -- so the public per-record schema
 * (below) is identical for PLC-5 and SLC/MicroLogix: both report "what PCCC
 * file type is this", just decoded from a different wire shape. This does
 * lose PLC-5's own inactive-vs-unrecognized-structure distinction (both
 * collapse to PCCC_FILE_UNKNOWN, same as SLC/MicroLogix's one generic
 * "unknown"); a caller that needs that distinction back can inspect the
 * record's "raw" bytes (attribute byte bit 6) itself.
 *
 * Deviation from the report's own reference pseudo-code: its Python divides
 * unconditionally (`total_words // 3` etc.) with no remainder check, but its
 * prose separately calls this a "Validation Rule" ("if total words mod words
 * per element != 0, flag ... mark the file as an unrecognized custom data
 * structure"). Silently keeping a resolved class label next to a fractional
 * element count would be worse than useless -- it looks precise and isn't --
 * so this follows the prose: a non-divisible word count degrades file_type to
 * PCCC_FILE_UNKNOWN and omits element_count, rather than trusting the class
 * nibble over the arithmetic. */
typedef struct {
    uint8_t file_number;
    uint16_t total_words;
    uint8_t file_type; /* pccc_file_t code */
    bool has_elements;
    uint32_t element_count;
} pccc_plc5_file_decode_t;

static pccc_plc5_file_decode_t pccc_decode_plc5_file_record(Bytes rec) {
    pccc_plc5_file_decode_t d = {0};
    uint8_t attr_byte = 0;
    bytes_unpack(rec, BYTES_LE, &attr_byte, &d.file_number, &d.total_words);

    /* Step 1: hardcoded default system files -- checked before, and instead
     * of, the attribute byte. */
    if(d.file_number == 0) {
        d.file_type = (uint8_t)PCCC_FILE_OUTPUT;
        d.has_elements = true;
        d.element_count = d.total_words;
        return d;
    }
    if(d.file_number == 1) {
        d.file_type = (uint8_t)PCCC_FILE_INPUT;
        d.has_elements = true;
        d.element_count = d.total_words;
        return d;
    }
    if(d.file_number == 2) {
        d.file_type = (uint8_t)PCCC_FILE_STATUS;
        d.has_elements = true;
        d.element_count = d.total_words;
        return d;
    }

    bool active = (attr_byte & 0x40) != 0;
    if(!active) {
        d.file_type = (uint8_t)PCCC_FILE_UNKNOWN;
        return d;
    }

    uint8_t radix_bits = (uint8_t)((attr_byte >> 4) & 0x03);
    uint8_t class_nibble = (uint8_t)(attr_byte & 0x0Fu);

    switch(class_nibble) {
        case 0x00: /* Simple Word Struct -- disambiguated by radix bits */
            d.has_elements = true;
            d.element_count = d.total_words;
            if(radix_bits == 0x00) { d.file_type = (uint8_t)PCCC_FILE_BIT; }       /* "Binary" */
            else if(radix_bits == 0x01) { d.file_type = (uint8_t)PCCC_FILE_INT; }  /* "Integer" */
            else if(radix_bits == 0x02) { d.file_type = (uint8_t)PCCC_FILE_ASCII; }
            else { /* radix 0x03: not assigned a meaning by the report for class 0 */
                d.file_type = (uint8_t)PCCC_FILE_UNKNOWN;
                d.has_elements = false;
            }
            break;

        case 0x01: /* Float, 2 words/element */
            if(d.total_words % 2 == 0) {
                d.file_type = (uint8_t)PCCC_FILE_FLOAT;
                d.has_elements = true;
                d.element_count = (uint32_t)(d.total_words / 2);
            } else {
                d.file_type = (uint8_t)PCCC_FILE_UNKNOWN; /* validation rule: footprint mismatch */
            }
            break;

        case 0x03: /* Timer, 3 words/element */
            if(d.total_words % 3 == 0) {
                d.file_type = (uint8_t)PCCC_FILE_TIMER;
                d.has_elements = true;
                d.element_count = (uint32_t)(d.total_words / 3);
            } else {
                d.file_type = (uint8_t)PCCC_FILE_UNKNOWN;
            }
            break;

        case 0x04: /* Counter, 3 words/element */
            if(d.total_words % 3 == 0) {
                d.file_type = (uint8_t)PCCC_FILE_COUNTER;
                d.has_elements = true;
                d.element_count = (uint32_t)(d.total_words / 3);
            } else {
                d.file_type = (uint8_t)PCCC_FILE_UNKNOWN;
            }
            break;

        case 0x05: /* Control, 3 words/element */
            if(d.total_words % 3 == 0) {
                d.file_type = (uint8_t)PCCC_FILE_CONTROL;
                d.has_elements = true;
                d.element_count = (uint32_t)(d.total_words / 3);
            } else {
                d.file_type = (uint8_t)PCCC_FILE_UNKNOWN;
            }
            break;

        case 0x07: /* String, 42 words/element */
            if(d.total_words % 42 == 0) {
                d.file_type = (uint8_t)PCCC_FILE_STRING;
                d.has_elements = true;
                d.element_count = (uint32_t)(d.total_words / 42);
            } else {
                d.file_type = (uint8_t)PCCC_FILE_UNKNOWN;
            }
            break;

        default: d.file_type = (uint8_t)PCCC_FILE_UNKNOWN; break; /* unassigned structure class */
    }

    return d;
}

/* One CBOR map per record -- the SAME shape for every PCCC platform
 * (PLC-5/SLC/MicroLogix): file_number, file_type (pccc_file_t code),
 * file_type_name (decoded via the shared pccc_file_type_name() lookup),
 * element_count when known, and raw (the untouched native record bytes, so
 * platform-specific extras this schema doesn't name -- PLC-5's attribute
 * byte, SLC/MicroLogix's unexplained trailing "reserved" field -- are not
 * lost, just not individually decoded). PLC-5 records are decoded first via
 * pccc_decode_plc5_file_record(); SLC/MicroLogix records already carry
 * file_number/file_type/element_count directly on the wire (see
 * enip_pccc_apply_listing in client/enip_session.c). */
static size_t pccc_listing_record_cbor_size(bool is_plc5, Bytes rec) {
    uint8_t file_number, file_type;
    bool has_elements;
    uint32_t element_count = 0;

    if(is_plc5) {
        pccc_plc5_file_decode_t d = pccc_decode_plc5_file_record(rec);
        file_number = d.file_number;
        file_type = d.file_type;
        has_elements = d.has_elements;
        element_count = d.element_count;
    } else {
        uint16_t elem_count16 = 0;
        bytes_unpack(rec, BYTES_LE, &file_number, &file_type, &elem_count16);
        has_elements = true;
        element_count = elem_count16;
    }

    const char *type_name = pccc_file_type_name(file_type);
    size_t sz = cbor_size_map_header(has_elements ? 5 : 4);
    sz += cbor_size_text(sizeof("file_number") - 1) + cbor_size_uint(file_number);
    sz += cbor_size_text(sizeof("file_type") - 1) + cbor_size_uint(file_type);
    sz += cbor_size_text(sizeof("file_type_name") - 1) + cbor_size_text((size_t)str_length(type_name));
    sz += cbor_size_text(sizeof("raw") - 1) + cbor_size_bytes(rec.len);
    if(has_elements) { sz += cbor_size_text(sizeof("element_count") - 1) + cbor_size_uint(element_count); }
    return sz;
}

static bool pccc_listing_record_cbor_write(Bytes dest, size_t *pos, bool is_plc5, Bytes rec) {
    uint8_t file_number, file_type;
    bool has_elements;
    uint32_t element_count = 0;

    if(is_plc5) {
        pccc_plc5_file_decode_t d = pccc_decode_plc5_file_record(rec);
        file_number = d.file_number;
        file_type = d.file_type;
        has_elements = d.has_elements;
        element_count = d.element_count;
    } else {
        uint16_t elem_count16 = 0;
        bytes_unpack(rec, BYTES_LE, &file_number, &file_type, &elem_count16);
        has_elements = true;
        element_count = elem_count16;
    }

    const char *type_name = pccc_file_type_name(file_type);
    if(!cbor_write_map_header(dest, pos, has_elements ? 5 : 4)) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT("file_number"))) { return false; }
    if(!cbor_write_uint(dest, pos, file_number)) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT("file_type"))) { return false; }
    if(!cbor_write_uint(dest, pos, file_type)) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT("file_type_name"))) { return false; }
    if(!cbor_write_text(dest, pos, type_name, (size_t)str_length(type_name))) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT("raw"))) { return false; }
    if(!cbor_write_bytes(dest, pos, rec.data, rec.len)) { return false; }
    if(has_elements) {
        if(!cbor_write_text(dest, pos, CBOR_LIT("element_count"))) { return false; }
        if(!cbor_write_uint(dest, pos, element_count)) { return false; }
    }
    return true;
}

static int enip_pccc_listing_get_formatted_data_size(plc_tag_p tag, plc_tag_format_type_t format) {
    if(format != PLCTAG_FORMAT_CBOR) { return PLCTAG_ERR_UNSUPPORTED; }

    bool is_plc5;
    if(!pccc_listing_is_pccc(tag, &is_plc5)) { return PLCTAG_ERR_UNSUPPORTED; }
    if(!tag->data) { return PLCTAG_ERR_NO_DATA; }

    size_t record_bytes = is_plc5 ? 4u : 6u;
    size_t record_count = (size_t)tag->size / record_bytes;

    size_t sz = cbor_size_map_header(3);
    sz += cbor_size_text(sizeof("schema") - 1) + cbor_size_text(sizeof(PCCC_LISTING_SCHEMA_NAME) - 1);
    sz += cbor_size_text(sizeof("schema-version") - 1) + cbor_size_uint(PCCC_LISTING_SCHEMA_VERSION);
    sz += cbor_size_text(sizeof("records") - 1) + cbor_size_array_header(record_count);
    for(size_t i = 0; i < record_count; i++) {
        sz += pccc_listing_record_cbor_size(is_plc5, bytes_from_buf(tag->data + i * record_bytes, record_bytes));
    }

    return (int)sz;
}

static int enip_pccc_listing_get_formatted_data(plc_tag_p tag, plc_tag_format_type_t format, uint8_t *buffer,
                                                 int buffer_length) {
    if(format != PLCTAG_FORMAT_CBOR) { return PLCTAG_ERR_UNSUPPORTED; }

    bool is_plc5;
    if(!pccc_listing_is_pccc(tag, &is_plc5)) { return PLCTAG_ERR_UNSUPPORTED; }
    if(!tag->data) { return PLCTAG_ERR_NO_DATA; }

    size_t record_bytes = is_plc5 ? 4u : 6u;
    size_t record_count = (size_t)tag->size / record_bytes;

    Bytes dest = bytes_from_buf(buffer, (size_t)buffer_length);
    size_t pos = 0;

    if(!cbor_write_map_header(dest, &pos, 3)) { return PLCTAG_ERR_TOO_SMALL; }
    if(!cbor_write_text(dest, &pos, CBOR_LIT("schema"))) { return PLCTAG_ERR_TOO_SMALL; }
    if(!cbor_write_text(dest, &pos, CBOR_LIT(PCCC_LISTING_SCHEMA_NAME))) { return PLCTAG_ERR_TOO_SMALL; }
    if(!cbor_write_text(dest, &pos, CBOR_LIT("schema-version"))) { return PLCTAG_ERR_TOO_SMALL; }
    if(!cbor_write_uint(dest, &pos, PCCC_LISTING_SCHEMA_VERSION)) { return PLCTAG_ERR_TOO_SMALL; }
    if(!cbor_write_text(dest, &pos, CBOR_LIT("records"))) { return PLCTAG_ERR_TOO_SMALL; }
    if(!cbor_write_array_header(dest, &pos, record_count)) { return PLCTAG_ERR_TOO_SMALL; }
    for(size_t i = 0; i < record_count; i++) {
        if(!pccc_listing_record_cbor_write(dest, &pos, is_plc5, bytes_from_buf(tag->data + i * record_bytes, record_bytes))) {
            return PLCTAG_ERR_TOO_SMALL;
        }
    }

    return PLCTAG_STATUS_OK;
}

/* Field-name reflection of the schema (matches @identity's get_schema
 * pattern). One field list for every PCCC platform, matching the unified
 * per-record shape above -- unlike the earlier per-platform design, this no
 * longer depends on is_plc5, only on PCCC-connection eligibility. */
static const char *const PCCC_LISTING_FIELDS[] = {"file_number", "file_type", "file_type_name", "element_count", "raw"};
#define PCCC_LISTING_FIELD_COUNT ((size_t)(sizeof(PCCC_LISTING_FIELDS) / sizeof(PCCC_LISTING_FIELDS[0])))

static size_t pccc_listing_schema_cbor_size(void) {
    size_t sz = cbor_size_map_header(3);
    sz += cbor_size_text(sizeof("schema") - 1) + cbor_size_text(sizeof(PCCC_LISTING_SCHEMA_NAME) - 1);
    sz += cbor_size_text(sizeof("schema-version") - 1) + cbor_size_uint(PCCC_LISTING_SCHEMA_VERSION);
    sz += cbor_size_text(sizeof("fields") - 1) + cbor_size_array_header(PCCC_LISTING_FIELD_COUNT);
    for(size_t i = 0; i < PCCC_LISTING_FIELD_COUNT; i++) { sz += cbor_size_text((size_t)str_length(PCCC_LISTING_FIELDS[i])); }
    return sz;
}

static bool pccc_listing_schema_cbor_write(Bytes dest, size_t *pos) {
    if(!cbor_write_map_header(dest, pos, 3)) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT("schema"))) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT(PCCC_LISTING_SCHEMA_NAME))) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT("schema-version"))) { return false; }
    if(!cbor_write_uint(dest, pos, PCCC_LISTING_SCHEMA_VERSION)) { return false; }
    if(!cbor_write_text(dest, pos, CBOR_LIT("fields"))) { return false; }
    if(!cbor_write_array_header(dest, pos, PCCC_LISTING_FIELD_COUNT)) { return false; }
    for(size_t i = 0; i < PCCC_LISTING_FIELD_COUNT; i++) {
        if(!cbor_write_text(dest, pos, PCCC_LISTING_FIELDS[i], (size_t)str_length(PCCC_LISTING_FIELDS[i]))) { return false; }
    }
    return true;
}

static int enip_pccc_listing_get_schema_size(plc_tag_p tag, plc_tag_format_type_t format) {
    if(format != PLCTAG_FORMAT_CBOR) { return PLCTAG_ERR_UNSUPPORTED; }
    bool is_plc5;
    if(!pccc_listing_is_pccc(tag, &is_plc5)) { return PLCTAG_ERR_UNSUPPORTED; }
    (void)is_plc5; /* eligibility only -- the schema no longer varies by platform */
    return (int)pccc_listing_schema_cbor_size();
}

static int enip_pccc_listing_get_schema(plc_tag_p tag, plc_tag_format_type_t format, uint8_t *buffer, int buffer_length) {
    if(format != PLCTAG_FORMAT_CBOR) { return PLCTAG_ERR_UNSUPPORTED; }
    bool is_plc5;
    if(!pccc_listing_is_pccc(tag, &is_plc5)) { return PLCTAG_ERR_UNSUPPORTED; }
    (void)is_plc5; /* eligibility only -- the schema no longer varies by platform */

    if(pccc_listing_schema_cbor_size() > (size_t)buffer_length) { return PLCTAG_ERR_TOO_SMALL; }

    Bytes dest = bytes_from_buf(buffer, (size_t)buffer_length);
    size_t pos = 0;
    if(!pccc_listing_schema_cbor_write(dest, &pos)) { return PLCTAG_ERR_TOO_SMALL; }

    return PLCTAG_STATUS_OK;
}

static struct tag_vtable_t enip_listing_tag_vtable = {
    .abort = enip_tag_abort,
    .read = enip_listing_tag_read,
    .status = enip_tag_status,
    .tickler = NULL,
    .write = NULL,
    .wake_plc = NULL,
    .tag_data_written = NULL,
    .get_int_attrib = NULL,
    .set_int_attrib = NULL,
    .get_byte_array_attrib = NULL,
    .get_formatted_data_size = enip_pccc_listing_get_formatted_data_size,
    .get_formatted_data = enip_pccc_listing_get_formatted_data,
    .set_formatted_data = NULL, /* read-only tag */
    .get_schema_size = enip_pccc_listing_get_schema_size,
    .get_schema = enip_pccc_listing_get_schema,
    .set_schema = NULL, /* built-in schema, not user-settable */
};

static int32_t enip_tag_data_written(plc_tag_p tag) {
    enip_tag_p t = (enip_tag_p)tag;

    int64_t fire_at = time_ms() + t->auto_sync_write_ms;

    if(t->auto_sync_next_write == 0 || t->auto_sync_next_write > fire_at) { t->auto_sync_next_write = fire_at; }

    return PLCTAG_STATUS_OK;
}

/* ============================================================================
 * Create / destroy
 * ============================================================================ */

static void enip_tag_destructor(void *tag_arg) {
    enip_tag_p t = (enip_tag_p)tag_arg;

    if(!t) { return; }

    if(t->conn) {
        enip_session_tag_detach(t->conn, t);
        t->conn = rc_dec(t->conn);
    }

    if(t->data) {
        mem_free(t->data);
        t->data = NULL;
    }

    if(t->api_mutex) { mutex_destroy(&t->api_mutex); }
    if(t->ext_mutex) { mutex_destroy(&t->ext_mutex); }
    if(t->tag_cond_wait) { cond_destroy(&t->tag_cond_wait); }
}

/* A trailing ".<digits>" on a tag name is a bit selector (e.g. "MyDint.5",
 * "TestINTArray[0].13"), not a structure member -- Logix member names are
 * never all-numeric. Returns the byte length of the name with the bit suffix
 * removed and sets *bit_out to the parsed bit number; when there is no bit
 * suffix returns the full length and sets *bit_out to -1. */
static size_t enip_split_bit_suffix(const char *name, int *bit_out) {
    *bit_out = -1;

    size_t len = strlen(name);
    const char *dot = strrchr(name, '.');
    if(!dot || dot == name || dot[1] == '\0') { return len; }

    for(const char *q = dot + 1; *q != '\0'; q++) {
        if(*q < '0' || *q > '9') { return len; }
    }

    *bit_out = (int)strtol(dot + 1, NULL, 10);
    return (size_t)(dot - name);
}

/* Allocate the tag with tag_name and the encoded CIP path packed into the
 * tail of the allocation (per enip_tag.h). */
static enip_tag_p create_tag_object(attr attribs) {
    const char *raw_name = attr_get_str(attribs, "name", NULL);

    if(!raw_name || str_length(raw_name) == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Missing required \"name\" attribute.");
        return NULL;
    }

    /* A trailing ".<bit>" is read as the whole underlying element; the generic
     * lib layer extracts the bit using tag->is_bit/tag->bit, so it must not go
     * into the encoded CIP symbolic path. */
    int bit = -1;
    size_t name_len = enip_split_bit_suffix(raw_name, &bit);

    Arena scratch;
    if(arena_init(&scratch, (size_t)1024) != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to allocate scratch arena!");
        return NULL;
    }

    /* NUL-terminated copy of the base name (bit suffix stripped) for encoding. */
    Bytes name_buf = bytes_alloc(&scratch, name_len + 1);
    if(bytes_is_null(name_buf)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to allocate name buffer!");
        arena_free(&scratch);
        return NULL;
    }
    bytes_pack_into(name_buf, BYTES_LE, bytes_from_buf((const uint8_t *)raw_name, name_len));
    name_buf.data[name_len] = '\0';
    const char *tag_name = (const char *)name_buf.data;

    Bytes encoded = enip_cip_encode_path(&scratch, tag_name);
    if(bytes_is_null(encoded)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to encode tag path for \"%s\"!", tag_name);
        arena_free(&scratch);
        return NULL;
    }

    size_t tail_size = name_len + 1 + encoded.len;

    enip_tag_p tag = (enip_tag_p)rc_alloc((int)(sizeof(struct enip_tag_t) + tail_size), enip_tag_destructor);
    if(!tag) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to allocate ENIP tag!");
        arena_free(&scratch);
        return NULL;
    }

    uint8_t *tail = (uint8_t *)(tag + 1);

    Bytes tail_rest = bytes_pack_into(bytes_from_buf(tail, tail_size), BYTES_LE,
                                      bytes_from_buf((const uint8_t *)tag_name, name_len + 1));
    tag->tag_name = (char *)tail;

    bytes_pack_into(tail_rest, BYTES_LE, encoded);
    tag->path = bytes_from_buf(tail + name_len + 1, encoded.len);

    arena_free(&scratch);

    tag->vtable = &enip_tag_vtable;
    tag->byte_order = &enip_tag_byte_order;
    tag->elem_count = (uint32_t)attr_get_int(attribs, "elem_count", 1);

    if(bit >= 0) {
        tag->is_bit = 1;
        tag->bit = bit;
    }

    return tag;
}

/* Reuse an ENIP data or @connection source tag's connection, else find-or-create
 * one from attribs. Sets *is_new true only for a freshly created connection. */
static enip_connection_t *enip_tag_get_conn(attr attribs, plc_tag_p src_tag, bool *is_new) {
    *is_new = false;

    if(src_tag
       && (src_tag->protocol_type == TAG_PROTOCOL_ENIP || src_tag->protocol_type == TAG_PROTOCOL_ENIP_CONNECTION)) {
        return rc_inc(((enip_tag_p)src_tag)->conn);
    }

    return enip_session_create(attribs, is_new);
}

/* Build a status-only @connection tag: no CIP path, no OPEN_PROBE. */
static plc_tag_p create_connection_tag(attr attribs,
                                       void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                       void *userdata, plc_tag_p src_tag) {
    enip_tag_p tag = (enip_tag_p)rc_alloc((int)sizeof(struct enip_tag_t), enip_tag_destructor);
    if(!tag) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to allocate @connection tag!");
        return NULL;
    }

    tag->vtable = &enip_connection_tag_vtable;
    tag->byte_order = &enip_tag_byte_order;
    tag->kind = ENIP_TAG_KIND_CONNECTION;
    tag->first_tickler_run = 1;
    tag->last_conn_state = (int32_t)PLCTAG_CONN_STATUS_DOWN;

    int32_t rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to initialize generic tag parts!");
        rc_dec(tag);
        return NULL;
    }

    tag->protocol_type = TAG_PROTOCOL_ENIP_CONNECTION;
    tag->skip_tickler = 1; /* the IO thread ticklers ENIP tags, not the global tickler */

    bool is_new = false;
    tag->conn = enip_tag_get_conn(attribs, src_tag, &is_new);

    if(!tag->conn) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to create or find a connection!");
        tag->status = (int8_t)PLCTAG_ERR_BAD_GATEWAY;
        tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, (int8_t)PLCTAG_ERR_BAD_GATEWAY);
        plc_tag_generic_handle_event_callbacks((plc_tag_p)tag);
        return (plc_tag_p)tag;
    }

    if(is_new) {
        /* Fresh session: observe the full transition history from the start. */
        tag->conn_status_read_idx = 0;
        tag->last_conn_state = (int32_t)PLCTAG_CONN_STATUS_DOWN;
    } else {
        /* Late join: snapshot the current state; the first tickler synthesises it. */
        tag->conn_status_read_idx = enip_session_conn_status_idx(tag->conn);
        tag->last_conn_state = enip_session_get_status(tag->conn);
    }

    tag->status = (int8_t)PLCTAG_STATUS_OK;
    tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, (int8_t)PLCTAG_STATUS_OK);

    /* Join the active list so the IO thread runs enip_conn_tag_tickler. */
    enip_session_schedule(tag->conn, tag, ENIP_OP_IDLE, time_ms());

    return (plc_tag_p)tag;
}

/* Build a read-only @identity tag: no CIP path, no OPEN_PROBE. Surfaces the
 * connection's cached CIP Identity payload. */
static plc_tag_p create_identity_tag(attr attribs,
                                     void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                     void *userdata, plc_tag_p src_tag) {
    enip_tag_p tag = (enip_tag_p)rc_alloc((int)sizeof(struct enip_tag_t), enip_tag_destructor);
    if(!tag) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to allocate @identity tag!");
        return NULL;
    }

    tag->vtable = &enip_identity_tag_vtable;
    tag->byte_order = &enip_tag_byte_order;
    tag->kind = ENIP_TAG_KIND_IDENTITY;

    int32_t rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to initialize generic tag parts!");
        rc_dec(tag);
        return NULL;
    }

    tag->protocol_type = TAG_PROTOCOL_ENIP;
    tag->skip_tickler = 1; /* the IO thread ticklers ENIP tags, not the global tickler */

    bool is_new = false;
    tag->conn = enip_tag_get_conn(attribs, src_tag, &is_new);

    if(!tag->conn) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to create or find a connection!");
        tag->status = (int8_t)PLCTAG_ERR_BAD_GATEWAY;
        tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, (int8_t)PLCTAG_ERR_BAD_GATEWAY);
        plc_tag_generic_handle_event_callbacks((plc_tag_p)tag);
        return (plc_tag_p)tag;
    }

    tag->status = (int8_t)PLCTAG_STATUS_OK;
    tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, (int8_t)PLCTAG_STATUS_OK);

    /* Join the active list; a read marks it due so the IO thread completes it. */
    enip_session_schedule(tag->conn, tag, ENIP_OP_IDLE, time_ms());

    return (plc_tag_p)tag;
}

/* Build a read-only @tags or @udt/<id> listing tag: no CIP path of its own (the
 * listing class/instance is built per-request), no OPEN_PROBE. ControlLogix-class
 * only; the IO thread rejects the network op on non-Rockwell devices. */
static plc_tag_p create_listing_tag(attr attribs,
                                    void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                    void *userdata, plc_tag_p src_tag, bool is_udt, uint16_t udt_id) {
    enip_tag_p tag = (enip_tag_p)rc_alloc((int)sizeof(struct enip_tag_t), enip_tag_destructor);
    if(!tag) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to allocate listing tag!");
        return NULL;
    }

    tag->vtable = &enip_listing_tag_vtable;
    tag->byte_order = &enip_tag_byte_order;
    tag->kind = is_udt ? ENIP_TAG_KIND_UDT : ENIP_TAG_KIND_LISTING;

    int32_t rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to initialize generic tag parts!");
        rc_dec(tag);
        return NULL;
    }

    tag->protocol_type = TAG_PROTOCOL_ENIP;
    tag->skip_tickler = 1; /* the IO thread ticklers ENIP tags, not the global tickler */
    tag->ready = 1;        /* no OPEN_PROBE; a read goes straight to the listing op */
    if(is_udt) { tag->list_next_id = udt_id; }

    bool is_new = false;
    tag->conn = enip_tag_get_conn(attribs, src_tag, &is_new);

    if(!tag->conn) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to create or find a connection!");
        tag->status = (int8_t)PLCTAG_ERR_BAD_GATEWAY;
        tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, (int8_t)PLCTAG_ERR_BAD_GATEWAY);
        plc_tag_generic_handle_event_callbacks((plc_tag_p)tag);
        return (plc_tag_p)tag;
    }

    tag->status = (int8_t)PLCTAG_STATUS_OK;
    tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, (int8_t)PLCTAG_STATUS_OK);

    /* Join the active list; a read marks it due so the IO thread runs the op. */
    enip_session_schedule(tag->conn, tag, ENIP_OP_IDLE, time_ms());

    return (plc_tag_p)tag;
}

/* Build a PLC-5/SLC/MicroLogix data tag from a parsed PCCC logical address.
 * No symbolic path and no OPEN_PROBE: the element size comes straight from the
 * address letter, so the data buffer is sized here and the tag is born ready.
 * The PCCC dialect (Execute-PCCC, CIP 0x4B) builds/parses the network ops. */
static plc_tag_p create_pccc_tag(attr attribs, const pccc_addr_t *addr,
                                 void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                 void *userdata, plc_tag_p src_tag) {
    enip_tag_p tag = (enip_tag_p)rc_alloc((int)sizeof(struct enip_tag_t), enip_tag_destructor);
    if(!tag) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to allocate PCCC tag!");
        return NULL;
    }

    tag->vtable = &enip_tag_vtable;
    tag->byte_order = &enip_tag_byte_order;
    tag->kind = ENIP_TAG_KIND_PCCC;
    tag->pccc_addr = *addr;
    tag->elem_size = (uint32_t)addr->element_size_bytes;
    tag->elem_count = (uint32_t)attr_get_int(attribs, "elem_count", 1);

    /* PLC-5 vs SLC/MicroLogix selects the address encoder and PCCC function
     * codes. Not derivable from CIP Identity, so honour an explicit hint
     * (plc=plc5 / cpu=plc-5); default to the SLC/MicroLogix family. */
    const char *plc = attr_get_str(attribs, "plc", attr_get_str(attribs, "cpu", ""));
    tag->pccc_plc5 = (uint8_t)(strchr(plc, '5') != NULL);

    if(addr->is_bit) {
        tag->is_bit = 1;
        tag->bit = addr->bit;
    }

    int32_t rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to initialize generic tag parts!");
        rc_dec(tag);
        return NULL;
    }

    tag->protocol_type = TAG_PROTOCOL_ENIP;
    tag->skip_tickler = 1; /* the IO thread ticklers ENIP tags, not the global tickler */

    /* Size and allocate the data buffer now -- there is no probe to learn it. */
    size_t total_size = (size_t)tag->elem_size * (size_t)tag->elem_count;
    if(total_size == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "PCCC tag has zero size (bad element size or count)!");
        rc_dec(tag);
        return NULL;
    }
    tag->data = mem_alloc((int)total_size);
    if(!tag->data) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to allocate PCCC tag data buffer!");
        rc_dec(tag);
        return NULL;
    }
    tag->size = (int32_t)total_size;
    /* window_elems/write_window_elems only gate Logix's batch-eligibility check
     * (is_batch_eligible, enip_session.c) and PCCC tags are never batch-eligible
     * (ENIP_TAG_KIND_PCCC), so these values are unused; set to the full tag for
     * consistency. Chunking a large PCCC op across multiple round trips is a
     * separate mechanism -- see the PCCC dialect's build/apply in enip_session.c. */
    tag->window_elems = tag->elem_count;
    tag->write_window_elems = tag->elem_count;
    tag->ready = 1; /* no OPEN_PROBE; a read/write goes straight to its PCCC op */

    bool is_new = false;
    tag->conn = enip_tag_get_conn(attribs, src_tag, &is_new);

    if(!tag->conn) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to create or find a connection!");
        tag->status = (int8_t)PLCTAG_ERR_BAD_GATEWAY;
        tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, (int8_t)PLCTAG_ERR_BAD_GATEWAY);
        plc_tag_generic_handle_event_callbacks((plc_tag_p)tag);
        return (plc_tag_p)tag;
    }

    tag->status = (int8_t)PLCTAG_STATUS_OK;
    tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, (int8_t)PLCTAG_STATUS_OK);

    /* Join the active list; a read/write marks it due so the IO thread runs the op. */
    enip_session_schedule(tag->conn, tag, ENIP_OP_IDLE, time_ms());

    return (plc_tag_p)tag;
}

plc_tag_p enip_tag_create_impl(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                void *userdata, plc_tag_p src_tag) {
    int32_t rc;

    if(str_cmp(attr_get_str(attribs, "name", ""), "@connection") == 0) {
        return create_connection_tag(attribs, tag_callback_func, userdata, src_tag);
    }

    if(str_cmp(attr_get_str(attribs, "name", ""), "@identity") == 0) {
        return create_identity_tag(attribs, tag_callback_func, userdata, src_tag);
    }

    /* @tags / @udt/<id> are ControlLogix-class listing tags (gated to Rockwell in
     * the IO thread, where the device identity is known). */
    if(str_cmp(attr_get_str(attribs, "name", ""), "@tags") == 0) {
        return create_listing_tag(attribs, tag_callback_func, userdata, src_tag, false, 0);
    }

    if(strncmp(attr_get_str(attribs, "name", ""), "@udt/", 5) == 0) {
        long udt_id = strtol(attr_get_str(attribs, "name", "") + 5, NULL, 10);
        if(udt_id < 0 || udt_id > 0xFFFF) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "@udt id must be between 0 and 65535!");
            return NULL;
        }
        return create_listing_tag(attribs, tag_callback_func, userdata, src_tag, true, (uint16_t)udt_id);
    }

    /* A PLC-5/SLC/MicroLogix logical address (N7:0, F8:0, B3:0/2, ...) parses
     * cleanly here; a Logix symbolic name does not. Use that as the PCCC
     * discriminator -- the device family is not yet known (Identity comes during
     * bring-up), but only a PCCC PLC accepts these addresses.
     * ponytail: name-syntax detection, not a separate plc= switch. */
    pccc_addr_t pccc_addr = {0};
    if(enip_pccc_parse_logical_address(attr_get_str(attribs, "name", ""), &pccc_addr) == PLCTAG_STATUS_OK
       && pccc_addr.file_type != PCCC_FILE_UNKNOWN) {
        return create_pccc_tag(attribs, &pccc_addr, tag_callback_func, userdata, src_tag);
    }

    enip_tag_p tag = create_tag_object(attribs);
    if(!tag) { return NULL; }

    rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to initialize generic tag parts!");
        rc_dec(tag);
        return NULL;
    }

    tag->protocol_type = TAG_PROTOCOL_ENIP;
    tag->skip_tickler = 1; /* the IO thread ticklers ENIP tags, not the global tickler */

    bool is_new = false;
    tag->conn = enip_tag_get_conn(attribs, src_tag, &is_new);

    if(!tag->conn) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to create or find a connection!");
        tag->status = (int8_t)PLCTAG_ERR_BAD_GATEWAY;
        tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, (int8_t)PLCTAG_ERR_BAD_GATEWAY);
        plc_tag_generic_handle_event_callbacks((plc_tag_p)tag);
        return (plc_tag_p)tag;
    }

    tag->status = (int8_t)PLCTAG_STATUS_PENDING;

    enip_session_schedule(tag->conn, tag, ENIP_OP_OPEN_PROBE, time_ms());

    return (plc_tag_p)tag;
}
