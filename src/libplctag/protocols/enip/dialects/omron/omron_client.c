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

/*
 * omron_client.c — OMRON NJ/NX client dialect (3.d, moved out of
 * enip_session.c). build/apply are enip_logix_build/enip_logix_apply
 * (dialects/rockwell/logix_client.c) unchanged -- identical path encoding
 * and CIP Common Format reply framing (OMRON-SPECIFIC-DESIGN.md §1); only
 * the @tags/@udt listing pair and the Forward Open size differ.
 */

#include <libplctag/protocols/enip/client/enip_cip.h>
#include <libplctag/protocols/enip/client/enip_dialect.h>
#include <libplctag/protocols/enip/dialects/rockwell/logix_client.h>

/* OMRON (OMRON-SPECIFIC-DESIGN.md §5): @tags/@udt request for t's current op.
 * @tags walks the Tag Name Server (class 0x6A, Get_Instance_List_Ex2) in
 * pages of OMRON_LIST_PAGE; @udt is a single Get_Attribute_All on the
 * Variable Type Object (class 0x6C) -- list_next_id is the
 * variable_type_instance_id, set at tag create (same list_next_id/udt_id
 * convention Rockwell uses; see enip_tag.c). Unlike Rockwell there is no
 * separate metadata/field-definition split: one class-0x6C reply carries the
 * whole definition (fragmented, if needed, via ordinary CIP status 0x06).
 * enip_dialect_t.build_listing for enip_omron_dialect. */
#define OMRON_LIST_PAGE ((uint32_t)100)
#define OMRON_LIST_KIND_USER ((uint16_t)2)

static Bytes enip_omron_build_listing(Arena *a, enip_tag_p t) {
    switch(t->op) {
        case ENIP_OP_LIST: return enip_cip_omron_list_tags(a, t->list_next_id, OMRON_LIST_PAGE, OMRON_LIST_KIND_USER);

        case ENIP_OP_UDT_META: return enip_cip_omron_udt_get_all(a, t->list_next_id);

        default: return bytes_null();
    }
}

/* Extract next_instance_id/nesting_variable_type_instance_id from one
 * complete (non-fragmented) §5.3 Variable Type Object GetAttributeAll reply,
 * so enip_omron_apply_listing can walk the member-sibling chain and recurse
 * into nested UDTs. Parses array_dimension generically and skips past its
 * number_of_elements array rather than assuming it is 0, since this runs
 * against real OMRON firmware, not just device_sim (which does always send
 * 0 today). Returns false if data is too short to hold either field. */
static bool omron_udt_reply_links(Bytes data, uint32_t *next_instance_id_out, uint32_t *nesting_instance_id_out) {
    uint8_t array_dimension = 0;
    Bytes rest = bytes_unpack(data, BYTES_LE, BYTES_SKIP(7) /* size_in_memory, reserved, cip_data_type(_array) */,
                              &array_dimension);
    if(bytes_is_null(rest)) { return false; }

    rest = bytes_unpack(rest, BYTES_LE, BYTES_SKIP((size_t)array_dimension * 4u));
    if(bytes_is_null(rest)) { return false; }

    uint8_t name_len = 0;
    rest = bytes_unpack(rest, BYTES_LE, BYTES_SKIP(2) /* num_members */, BYTES_SKIP(4) /* reserved */,
                        BYTES_SKIP(2) /* crc */, &name_len);
    if(bytes_is_null(rest)) { return false; }

    bool pad = (name_len % 2u) == 0u;
    rest = bytes_unpack(rest, BYTES_LE, BYTES_SKIP((size_t)name_len + (pad ? 1u : 0u)));
    if(bytes_is_null(rest)) { return false; }

    return !bytes_is_null(bytes_unpack(rest, BYTES_LE, next_instance_id_out, nesting_instance_id_out));
}

/* Push id onto t's pending class-0x6C instance queue (dropped silently if
 * full or 0 -- 0 means "no more", not a real instance id). */
static void omron_udt_walk_push(enip_tag_p t, uint32_t id) {
    if(id == 0 || t->udt_walk_pending_count >= (sizeof(t->udt_walk_pending) / sizeof(t->udt_walk_pending[0]))) { return; }
    t->udt_walk_pending[t->udt_walk_pending_count++] = id;
}

/* OMRON: accumulate one @tags/@udt reply into t->data verbatim (no synthetic
 * header -- @tags/@udt have no CBOR schema yet on any dialect, see
 * enip_tag.c's ENIP_TAG_KIND_LISTING/UDT doc comment, so raw bytes are all a
 * caller can get today; every reply visited by the @udt member-sibling walk
 * below is appended in visitation order). @tags advances list_next_id by the
 * reply's own instance_count and continues while its status byte is nonzero
 * (§5.1). @udt continues on ordinary CIP_STATUS_FRAG while one instance's
 * reply is still being assembled; once complete, next_instance_id/
 * nesting_variable_type_instance_id (§5.3) are queued and the walk continues
 * until the pending queue (udt_walk_pending) empties. Caller holds
 * t->api_mutex. enip_dialect_t.apply_listing for enip_omron_dialect. */
static int32_t enip_omron_apply_listing(enip_tag_p t, uint8_t cip_status, Bytes data, bool *more) {
    *more = false;

    if(t->op == ENIP_OP_LIST) {
        if(data.len < 4) { return (data.len == 0) ? PLCTAG_STATUS_OK : PLCTAG_ERR_BAD_REPLY; }

        uint16_t instance_count = 0;
        uint8_t status_byte = 0;
        bytes_unpack(data, BYTES_LE, &instance_count, &status_byte);

        if(!tag_data_append((plc_tag_p)t, &t->buf_cap, data)) { return PLCTAG_ERR_NO_MEM; }

        t->list_next_id += instance_count;
        if(status_byte != 0 && instance_count > 0) { *more = true; }

        return PLCTAG_STATUS_OK;
    }

    if(t->op == ENIP_OP_UDT_META) {
        if(data.len > 0) {
            if(!tag_data_append((plc_tag_p)t, &t->buf_cap, data)) { return PLCTAG_ERR_NO_MEM; }
        }

        if(cip_status == CIP_STATUS_FRAG) {
            *more = true; /* this instance's reply is still being assembled */
            return PLCTAG_STATUS_OK;
        }

        uint32_t next_id = 0, nesting_id = 0;
        if(omron_udt_reply_links(data, &next_id, &nesting_id)) {
            omron_udt_walk_push(t, nesting_id); /* recurse into a nested UDT member first (order is arbitrary) */
            omron_udt_walk_push(t, next_id);    /* then this template's next sibling member */
        }

        if(t->udt_walk_pending_count > 0) {
            t->list_next_id = t->udt_walk_pending[--t->udt_walk_pending_count];
            *more = true;
        }

        return PLCTAG_STATUS_OK;
    }

    return PLCTAG_ERR_UNSUPPORTED;
}

/* OMRON-SPECIFIC-DESIGN.md §6: same build/apply as Logix (§1 -- identical path
 * encoding, Read/Write Tag services, and CIP Common Format reply framing);
 * only the requested Forward Open size differs (§2.2 family default). */
/* requested_cip_size 1892: "modern Sysmac standard" per vendor documentation
 * (unverified against real hardware in this tree) -- covers the NX1/NJ
 * mainline (NX102, NX1P2, NJ501, NJ301). The flagship NX7-series (NX701) is
 * documented as negotiating up to 9600, and legacy CJ1W-EIP21/early-NJ
 * bridges as low as 1444; this dialect doesn't distinguish those from the
 * mainline (no sub-family classification -- common/plc_classify.c only
 * resolves to ENIP_PLC_OMRON_NJNX, not a specific catalog line), so 1892 is
 * the safer common denominator: Forward Open is accept/reject, not a
 * negotiate-down, so a target that can't honor 1892 rejects the connection
 * outright rather than silently granting less. A target that can't do Large
 * Forward Open at all still recovers via the 0x08 fallback (§16.4); a
 * target that supports 0x5B but rejects this specific size for some other
 * reason is a hard failure, same as an oversized standard-FO request always
 * was. */
const enip_dialect_t enip_omron_dialect = {
    .name = "omron-njnx",
    .requested_cip_size = 1892,
    .max_batch_cap = 0,
    .build = enip_logix_build,
    .apply = enip_logix_apply,
    .build_listing = enip_omron_build_listing,
    .apply_listing = enip_omron_apply_listing,
};
