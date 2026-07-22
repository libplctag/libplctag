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
#include <string.h>

#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include "platform.h"
#include "utils/attr.h"
#include "utils/bytes.h"
#include "utils/debug.h"
#include "utils/rc.h"
#include "device.h"
#include "device_sim.h"
#include "device_types.h"
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
 * plc= string table. elem_type=/pccc_type= name->type+size lookups
 * (lookup_cip_type/lookup_pccc_type) now live in device_types.h (3.h),
 * shared with device_elem_size_for_type's type->size reverse lookup.
 * ============================================================================ */

static enip_plc_type_t parse_plc_type(const char *s) {
    if(!s || *s == '\0') { return ENIP_PLC_LGX; }
    if(str_cmp_i(s, "ControlLogix") == 0 || str_cmp_i(s, "logix") == 0) { return ENIP_PLC_LGX; }
    if(str_cmp_i(s, "Micro800") == 0) { return ENIP_PLC_MICRO800; }
    if(str_cmp_i(s, "Omron") == 0) { return ENIP_PLC_OMRON_NJNX; }
    if(str_cmp_i(s, "PLC5") == 0 || str_cmp_i(s, "PLC/5") == 0) { return ENIP_PLC_PLC5; }
    if(str_cmp_i(s, "SLC") == 0 || str_cmp_i(s, "SLC500") == 0) { return ENIP_PLC_SLC; }
    if(str_cmp_i(s, "Micrologix") == 0) { return ENIP_PLC_MLGX; }
    pdebug(DEBUG_MODULE_SERVER, PLCTAG_DEBUG_WARN, 0, "eip_server_tag_create: unknown plc=\"%s\", defaulting to ControlLogix.", s);
    return ENIP_PLC_LGX;
}

/* ============================================================================
 * udt= / elem_type=@<name> — UDT template registration and instantiation via
 * role=server attribute strings (ENIP-UPDATES-PLAN.md item 2). The internal
 * C API (device_sim_add_udt_type/udt_member_t) stays unexported; FFI callers
 * reach it only through these two attributes.
 * ============================================================================ */

/* Split "udt" attribute value "<name>:<member>,<member>,..." on the first
 * colon only (member specs have their own colons). *name_out is a fresh
 * mem_alloc'd copy the caller frees; *members_out points into udt_attr
 * itself (no member list is a bare "<name>", valid for a zero-member
 * template). Returns false only on allocation failure. */
static bool split_udt_attr(const char *udt_attr, char **name_out, const char **members_out) {
    const char *colon = strchr(udt_attr, ':');
    if(!colon) {
        *name_out = str_dup(udt_attr);
        *members_out = NULL;
        return *name_out != NULL;
    }

    int name_len = (int)(colon - udt_attr);
    char *name = (char *)mem_alloc(name_len + 1);
    if(!name) { return false; }
    mem_copy(name, (void *)(uintptr_t)udt_attr, name_len);
    name[name_len] = '\0';

    *name_out = name;
    *members_out = colon + 1;
    return true;
}

/* Parse one "mname:mtype[:count]" member spec into *out, resolving mtype
 * against CIP_TYPES or -- for "@<udtname>" -- an already-registered template
 * on this endpoint (nesting; the referenced UDT must have been declared by an
 * earlier tag on the same endpoint). Assigns out->offset from *offset_io
 * using natural alignment (member size, capped at 4 bytes -- BOOL members are
 * NOT bit-packed the way a real Logix UDT would; ponytail: full-byte BOOL is
 * the simplification, bit-packing is the upgrade path if a test ever needs
 * wire-exact BOOL member offsets), then advances *offset_io past it. Returns
 * false on any parse/lookup failure (out->name may be partially set; caller
 * discards on failure, so no cleanup here beyond the split array). */
static bool parse_udt_member(device_t *dev, const char *member_str, udt_member_t *out, uint32_t *offset_io,
                             uint32_t *max_align_io) {
    char **parts = str_split(member_str, ":");
    if(!parts || !parts[0] || !parts[1]) {
        pdebug(DEBUG_MODULE_SERVER, PLCTAG_DEBUG_ERROR, 0,
               "eip_server_tag_create: malformed udt= member \"%s\" (want name:type[:count]).", member_str);
        if(parts) { mem_free(parts); }
        return false;
    }

    uint32_t count = 1;
    if(parts[2]) {
        int count_val = 0;
        if(str_to_int(parts[2], &count_val) != PLCTAG_STATUS_OK || count_val < 1) {
            pdebug(DEBUG_MODULE_SERVER, PLCTAG_DEBUG_ERROR, 0,
                   "eip_server_tag_create: udt= member \"%s\" has a bad count.", member_str);
            mem_free(parts);
            return false;
        }
        count = (uint32_t)count_val;
    }

    tag_type_t mtype = 0;
    size_t melem_size = 0;
    if(parts[1][0] == '@') {
        udt_template_t *nested = device_udt_find_by_name(dev, parts[1] + 1);
        if(!nested) {
            pdebug(DEBUG_MODULE_SERVER, PLCTAG_DEBUG_ERROR, 0,
                   "eip_server_tag_create: udt= member \"%s\" references unknown nested UDT \"%s\".", member_str, parts[1] + 1);
            mem_free(parts);
            return false;
        }
        mtype = DEVICE_SIM_STRUCTURE_TYPE(nested->template_id);
        melem_size = nested->instance_size;
    } else if(!lookup_cip_type(parts[1], &mtype, &melem_size)) {
        pdebug(DEBUG_MODULE_SERVER, PLCTAG_DEBUG_ERROR, 0,
               "eip_server_tag_create: udt= member \"%s\" has an unknown type.", member_str);
        mem_free(parts);
        return false;
    }

    uint32_t align = (uint32_t)((melem_size < 4) ? melem_size : 4);
    if(align == 0) { align = 1; }
    uint32_t offset = (*offset_io + (align - 1)) / align * align;

    out->name = str_dup(parts[0]);
    out->type = mtype;
    out->array_count = count;
    out->offset = offset;

    *offset_io = offset + (uint32_t)melem_size * count;
    if(align > *max_align_io) { *max_align_io = align; }

    mem_free(parts);
    return out->name != NULL;
}

/* Find an existing template named struct_name on dev, or parse
 * member_list_str ("m1:t1[:c1],m2:t2[:c2],...", possibly NULL/empty for a
 * zero-member template) and register a new one. Returns NULL on any parse or
 * registration failure. */
static udt_template_t *resolve_or_register_udt(device_sim_t *sim, device_t *dev, const char *struct_name,
                                               const char *member_list_str) {
    udt_template_t *existing = device_udt_find_by_name(dev, struct_name);
    if(existing) { return existing; }

    char **member_toks = (member_list_str && *member_list_str) ? str_split(member_list_str, ",") : NULL;
    uint32_t num_members = 0;
    if(member_toks) { while(member_toks[num_members]) { num_members++; } }

    udt_member_t *members = NULL;
    if(num_members > 0) {
        members = (udt_member_t *)mem_alloc((int)(num_members * sizeof(udt_member_t)));
        if(!members) {
            if(member_toks) { mem_free(member_toks); }
            return NULL;
        }
        mem_set(members, 0, (int)(num_members * sizeof(udt_member_t)));
    }

    uint32_t offset = 0;
    uint32_t max_align = 1;
    bool ok = true;
    for(uint32_t i = 0; i < num_members && ok; i++) {
        ok = parse_udt_member(dev, member_toks[i], &members[i], &offset, &max_align);
    }

    udt_template_t *result = NULL;
    if(ok) {
        uint32_t instance_size = (offset > 0) ? (offset + (max_align - 1)) / max_align * max_align : (uint32_t)1;
        uint16_t template_id = 0;
        if(device_sim_add_udt_type(sim, struct_name, instance_size, members, num_members, &template_id) == PLCTAG_STATUS_OK) {
            result = device_udt_find(dev, template_id);
        }
    }

    for(uint32_t i = 0; i < num_members; i++) {
        if(members[i].name) { mem_free((void *)members[i].name); }
    }
    if(members) { mem_free(members); }
    if(member_toks) { mem_free(member_toks); }

    return result;
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
    bytes_pack_into(bytes_from_buf(ptag->data, total), BYTES_LE, bytes_from_buf(td->data, total));
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
    bytes_pack_into(bytes_from_buf(td->data, total), BYTES_LE, bytes_from_buf(ptag->data, total));
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
        size_t tag_size = td->elem_count * td->elem_size;
        bytes_pack_into(bytes_from_buf(ptag->data, tag_size), BYTES_LE, bytes_from_buf(td->data, tag_size));
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
        pdebug(DEBUG_MODULE_SERVER, PLCTAG_DEBUG_ERROR, 0, "eip_server_tag_create: name= is required.");
        return PLC_TAG_P_NULL;
    }

    /* No separate "port" attribute: gateway is "addr" or "addr:port" (same
     * convention as the client's enip_session_create), the only place a
     * non-default port is ever specified. */
    const char *gateway_raw = attr_get_str(attribs, "gateway", NULL);
    const char *bind_addr = NULL;
    uint16_t port = 44818;
    char **addr_port = NULL;
    if(gateway_raw && str_length(gateway_raw) > 0) {
        addr_port = str_split(gateway_raw, ":");
        if(!addr_port || !addr_port[0]) {
            pdebug(DEBUG_MODULE_SERVER, PLCTAG_DEBUG_ERROR, 0, "eip_server_tag_create: malformed \"gateway\" attribute \"%s\".",
                   gateway_raw);
            if(addr_port) { mem_free(addr_port); }
            return PLC_TAG_P_NULL;
        }
        bind_addr = addr_port[0];
        if(addr_port[1]) {
            int port_val = 0;
            if(str_to_int(addr_port[1], &port_val) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_SERVER, PLCTAG_DEBUG_ERROR, 0,
                       "eip_server_tag_create: unable to extract port number from gateway string \"%s\".", gateway_raw);
                mem_free(addr_port);
                return PLC_TAG_P_NULL;
            }
            port = (uint16_t)port_val;
        }
    }
    enip_plc_type_t plc_type = parse_plc_type(attr_get_str(attribs, "plc", NULL));
    const char *model = attr_get_str(attribs, "model", NULL);

    /* bind_addr points into addr_port's single allocation, freed right below
     * -- copy it out first since the info log further down (after type
     * resolution) still wants it for the gateway=%s message. */
    char bind_addr_buf[64];
    str_copy(bind_addr_buf, (int)sizeof(bind_addr_buf), bind_addr ? bind_addr : "0.0.0.0");

    /* Endpoint (and its listener thread) is resolved up front -- before type
     * resolution -- because udt=/elem_type=@name (below) need to look up or
     * register a UDT template on this specific endpoint's device_t. Every
     * failure path from here on must endpoint_release(sim) until tag->sim
     * takes over that ownership (right after rc_alloc succeeds). */
    device_sim_t *sim = endpoint_find_or_create(bind_addr, port, plc_type, model);
    if(addr_port) { mem_free(addr_port); }
    bind_addr = NULL; /* dangling now; use bind_addr_buf below */
    if(!sim) {
        pdebug(DEBUG_MODULE_SERVER, PLCTAG_DEBUG_ERROR, 0, "eip_server_tag_create: endpoint_find_or_create failed.");
        return PLC_TAG_P_NULL;
    }
    device_t *dev = device_sim_get_device(sim);

    const char *pccc_type_str = attr_get_str(attribs, "pccc_type", NULL);
    bool is_pccc = pccc_type_str && str_length(pccc_type_str) > 0;

    const char *udt_attr = !is_pccc ? attr_get_str(attribs, "udt", NULL) : NULL;
    const char *elem_type_str = is_pccc ? pccc_type_str : attr_get_str(attribs, "elem_type", NULL);

    tag_type_t tag_type = 0;
    size_t elem_size = 0;
    bool type_ok = false;
    const char *type_desc = elem_type_str; /* for the "type=%s" info log below */

    if(is_pccc) {
        type_ok = elem_type_str && lookup_pccc_type(elem_type_str, &tag_type, &elem_size);
    } else if(udt_attr && str_length(udt_attr) > 0) {
        char *struct_name = NULL;
        const char *member_list = NULL;
        if(split_udt_attr(udt_attr, &struct_name, &member_list)) {
            udt_template_t *tmpl = resolve_or_register_udt(sim, dev, struct_name, member_list);
            if(tmpl) {
                tag_type = DEVICE_SIM_STRUCTURE_TYPE(tmpl->template_id);
                elem_size = tmpl->instance_size;
                type_ok = true;
            } else {
                pdebug(DEBUG_MODULE_SERVER, PLCTAG_DEBUG_ERROR, 0,
                       "eip_server_tag_create: udt=\"%s\" failed to parse or register.", udt_attr);
            }
            mem_free(struct_name);
        }
        type_desc = udt_attr;
    } else if(elem_type_str && elem_type_str[0] == '@') {
        udt_template_t *tmpl = device_udt_find_by_name(dev, elem_type_str + 1);
        if(tmpl) {
            tag_type = DEVICE_SIM_STRUCTURE_TYPE(tmpl->template_id);
            elem_size = tmpl->instance_size;
            type_ok = true;
        } else {
            pdebug(DEBUG_MODULE_SERVER, PLCTAG_DEBUG_ERROR, 0,
                   "eip_server_tag_create: elem_type=%s references a UDT not yet declared on this endpoint.", elem_type_str);
        }
    } else {
        type_ok = elem_type_str && lookup_cip_type(elem_type_str, &tag_type, &elem_size);
    }

    if(!type_ok) {
        pdebug(DEBUG_MODULE_SERVER, PLCTAG_DEBUG_ERROR, 0,
               "eip_server_tag_create: %s is required and must be one of %s, udt=<name>[:members], or elem_type=@<udtname> (got \"%s\").",
               is_pccc ? "pccc_type=" : "elem_type=", is_pccc ? "B/N/L/F/R/ST" : "BOOL/SINT/INT/DINT/LINT/USINT/UINT/UDINT/ULINT/REAL/LREAL/BYTE/WORD/DWORD/LWORD/STRING",
               elem_type_str ? elem_type_str : (udt_attr ? udt_attr : "(none)"));
        endpoint_release(sim);
        return PLC_TAG_P_NULL;
    }

    /* dim0/dim1/dim2 (CIP only) describe a multi-dimensional array for
     * wire-level indexed addressing (common/cip.c) and @tags dimension
     * reporting (dialects/rockwell/ab_listing.c); PCCC tags stay 1D. When
     * unset, elem_count= gives a flat 1D array (the common case). */
    uint32_t num_dims = 1;
    uint32_t dims[3] = {1, 1, 1};
    if(!is_pccc && attr_get_int(attribs, "dim0", 0) > 0) {
        dims[0] = (uint32_t)attr_get_int(attribs, "dim0", 0);
        if(attr_get_int(attribs, "dim1", 0) > 0) {
            num_dims = 2;
            dims[1] = (uint32_t)attr_get_int(attribs, "dim1", 0);
            if(attr_get_int(attribs, "dim2", 0) > 0) {
                num_dims = 3;
                dims[2] = (uint32_t)attr_get_int(attribs, "dim2", 0);
            }
        }
    } else {
        int elem_count_attr = attr_get_int(attribs, "elem_count", 1);
        if(elem_count_attr < 1) {
            pdebug(DEBUG_MODULE_SERVER, PLCTAG_DEBUG_ERROR, 0, "eip_server_tag_create: elem_count= must be >= 1.");
            endpoint_release(sim);
            return PLC_TAG_P_NULL;
        }
        dims[0] = (uint32_t)elem_count_attr;
    }
    size_t elem_count = (size_t)dims[0] * (size_t)dims[1] * (size_t)dims[2];

    pdebug(DEBUG_MODULE_SERVER, PLCTAG_DEBUG_INFO, 0, "eip_server_tag_create: name=\"%s\" type=%s elem_count=%zu gateway=%s port=%u.",
           name, type_desc ? type_desc : "(udt)", elem_count, bind_addr_buf, (unsigned)port);

    eip_server_tag_p tag = (eip_server_tag_p)rc_alloc((int)sizeof(struct eip_server_tag_t), eip_server_tag_destroy);
    if(!tag) {
        pdebug(DEBUG_MODULE_SERVER, PLCTAG_DEBUG_ERROR, 0, "eip_server_tag_create: rc_alloc failed.");
        endpoint_release(sim);
        return PLC_TAG_P_NULL;
    }

    plc_tag_p ptag = (plc_tag_p)tag;
    ptag->vtable = &eip_server_tag_vtable;
    ptag->protocol_type = TAG_PROTOCOL_ENIP;

    /* Ownership of sim transfers to tag from here on: eip_server_tag_destroy
     * (rc_dec below on any later failure) calls endpoint_release(tag->sim). */
    tag->sim = sim;

    int rc = plc_tag_generic_init_tag(ptag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_SERVER, PLCTAG_DEBUG_ERROR, 0, "eip_server_tag_create: plc_tag_generic_init_tag failed: %s.",
               plc_tag_decode_error(rc));
        rc_dec(tag);
        return PLC_TAG_P_NULL;
    }

    ptag->byte_order = &eip_server_tag_byte_order;

    size_t total_size = elem_size * elem_count;
    ptag->data = (uint8_t *)mem_alloc((int)total_size);
    if(!ptag->data) {
        pdebug(DEBUG_MODULE_SERVER, PLCTAG_DEBUG_ERROR, 0, "eip_server_tag_create: failed to allocate %zu bytes of tag data.",
               total_size);
        rc_dec(tag);
        return PLC_TAG_P_NULL;
    }
    ptag->size = (int)total_size;

    tag->tag_def = device_tag_alloc(name, tag_type, elem_size, elem_count, NULL, NULL, NULL);
    if(!tag->tag_def) {
        pdebug(DEBUG_MODULE_SERVER, PLCTAG_DEBUG_ERROR, 0, "eip_server_tag_create: device_tag_alloc failed.");
        rc_dec(tag);
        return PLC_TAG_P_NULL;
    }
    tag->tag_def->num_dimensions = num_dims;
    tag->tag_def->dimensions[0] = dims[0];
    tag->tag_def->dimensions[1] = dims[1];
    tag->tag_def->dimensions[2] = dims[2];
    tag->tag_def->fault_status = (uint8_t)attr_get_int(attribs, "sim_fault", 0);
    if(is_pccc) { tag->tag_def->data_file_num = (size_t)attr_get_int(attribs, "pccc_file", 0); }

    device_tags_append(dev, tag->tag_def);

    /* sim_delay_ms / sim_max_packet are endpoint-scoped (SERVER_TAGS.md §7):
     * they take effect only for the tag that starts a new endpoint, since a
     * running endpoint's response delay and packet-size limits are shared by
     * every tag on it (mirrors the device_sim CLI's --delay=). 0 (unset)
     * leaves the endpoint's defaults untouched. */
    int32_t sim_delay_ms = (int32_t)attr_get_int(attribs, "sim_delay_ms", 0);
    if(sim_delay_ms != 0) { device_sim_set_response_delay(tag->sim, (uint32_t)sim_delay_ms); }
    int32_t sim_max_packet = (int32_t)attr_get_int(attribs, "sim_max_packet", 0);
    if(sim_max_packet != 0) { device_sim_set_max_packet(tag->sim, (uint32_t)sim_max_packet, (uint32_t)sim_max_packet); }

    tag_raise_event(ptag, PLCTAG_EVENT_CREATED, PLCTAG_STATUS_OK);

    pdebug(DEBUG_MODULE_SERVER, PLCTAG_DEBUG_INFO, tag->tag_id, "eip_server_tag_create: done.");

    return ptag;
}
