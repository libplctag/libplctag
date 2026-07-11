/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever    *
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

#include <stddef.h>
#include <stdint.h>

#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include "platform.h"
#include "utils/attr.h"
#include "utils/debug.h"
#include "utils/rc.h"
#include "device.h"
#include "device_sim.h"
#include "endpoint.h"
#include "eip_server_tag.h"


/* ============================================================================
 * struct eip_server_tag_t
 * ============================================================================ */

struct eip_server_tag_t {
    TAG_BASE_STRUCT;

    device_sim_t *sim;     /* the endpoint this tag belongs to (refcounted via endpoint_release) */
    tag_def_t    *tag_def; /* this tag's entry in sim's device_t.tags list */
};

typedef struct eip_server_tag_t *eip_server_tag_p;

/* CIP types are little-endian on the wire (same layout as the client, see
 * client/enip_tag.c's enip_tag_byte_order — duplicated here rather than
 * exported, since it is one small const table). */
static tag_byte_order_t eip_server_tag_byte_order = {.is_allocated = 0,

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

/* ============================================================================
 * elem_type= / plc= string tables — matches src/tools/device_sim/args.c's
 * CIP_TYPES / parse_plc_type() tables (kept as a small local copy rather than
 * shared cross-file; args.c is CLI-only and not linked into the library).
 * ============================================================================ */

typedef struct {
    const char *name;
    tag_type_t  type;
    size_t      elem_size;
} cip_type_entry_t;

static const cip_type_entry_t CIP_TYPES[] = {
    {"BOOL",  TAG_CIP_TYPE_BOOL,  1},
    {"SINT",  TAG_CIP_TYPE_SINT,  1},
    {"INT",   TAG_CIP_TYPE_INT,   2},
    {"DINT",  TAG_CIP_TYPE_DINT,  4},
    {"LINT",  TAG_CIP_TYPE_LINT,  8},
    {"REAL",  TAG_CIP_TYPE_REAL,  4},
    {"LREAL", TAG_CIP_TYPE_LREAL, 8},
    {NULL,    0,                  0},
};

static bool lookup_cip_type(const char *name, tag_type_t *type_out, size_t *elem_size_out) {
    for(const cip_type_entry_t *e = CIP_TYPES; e->name; e++) {
        if(str_cmp_i(name, e->name) == 0) {
            *type_out = e->type;
            *elem_size_out = e->elem_size;
            return true;
        }
    }
    return false;
}

static plc_type_t parse_plc_type(const char *s) {
    if(!s || *s == '\0') { return PLC_CONTROL_LOGIX; }
    if(str_cmp_i(s, "ControlLogix") == 0 || str_cmp_i(s, "logix") == 0) { return PLC_CONTROL_LOGIX; }
    if(str_cmp_i(s, "Micro800") == 0) { return PLC_MICRO800; }
    if(str_cmp_i(s, "Omron") == 0) { return PLC_OMRON; }
    if(str_cmp_i(s, "PLC5") == 0 || str_cmp_i(s, "PLC/5") == 0) { return PLC_PLC5; }
    if(str_cmp_i(s, "SLC") == 0 || str_cmp_i(s, "SLC500") == 0) { return PLC_SLC; }
    if(str_cmp_i(s, "Micrologix") == 0) { return PLC_MICROLOGIX; }
    pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0, "eip_server_tag_create: unknown plc=\"%s\", defaulting to ControlLogix.", s);
    return PLC_CONTROL_LOGIX;
}

/* ============================================================================
 * Vtable
 * ============================================================================ */

static int eip_server_tag_abort(plc_tag_p tag) {
    tag->status = PLCTAG_STATUS_OK;
    return PLCTAG_STATUS_OK;
}

static int eip_server_tag_wake_plc(plc_tag_p tag) {
    (void)tag;
    return PLCTAG_STATUS_OK;
}

static int eip_server_tag_status(plc_tag_p tag) {
    tag->status = PLCTAG_STATUS_OK;
    return PLCTAG_STATUS_OK;
}

/* Called under tag->api_mutex (plc_tag_read's own critical_block); mirrors
 * system_tag_read's pattern. PLCTAG_EVENT_READ_STARTED is already raised by
 * plc_tag_read() itself before calling this. */
static int eip_server_tag_read(plc_tag_p ptag) {
    eip_server_tag_p tag = (eip_server_tag_p)ptag;
    tag_def_t *td = tag->tag_def;
    size_t total = td->elem_count * td->elem_size;

    mutex_lock(td->data_mutex);
    mem_copy(ptag->data, td->data, (int)total);
    mutex_unlock(td->data_mutex);

    tag_raise_event(ptag, PLCTAG_EVENT_READ_COMPLETED, PLCTAG_STATUS_OK);
    plc_tag_generic_handle_event_callbacks(ptag);

    return PLCTAG_STATUS_OK;
}

/* Called under tag->api_mutex (plc_tag_write's own critical_block); mirrors
 * system_tag_write's pattern. PLCTAG_EVENT_WRITE_STARTED is already raised by
 * plc_tag_write() itself before calling this. */
static int eip_server_tag_write(plc_tag_p ptag) {
    eip_server_tag_p tag = (eip_server_tag_p)ptag;
    tag_def_t *td = tag->tag_def;
    size_t total = td->elem_count * td->elem_size;

    mutex_lock(td->data_mutex);
    mem_copy(td->data, ptag->data, (int)total);
    mutex_unlock(td->data_mutex);

    tag_raise_event(ptag, PLCTAG_EVENT_WRITE_COMPLETED, PLCTAG_STATUS_OK);
    plc_tag_generic_handle_event_callbacks(ptag);

    return PLCTAG_STATUS_OK;
}

/*
 * Called by the shared tag-tickler thread, under tag->api_mutex (see
 * lib.c:tag_tickler_func). Detects a remote client's read/write (flagged by
 * the listener thread in common/cip.c's handle_read/handle_write, under
 * tag_def->data_mutex there — see device.h's tag_def_t comment) and:
 *   - refreshes ptag->data from the tag_def's buffer, so the app's next
 *     plc_tag_get_* and the create_ex callback both see the current value
 *     without an explicit plc_tag_read();
 *   - raises STARTED then COMPLETED itself, rather than setting
 *     ptag->read_complete/write_complete and leaving it to
 *     tag_tickler_func's own generic post-tickler check: that check calls
 *     tag_raise_event(..._COMPLETED) directly, but tag_raise_event's
 *     COMPLETED case is a no-op unless a prior STARTED already set the
 *     matching event_..._complete_enable flag — which normally happens
 *     inside plc_tag_read()/plc_tag_write()'s own generic wrapper, neither of
 *     which ever runs here because the read/write was driven by a remote
 *     peer, not a local call. Raising both ourselves (matching every other
 *     tag type's STARTED-then-COMPLETED pairing) sidesteps that gate.
 */
static int eip_server_tag_tickler(plc_tag_p ptag) {
    eip_server_tag_p tag = (eip_server_tag_p)ptag;
    tag_def_t *td = tag->tag_def;
    bool got_read = false;
    bool got_write = false;

    mutex_lock(td->data_mutex);
    if(td->pending_read_event || td->pending_write_event) {
        mem_copy(ptag->data, td->data, (int)(td->elem_count * td->elem_size));
    }
    if(td->pending_read_event) {
        td->pending_read_event = false;
        got_read = true;
    }
    if(td->pending_write_event) {
        td->pending_write_event = false;
        got_write = true;
    }
    mutex_unlock(td->data_mutex);

    if(got_read) {
        tag_raise_event(ptag, PLCTAG_EVENT_READ_STARTED, PLCTAG_STATUS_OK);
        tag_raise_event(ptag, PLCTAG_EVENT_READ_COMPLETED, PLCTAG_STATUS_OK);
    }
    if(got_write) {
        tag_raise_event(ptag, PLCTAG_EVENT_WRITE_STARTED, PLCTAG_STATUS_OK);
        tag_raise_event(ptag, PLCTAG_EVENT_WRITE_COMPLETED, PLCTAG_STATUS_OK);
    }
    if(got_read || got_write) { plc_tag_generic_handle_event_callbacks(ptag); }

    return PLCTAG_STATUS_OK;
}

static struct tag_vtable_t eip_server_tag_vtable = {
    .abort = eip_server_tag_abort,
    .read = eip_server_tag_read,
    .status = eip_server_tag_status,
    .tickler = eip_server_tag_tickler,
    .write = eip_server_tag_write,
    .wake_plc = eip_server_tag_wake_plc,
    .tag_data_written = NULL,

    .get_int_attrib = NULL,
    .set_int_attrib = NULL,
    .get_byte_array_attrib = NULL,
};

/* ============================================================================
 * Destroy
 * ============================================================================ */

static void eip_server_tag_destroy(void *tag_arg) {
    eip_server_tag_p tag = (eip_server_tag_p)tag_arg;
    plc_tag_p ptag = (plc_tag_p)tag;

    if(!tag) { return; }

    if(tag->tag_def) {
        device_t *dev = device_sim_get_device(tag->sim);
        device_tags_remove(dev, tag->tag_def);
        /* tag_def is deliberately not freed here — see device.h's tags field
         * comment. Its data/data_mutex stay valid but unreachable until the
         * whole endpoint (device_sim_destroy) tears down. */
    }
    if(tag->sim) { endpoint_release(tag->sim); }

    if(ptag->ext_mutex) { mutex_destroy(&ptag->ext_mutex); }
    if(ptag->api_mutex) { mutex_destroy(&ptag->api_mutex); }
    if(ptag->tag_cond_wait) { cond_destroy(&ptag->tag_cond_wait); }
}

/* ============================================================================
 * Create
 * ============================================================================ */

extern plc_tag_p eip_server_tag_create(attr attribs,
                                       void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                       void *userdata, plc_tag_p src_tag) {
    (void)src_tag;

    const char *name = attr_get_str(attribs, "name", NULL);
    if(!name || str_length(name) < 1) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_ERROR, 0, "eip_server_tag_create: name= is required.");
        return PLC_TAG_P_NULL;
    }

    const char *type_str = attr_get_str(attribs, "elem_type", NULL);
    tag_type_t tag_type = 0;
    size_t elem_size = 0;
    if(!type_str || !lookup_cip_type(type_str, &tag_type, &elem_size)) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_ERROR, 0, "eip_server_tag_create: elem_type= is required and must be one of BOOL/SINT/INT/DINT/LINT/REAL/LREAL (got \"%s\").",
               type_str ? type_str : "(none)");
        return PLC_TAG_P_NULL;
    }

    int elem_count_attr = attr_get_int(attribs, "elem_count", 1);
    if(elem_count_attr < 1) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_ERROR, 0, "eip_server_tag_create: elem_count= must be >= 1.");
        return PLC_TAG_P_NULL;
    }
    size_t elem_count = (size_t)elem_count_attr;

    const char *bind_addr = attr_get_str(attribs, "gateway", NULL);
    uint16_t port = (uint16_t)attr_get_int(attribs, "port", 44818);
    plc_type_t plc_type = parse_plc_type(attr_get_str(attribs, "plc", NULL));

    pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_INFO, 0, "eip_server_tag_create: name=\"%s\" type=%s elem_count=%zu gateway=%s port=%u.",
           name, type_str, elem_count, bind_addr ? bind_addr : "0.0.0.0", (unsigned)port);

    eip_server_tag_p tag = (eip_server_tag_p)rc_alloc((int)sizeof(struct eip_server_tag_t), eip_server_tag_destroy);
    if(!tag) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_ERROR, 0, "eip_server_tag_create: rc_alloc failed.");
        return PLC_TAG_P_NULL;
    }

    plc_tag_p ptag = (plc_tag_p)tag;
    ptag->vtable = &eip_server_tag_vtable;
    ptag->protocol_type = TAG_PROTOCOL_ENIP;

    int rc = plc_tag_generic_init_tag(ptag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_ERROR, 0, "eip_server_tag_create: plc_tag_generic_init_tag failed: %s.",
               plc_tag_decode_error(rc));
        rc_dec(tag);
        return PLC_TAG_P_NULL;
    }

    ptag->byte_order = &eip_server_tag_byte_order;

    size_t total_size = elem_size * elem_count;
    ptag->data = (uint8_t *)mem_alloc((int)total_size);
    if(!ptag->data) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_ERROR, 0, "eip_server_tag_create: failed to allocate %zu bytes of tag data.",
               total_size);
        rc_dec(tag);
        return PLC_TAG_P_NULL;
    }
    ptag->size = (int)total_size;

    tag->sim = endpoint_find_or_create(bind_addr, port, plc_type);
    if(!tag->sim) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_ERROR, 0, "eip_server_tag_create: endpoint_find_or_create failed.");
        rc_dec(tag);
        return PLC_TAG_P_NULL;
    }

    device_t *dev = device_sim_get_device(tag->sim);

    tag->tag_def = device_tag_alloc(name, tag_type, elem_size, elem_count, NULL, NULL, NULL);
    if(!tag->tag_def) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_ERROR, 0, "eip_server_tag_create: device_tag_alloc failed.");
        rc_dec(tag);
        return PLC_TAG_P_NULL;
    }
    tag->tag_def->num_dimensions = 1;
    tag->tag_def->dimensions[0] = elem_count;
    tag->tag_def->dimensions[1] = 1;
    tag->tag_def->dimensions[2] = 1;

    device_tags_append(dev, tag->tag_def);

    tag_raise_event(ptag, PLCTAG_EVENT_CREATED, PLCTAG_STATUS_OK);

    pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_INFO, tag->tag_id, "eip_server_tag_create: done.");

    return ptag;
}
