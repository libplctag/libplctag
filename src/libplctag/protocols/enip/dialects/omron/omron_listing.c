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
 * §5.3's per-member sibling chain is implemented: each member of a template
 * gets a synthetic class-0x6C instance id (see member_id_encode/decode
 * below) so next_instance_id can walk the member list and
 * nesting_variable_type_instance_id can point straight at a nested member's
 * own template id (real class-0x6C instances, needing no synthesis). Class
 * 0x6B (Variable Object, §5.2) itself is still not implemented -- our client
 * sets list_next_id to the template id directly at tag create (see
 * client/enip_session.c), so nothing here needs the variable ->
 * variable_type_instance_id indirection.
 *
 * Wire format reference: OMRON-SPECIFIC-DESIGN.md §5.
 */

#include <stdint.h>

#include "platform.h"
#include "utils/bytes.h"
#include "utils/debug.h"
#include <libplctag/protocols/enip/common/cip_path.h>
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

/* Walk logical CIP path bytes and extract the instance_id, via the shared
 * cip_path_parse codec (common/cip_path.c). Best-effort like the original:
 * *inst_out stays 0 on any malformed/unsupported path rather than erroring. */
static void parse_instance(const uint8_t *path, uint32_t path_len, uint32_t *inst_out) {
    cip_path_ids_t ids;
    *inst_out = cip_path_parse(bytes_from_buf(path, path_len), &ids) ? ids.instance_id : 0;
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

/* Synthetic class-0x6C instance id for template_id's member_index'th member
 * (§5.3's sibling records are separate CIP instances on real OMRON firmware;
 * device_sim has no real per-member instance table, so it synthesizes one
 * deterministically instead of allocating/storing it). Always > 0xFFFF so it
 * can never collide with a real template id (capped below 0x0FFF -- see
 * device_sim_add_udt_type) and always needs the 32-bit (0x26) path segment,
 * leaving every existing 16-bit top-level request untouched. member_index is
 * capped at 255 -- generous for any realistic UDT. */
/* Bit 24: high enough that after decode's >>8 it lands at bit 16, outside
 * the 12-bit (bits 0-11) template_id mask below -- a lower flag bit (e.g.
 * bit 16) would land inside that mask after the shift and corrupt the
 * decoded template_id. */
#define OMRON_MEMBER_ID_FLAG ((uint32_t)0x01000000u)

static uint32_t member_id_encode(uint16_t template_id, uint16_t member_index) {
    return OMRON_MEMBER_ID_FLAG | ((uint32_t)template_id << 8) | (uint32_t)(member_index & 0xFFu);
}

static bool member_id_decode(uint32_t id, uint16_t *template_id_out, uint16_t *member_index_out) {
    if((id & OMRON_MEMBER_ID_FLAG) == 0) { return false; }
    *template_id_out = (uint16_t)((id >> 8) & 0x0FFFu);
    *member_index_out = (uint16_t)(id & 0xFFu);
    return true;
}

/* tmpl->definition is [member-info array: {type(u16),info(u16),offset(u32)}
 * per member][struct_name ';' '\0'][member name '\0' ...], in that order
 * (device_sim.c's encode_udt_definition). Locates member[idx]'s type/count
 * and NUL-terminated name within it. Returns false if idx or the blob is out
 * of range. */
static bool find_member(udt_template_t *tmpl, uint16_t idx, uint16_t *type_out, uint16_t *array_count_out,
                        const uint8_t **name_out, uint32_t *name_len_out) {
    if(idx >= tmpl->num_members) { return false; }

    uint32_t info_off = (uint32_t)idx * 8u;
    if(info_off + 8u > tmpl->definition_len) { return false; }
    bytes_unpack(bytes_from_buf(tmpl->definition + info_off, 8), BYTES_LE, type_out, array_count_out, BYTES_SKIP(4));

    uint8_t *p = tmpl->definition + (uint32_t)tmpl->num_members * 8u;
    uint8_t *end = tmpl->definition + tmpl->definition_len;

    while(p < end && *p != '\0') { p++; } /* skip struct_name ';' */
    if(p >= end) { return false; }
    p++; /* past the '\0' following ';' */

    for(uint16_t i = 0; i < idx; i++) {
        while(p < end && *p != '\0') { p++; }
        if(p >= end) { return false; }
        p++;
    }

    uint8_t *start = p;
    while(p < end && *p != '\0') { p++; }
    if(p > end) { return false; }

    *name_out = start;
    *name_len_out = (uint32_t)(p - start);
    return true;
}

/* Common §5.3 Variable Type Object reply encoder, shared by the top-level
 * (whole-template) and per-member replies -- only the field values differ.
 * array_dimension is always 0 here: neither device_sim's template shape nor
 * a scalar member's shape needs the number_of_elements/start_array_elements
 * arrays (an array *member*'s count is reported via a size_in_memory that
 * already accounts for it, matching how device_sim tracks array tags
 * elsewhere -- see the file doc comment). */
static int32_t encode_variable_type_reply(uint32_t size_in_memory, uint8_t cip_data_type, uint16_t num_members,
                                          uint16_t crc, const uint8_t *name, uint32_t name_len, uint32_t next_instance_id,
                                          uint32_t nesting_instance_id, uint8_t *resp, uint32_t resp_cap,
                                          uint32_t *resp_len) {
    bool pad = (name_len % 2u) == 0u;

    Bytes resp_buf = bytes_from_buf(resp, resp_cap);
    Bytes rest = bytes_pack_into(resp_buf, BYTES_LE, size_in_memory, BYTES_SKIP(1), cip_data_type, cip_data_type,
                                 (uint8_t)0 /* array_dimension */, num_members, BYTES_SKIP(4), crc, (uint8_t)name_len,
                                 bytes_from_buf(name, name_len));
    if(!bytes_is_null(rest) && pad) { rest = bytes_pack_into(rest, BYTES_LE, (uint8_t)0); }
    if(!bytes_is_null(rest)) { rest = bytes_pack_into(rest, BYTES_LE, next_instance_id, nesting_instance_id); }
    if(bytes_is_null(rest)) { return PLCTAG_ERR_TOO_LARGE; }

    *resp_len = (uint32_t)(resp_buf.len - rest.len);
    return PLCTAG_STATUS_OK;
}

/* Whole-template reply: instance id is a real template id. next_instance_id
 * starts the member sibling chain (member 0), or 0 if the template has no
 * members. */
static int32_t handle_variable_type_template(device_t *dev, uint16_t template_id, uint8_t *resp, uint32_t resp_cap,
                                              uint32_t *resp_len) {
    udt_template_t *tmpl = device_udt_find(dev, template_id);
    if(!tmpl) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0, "omron_listing: class 0x6C unknown template id %u.",
               (unsigned)template_id);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    uint8_t *name = tmpl->definition + (uint32_t)tmpl->num_members * 8u;
    uint32_t name_len = 0;
    while(name_len < tmpl->definition_len && name[name_len] != ';') { name_len++; }

    uint32_t next_id = (tmpl->num_members > 0) ? member_id_encode(template_id, 0) : 0;

    int32_t rc = encode_variable_type_reply((uint32_t)tmpl->instance_size, OMRON_CIP_DATA_TYPE_STRUCT, tmpl->num_members,
                                            tmpl->handle, name, name_len, next_id, 0, resp, resp_cap, resp_len);
    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_DETAIL, 0,
               "omron_listing: variable type %u ('%.*s') size=%u members=%u.", (unsigned)template_id, (int)name_len,
               (const char *)name, (unsigned)tmpl->instance_size, (unsigned)tmpl->num_members);
    }
    return rc;
}

/* One member's reply: a synthetic sibling instance. Atomic members report
 * their own type/size directly; a member typed DEVICE_SIM_STRUCTURE_TYPE(id)
 * reports OMRON_CIP_DATA_TYPE_STRUCT and points nesting_variable_type_instance_id
 * at that nested template's own (real) id, so the client's next GetAttributeAll
 * for the nested UDT lands back in handle_variable_type_template above --
 * recursion needs no synthetic id of its own. next_instance_id continues this
 * template's own sibling chain (member_index + 1, or 0 if this was the last). */
static int32_t handle_variable_type_member(device_t *dev, uint16_t template_id, uint16_t member_index, uint8_t *resp,
                                           uint32_t resp_cap, uint32_t *resp_len) {
    udt_template_t *tmpl = device_udt_find(dev, template_id);
    if(!tmpl) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0, "omron_listing: class 0x6C member request for unknown template id %u.",
               (unsigned)template_id);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    uint16_t type = 0, array_count = 0;
    const uint8_t *name = NULL;
    uint32_t name_len = 0;
    if(!find_member(tmpl, member_index, &type, &array_count, &name, &name_len)) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0, "omron_listing: template %u has no member %u.", (unsigned)template_id,
               (unsigned)member_index);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    bool is_struct = (type & 0x8000u) != 0;
    uint16_t nested_id = (uint16_t)(type & 0x0FFFu);
    udt_template_t *nested = is_struct ? device_udt_find(dev, nested_id) : NULL;

    uint32_t size_in_memory;
    uint8_t cip_data_type;
    uint16_t num_members;
    uint16_t crc;
    uint32_t nesting_instance_id;

    if(is_struct) {
        size_in_memory = nested ? nested->instance_size : 0;
        cip_data_type = OMRON_CIP_DATA_TYPE_STRUCT;
        num_members = nested ? nested->num_members : 0;
        crc = nested ? nested->handle : 0;
        nesting_instance_id = nested ? nested_id : 0;
    } else {
        size_t elem_size = device_elem_size_for_type((tag_type_t)type);
        size_in_memory = (uint32_t)(elem_size * (array_count > 1 ? array_count : 1));
        cip_data_type = (uint8_t)(type & 0xFFu);
        num_members = 0;
        crc = 0;
        nesting_instance_id = 0;
    }

    uint32_t next_id = ((uint32_t)(member_index + 1) < tmpl->num_members) ? member_id_encode(template_id, (uint16_t)(member_index + 1)) : 0;

    int32_t rc = encode_variable_type_reply(size_in_memory, cip_data_type, num_members, crc, name, name_len, next_id,
                                            nesting_instance_id, resp, resp_cap, resp_len);
    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_DETAIL, 0,
               "omron_listing: variable type %u member %u ('%.*s') size=%u struct=%u.", (unsigned)template_id,
               (unsigned)member_index, (int)name_len, (const char *)name, (unsigned)size_in_memory, (unsigned)is_struct);
    }
    return rc;
}

/*
 * handle_variable_type — service 0x01 (GetAttributeAll) on class 0x6C
 * (OMRON-SPECIFIC-DESIGN.md §5.3). instance is either a real template id
 * (whole-template reply) or a synthetic member id (member_id_decode) for one
 * member of the member-sibling chain that reply's next_instance_id starts.
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

    uint32_t instance = 0;
    parse_instance(path, path_len, &instance);

    uint16_t template_id = 0, member_index = 0;
    if(member_id_decode(instance, &template_id, &member_index)) {
        return handle_variable_type_member(dev, template_id, member_index, resp, resp_cap, resp_len);
    }

    return handle_variable_type_template(dev, (uint16_t)instance, resp, resp_cap, resp_len);
}
