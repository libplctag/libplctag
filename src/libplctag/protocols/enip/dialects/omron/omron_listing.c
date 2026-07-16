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
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

/*
 * omron_listing.c — OMRON NJ/NX CIP tag and UDT listing dialect.
 *
 * Class 0x6A (Tag Name Server):
 *   Service 0x5F (GetInstanceListEx2) returns paged variable-name-server
 *   entries. Reply: instance_count(u16) | status_byte(u8) | reserved(u8),
 *   then per entry: data_length(u16) | class_id(u16) | instance_id(u32) |
 *   name_length(u8) | name(N bytes). status_byte != 0 means more follow.
 *
 * Class 0x6C (Variable Type Object):
 *   Service 0x01 (GetAttributeAll), instance = the template id registered
 *   via device_sim_add_udt_type -- our client sets list_next_id to this id
 *   directly at tag create (OMRON-SPECIFIC-DESIGN.md §5.3's class-0x6B
 *   variable_type_instance_id indirection is not needed for round trips
 *   against this simulator; class 0x6B itself is not implemented, see below).
 *
 * ponytail: §5.3's per-member sibling chain (next_instance_id walking
 * separate class-0x6C member instances, recursing into nested UDTs via
 * nesting_variable_type_instance_id) is not implemented -- the top-level
 * GetAttributeAll reply reports member_count/size/name faithfully but
 * next_instance_id is always 0. Nothing decodes member-level OMRON @udt
 * data yet (client/enip_session.c's enip_omron_apply_listing just
 * accumulates raw bytes -- see its doc comment), so there is no consumer to
 * validate a deeper member-instance graph against. Add the sibling chain
 * (and class 0x6B, which real OMRON uses to map a variable to its
 * variable_type_instance_id) when an OMRON @udt CBOR schema needs it.
 *
 * Wire format reference: OMRON-SPECIFIC-DESIGN.md §5.
 */

#include <stdint.h>

#include "platform.h"
#include "utils/bytes.h"
#include "utils/debug.h"
#include <libplctag/protocols/enip/server/device.h>
#include <libplctag/protocols/enip/server/device_sim.h>
#include "omron_listing.h"

#define CIP_SRV_GET_INSTANCE_LIST_EX2 ((uint8_t)0x5F)
#define CIP_SRV_GET_ATTR_ALL ((uint8_t)0x01)

/* Real OMRON Variable Object class -- used only as the informational
 * class_id field in class-0x6A records (§5.1); class 0x6B itself is not
 * implemented (see file doc comment). */
#define OMRON_VARIABLE_CLASS_ID ((uint16_t)0x6B)

/* Placeholder CIP data-type code flagging "this is a structure", mirroring
 * the low byte of Rockwell's CIP_TYPE_STRUCT_HEADER (0x02A0). OMRON's real
 * per-type code isn't documented in OMRON-SPECIFIC-DESIGN.md and nothing
 * decodes this field yet (see file doc comment). */
#define OMRON_CIP_DATA_TYPE_STRUCT ((uint8_t)0xA0)

static int32_t handle_tag_name_server(device_sim_t *sim, uint8_t service,
                                      const uint8_t *path, uint32_t path_len,
                                      const uint8_t *req, uint32_t req_len,
                                      uint8_t *resp, uint32_t resp_cap,
                                      uint32_t *resp_len, void *user_data);

static int32_t handle_variable_type(device_sim_t *sim, uint8_t service,
                                    const uint8_t *path, uint32_t path_len,
                                    const uint8_t *req, uint32_t req_len,
                                    uint8_t *resp, uint32_t resp_cap,
                                    uint32_t *resp_len, void *user_data);

extern int32_t omron_listing_register(device_sim_t *sim, device_t *dev) {
    int32_t rc;

    rc = device_sim_add_cip_object(sim, 0x6Au, DEVICE_SIM_ANY_INSTANCE, handle_tag_name_server, dev);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_ERROR, 0,
               "omron_listing_register: failed to register class 0x6A handler.");
        return rc;
    }

    rc = device_sim_add_cip_object(sim, 0x6Cu, DEVICE_SIM_ANY_INSTANCE, handle_variable_type, dev);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_ERROR, 0,
               "omron_listing_register: failed to register class 0x6C handler.");
        return rc;
    }

    pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_INFO, 0,
           "omron_listing_register: registered class 0x6A/0x6C handlers.");
    return PLCTAG_STATUS_OK;
}

/* Walk logical CIP path bytes and extract the instance_id (8-bit 0x24 or
 * 16-bit 0x25 instance segment, after an 8-bit 0x20 class segment). Local
 * copy of the same small parse used by dialects/rockwell/ab_listing.c --
 * each dialect owns its own path parsing rather than sharing a helper across
 * vendor modules. */
static void parse_instance(const uint8_t *path, uint32_t path_len, uint32_t *inst_out) {
    Bytes rest = bytes_from_buf(path, path_len);
    *inst_out = 0;

    while(rest.len > 0) {
        uint8_t seg = 0;
        Bytes next = bytes_unpack(rest, BYTES_LE, &seg);
        if(bytes_is_null(next)) { return; }
        rest = next;

        if(seg == 0x20) {
            uint8_t class_id = 0;
            next = bytes_unpack(rest, BYTES_LE, &class_id);
            if(bytes_is_null(next)) { return; }
            rest = next;
        } else if(seg == 0x24) {
            uint8_t inst8 = 0;
            if(!bytes_is_null(bytes_unpack(rest, BYTES_LE, &inst8))) { *inst_out = inst8; }
            return;
        } else if(seg == 0x25) {
            uint16_t inst16 = 0;
            if(!bytes_is_null(bytes_unpack(rest, BYTES_LE, BYTES_SKIP(1), &inst16))) { *inst_out = inst16; }
            return;
        } else {
            return;
        }
    }
}

/*
 * handle_tag_name_server — service 0x5F on class 0x6A (OMRON-SPECIFIC-DESIGN.md
 * §5.1). Request body: start_instance(u32 LE) | count(u32 LE) | kind(u16 LE).
 * `kind` is not filtered on -- device_sim's tag list has no system/user
 * distinction, so every non-PCCC tag is reported regardless of the requested
 * kind (real OMRON firmware separates kind=1 system / kind=2 user vars).
 */
static int32_t handle_tag_name_server(device_sim_t *sim, uint8_t service, const uint8_t *path, uint32_t path_len,
                                      const uint8_t *req, uint32_t req_len, uint8_t *resp, uint32_t resp_cap,
                                      uint32_t *resp_len, void *user_data) {
    device_t *dev = (device_t *)user_data;
    (void)sim;
    (void)path;
    (void)path_len;

    if(service != CIP_SRV_GET_INSTANCE_LIST_EX2) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0, "omron_listing: class 0x6A does not support service 0x%02x.",
               (unsigned)service);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    uint32_t start_instance = 0, max_count = 0;
    uint16_t kind = 0;
    if(bytes_is_null(bytes_unpack(bytes_from_buf(req, req_len), BYTES_LE, &start_instance, &max_count, &kind))) {
        return PLCTAG_ERR_TOO_SMALL;
    }
    (void)kind;

    Bytes resp_buf = bytes_from_buf(resp, resp_cap);
    if(resp_buf.len < 4) { return PLCTAG_ERR_TOO_SMALL; }
    Bytes body = bytes_slice(resp_buf, 4, resp_buf.len - 4); /* header (count/status/reserved) packed last */

    uint32_t packed = 0;
    uint8_t status_byte = 0;

    critical_block(dev->tags_mutex) {
        uint32_t instance_id = 0;
        tag_def_t *tag = dev->tags;

        while(tag) {
            instance_id++;

            if(tag->data_file_num != 0 || instance_id < start_instance) {
                tag = tag->next_tag;
                continue;
            }

            if(packed >= max_count) { status_byte = 1; break; }

            uint16_t name_len = (uint16_t)str_length(tag->name);
            uint16_t data_length = (uint16_t)(2u + 4u + 1u + name_len); /* class_id + instance_id + name_length + name */

            Bytes next = bytes_pack_into(body, BYTES_LE, data_length, (uint16_t)OMRON_VARIABLE_CLASS_ID, instance_id,
                                         (uint8_t)name_len, bytes_from_buf((const uint8_t *)tag->name, name_len));
            if(bytes_is_null(next)) { status_byte = 1; break; }
            body = next;
            packed++;

            tag = tag->next_tag;
        }
    }

    bytes_pack_into(resp_buf, BYTES_LE, (uint16_t)packed, status_byte, (uint8_t)0);
    *resp_len = (uint32_t)(resp_buf.len - body.len);

    pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_DETAIL, 0,
           "omron_listing: tag name server start=%u packed=%u more=%u.", (unsigned)start_instance, (unsigned)packed,
           (unsigned)status_byte);

    return PLCTAG_STATUS_OK;
}

/*
 * handle_variable_type — service 0x01 (GetAttributeAll) on class 0x6C
 * (OMRON-SPECIFIC-DESIGN.md §5.3), array_dimension always 0 (device_sim's
 * udt_template_t describes a scalar struct shape; array-of-struct shape is a
 * property of the owning tag, not the template). See the file doc comment
 * for the member-sibling-chain deferral (next_instance_id always 0).
 */
static int32_t handle_variable_type(device_sim_t *sim, uint8_t service, const uint8_t *path, uint32_t path_len,
                                    const uint8_t *req, uint32_t req_len, uint8_t *resp, uint32_t resp_cap,
                                    uint32_t *resp_len, void *user_data) {
    device_t *dev = (device_t *)user_data;
    (void)sim;
    (void)req;
    (void)req_len;

    *resp_len = 0;

    if(service != CIP_SRV_GET_ATTR_ALL) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0, "omron_listing: class 0x6C does not support service 0x%02x.",
               (unsigned)service);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    uint32_t template_id = 0;
    parse_instance(path, path_len, &template_id);

    udt_template_t *tmpl = device_udt_find(dev, (uint16_t)template_id);
    if(!tmpl) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0, "omron_listing: class 0x6C unknown template id %u.",
               (unsigned)template_id);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* Struct name is the NUL-delimited blob right after the member-info
     * array (device_sim.c's encode_udt_definition), terminated by ';' then
     * '\0' -- see device_sim_add_udt_type's doc comment. */
    uint8_t *name = tmpl->definition + (uint32_t)tmpl->num_members * 8u;
    uint32_t name_len = 0;
    while(name_len < tmpl->definition_len && name[name_len] != ';') { name_len++; }

    bool pad = (name_len % 2u) == 0u;

    Bytes resp_buf = bytes_from_buf(resp, resp_cap);
    Bytes rest = bytes_pack_into(resp_buf, BYTES_LE, (uint32_t)tmpl->instance_size, BYTES_SKIP(1),
                                 OMRON_CIP_DATA_TYPE_STRUCT, OMRON_CIP_DATA_TYPE_STRUCT, (uint8_t)0 /* array_dimension */,
                                 (uint16_t)tmpl->num_members, BYTES_SKIP(4), tmpl->handle, (uint8_t)name_len,
                                 bytes_from_buf(name, name_len));
    if(!bytes_is_null(rest) && pad) { rest = bytes_pack_into(rest, BYTES_LE, (uint8_t)0); }
    if(!bytes_is_null(rest)) {
        rest = bytes_pack_into(rest, BYTES_LE, (uint32_t)0 /* next_instance_id */, (uint32_t)0 /* nesting */);
    }
    if(bytes_is_null(rest)) { return PLCTAG_ERR_TOO_LARGE; }

    *resp_len = (uint32_t)(resp_buf.len - rest.len);

    pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_DETAIL, 0,
           "omron_listing: variable type %u ('%.*s') size=%u members=%u.", (unsigned)template_id, (int)name_len,
           (const char *)name, (unsigned)tmpl->instance_size, (unsigned)tmpl->num_members);

    return PLCTAG_STATUS_OK;
}
