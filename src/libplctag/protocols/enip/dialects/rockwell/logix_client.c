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
 * logix_client.c — Logix/Micro800 client dialect build/apply +
 * @tags/@udt listing (3.d, moved out of enip_session.c).
 */

#include <libplctag/protocols/enip/client/enip_cip.h>
#include <libplctag/protocols/enip/client/enip_dialect.h>
#include "logix_client.h"

/* Logix/Micro800 symbolic build (enip_dialect_t.build): encode the CIP
 * sub-request for t's current data op into the caller-owned `dest` region.
 * Returns the used prefix of dest, or bytes_null() if it does not fit / on
 * error. Uses c->arena as scratch (the existing CIP encoders allocate there).
 * Caller holds t->api_mutex. */
extern Bytes enip_logix_build(enip_connection_t *c, enip_tag_p t, Bytes dest) {
    Arena *a = &c->arena;
    Bytes req = bytes_null();

    switch(t->op) {
        /* §16a.6: always ReadFrag (offset 0), not plain ReadTag -- for an
         * element that fits, the reply is identical (status 0, all the
         * data); for one that doesn't, the device signals CIP_STATUS_FRAG
         * and apply_tag_reply switches to ENIP_OP_OPEN_PROBE_FRAG instead of
         * this needing a client-side "will it fit" pre-check. */
        case ENIP_OP_OPEN_PROBE: req = enip_cip_read_frag(a, t->path, 1, 0); break;

        case ENIP_OP_OPEN_PROBE_FRAG: req = enip_cip_read_frag(a, t->path, 1, t->frag_offset); break;

        case ENIP_OP_OPEN_BULK: {
            uint32_t n = t->elem_count - t->read_off;
            if(n > t->window_elems) { n = t->window_elems; }
            Bytes path = enip_cip_encode_path_at(a, t->path, t->read_off);
            req = enip_cip_read(a, path, (uint16_t)n);
            break;
        }

        case ENIP_OP_READ:
            if(t->fragmented_elem) {
                req = enip_cip_read_frag(a, t->path, 1, t->frag_offset);
            } else if(t->elem_count <= 1) {
                req = enip_cip_read(a, t->path, (uint16_t)t->elem_count);
            } else {
                uint32_t n = t->elem_count - t->read_off;
                if(n > t->window_elems) { n = t->window_elems; }
                Bytes path = enip_cip_encode_path_at(a, t->path, t->read_off);
                req = enip_cip_read(a, path, (uint16_t)n);
            }
            break;

        case ENIP_OP_WRITE: {
            Bytes type_header = bytes_from_buf(t->type_header, t->type_header_len);
            if(t->fragmented_elem) {
                /* §16a.6: fixed-size aligned chunk, computed once at
                 * fragmentation-detection time (see apply_tag_reply's
                 * OPEN_PROBE_FRAG completion) so build and apply always
                 * agree on the chunk boundary without exchanging state. */
                size_t remaining = (size_t)t->size - (size_t)t->frag_offset;
                size_t chunk = (remaining < (size_t)t->frag_write_chunk) ? remaining : (size_t)t->frag_write_chunk;
                Bytes data = bytes_from_buf(t->data + t->frag_offset, chunk);
                req = enip_cip_write_frag(a, t->path, type_header, 1, t->frag_offset, data);
            } else if(t->elem_count <= 1) {
                Bytes data = bytes_from_buf(t->data, (size_t)t->size);
                req = enip_cip_write(a, t->path, type_header, (uint16_t)t->elem_count, data);
            } else {
                uint32_t n = write_window_count(t);
                Bytes path = enip_cip_encode_path_at(a, t->path, t->read_off);
                Bytes data = bytes_from_buf(t->data + (size_t)t->read_off * (size_t)t->elem_size, (size_t)n * (size_t)t->elem_size);
                req = enip_cip_write(a, path, type_header, (uint16_t)n, data);
            }
            break;
        }

        default: return bytes_null();
    }

    if(bytes_is_null(req)) { return bytes_null(); }

    if(bytes_is_null(bytes_pack_into(dest, BYTES_LE, req))) { return bytes_null(); }
    return bytes_from_buf(dest.data, req.len);
}

/* Logix/Micro800 apply (enip_dialect_t.apply): parse one CIP reply, treat any
 * non-zero CIP status other than CIP_STATUS_FRAG (§16a.6: fragmentation
 * continuation, not an error) as a remote error, copy into t->data via
 * apply_tag_reply, and set *more when another read/write window (or
 * fragment) is due. Caller holds api_mutex. */
extern int32_t enip_logix_apply(enip_connection_t *c, enip_tag_p t, Bytes cip_reply, bool *more) {
    *more = false;

    cip_reply_t reply;
    if(!enip_cip_parse_reply(cip_reply, &reply)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to parse CIP reply!");
        return PLCTAG_ERR_BAD_REPLY;
    }

    if(reply.status != 0 && reply.status != CIP_STATUS_FRAG) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "CIP error 0x%02X (ext 0x%04X).", reply.status, reply.ext_status);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    int32_t rc = apply_tag_reply(c, t, reply.status, reply.data);

    if(rc == PLCTAG_STATUS_OK) {
        if(t->frag_more) {
            *more = true;
        } else if((t->op == ENIP_OP_OPEN_BULK || t->op == ENIP_OP_READ || t->op == ENIP_OP_WRITE)
                  && t->read_off < t->elem_count) {
            *more = true;
        }
    }

    return rc;
}

/* Rockwell (ROCKWELL-SPECIFIC-DESIGN.md): @tags/@udt request for t's current
 * op, CIP class 0x6B (symbol) / 0x6C (template). read_off is the byte cursor
 * into the accumulated buffer; list_next_id is the symbol instance id (@tags)
 * or template id (@udt); list_total is the @udt field-definition byte target.
 * enip_dialect_t.build_listing for enip_logix_dialect. */
static Bytes enip_logix_build_listing(Arena *a, enip_tag_p t) {
    switch(t->op) {
        case ENIP_OP_LIST: return enip_cip_list_tags(a, t->path, (uint16_t)t->list_next_id);

        case ENIP_OP_UDT_META: return enip_cip_udt_meta(a, (uint16_t)t->list_next_id);

        case ENIP_OP_UDT_FIELDS: {
            uint32_t remaining = (t->list_total > t->read_off) ? (t->list_total - t->read_off) : 0;
            return enip_cip_udt_fields(a, (uint16_t)t->list_next_id, t->read_off, (uint16_t)remaining);
        }

        default: return bytes_null();
    }
}

/* Rockwell: accumulate one @tags/@udt reply into t->data and advance the
 * continuation cursor. Sets *more when another request is needed (FRAG
 * status, or the UDT metadata->fields transition). Caller holds t->api_mutex.
 * enip_dialect_t.apply_listing for enip_logix_dialect. */
static int32_t enip_logix_apply_listing(enip_tag_p t, uint8_t cip_status, Bytes data, bool *more) {
    *more = false;

    if(t->op == ENIP_OP_LIST) {
        if(data.len > 0) {
            size_t need = (size_t)t->read_off + data.len;
            uint8_t *buf = mem_realloc(t->data, (int)need);
            if(!buf) { return PLCTAG_ERR_NO_MEM; }
            t->data = buf;
            bytes_pack_into(bytes_from_buf(t->data + t->read_off, data.len), BYTES_LE, data);
            t->read_off = (uint32_t)need;
            t->size = (int32_t)need;

            /* Walk this packet's entries to find the highest instance id. Each
             * entry is a 22-byte fixed prefix (instance_id u32, symbol_type u16,
             * element_length u16, array_dims 3xu32, string_len u16) + name. */
            size_t off = 0;
            while(off + 22 <= data.len) {
                uint32_t inst = 0;
                uint16_t name_len = 0;
                bytes_unpack(bytes_from_buf(data.data + off, data.len - off), BYTES_LE, &inst, BYTES_SKIP(16), &name_len);
                t->list_next_id = inst + 1;
                off += (size_t)22 + name_len;
            }
        }

        if(cip_status == CIP_STATUS_FRAG) { *more = true; }

        return PLCTAG_STATUS_OK;
    }

    if(t->op == ENIP_OP_UDT_META) {
        /* The Get_Attribute_List reply packs count(2) then per-attribute
         * {id(2), status(2), value}. Mirror the AB driver's fixed offsets to pull
         * the four values and build a 14-byte synthetic header (udt id, field
         * definition words, instance size, member count, handle). */
        if(data.len < 30) { return PLCTAG_ERR_BAD_REPLY; }

        uint32_t desc_words = 0, inst_size = 0;
        uint16_t num_members = 0, handle = 0;
        bytes_unpack(bytes_from_buf(data.data + 6, 4), BYTES_LE, &desc_words);
        bytes_unpack(bytes_from_buf(data.data + 14, 4), BYTES_LE, &inst_size);
        bytes_unpack(bytes_from_buf(data.data + 22, 2), BYTES_LE, &num_members);
        bytes_unpack(bytes_from_buf(data.data + 28, 2), BYTES_LE, &handle);

        uint8_t *buf = mem_realloc(t->data, 14);
        if(!buf) { return PLCTAG_ERR_NO_MEM; }
        t->data = buf;
        Bytes hdr = bytes_from_buf(t->data, 14);
        bytes_pack_into(hdr, BYTES_LE, (uint16_t)t->list_next_id, (uint32_t)desc_words, (uint32_t)inst_size,
                        (uint16_t)num_members, (uint16_t)handle);
        t->size = 14;

        /* field-definition byte target (per the template docs), rounded up to 4. */
        uint32_t total = (4 * desc_words) - 23;
        t->list_total = (total + 3) & ~(uint32_t)3;
        t->read_off = 0;

        /* transition to reading the field definition bytes. */
        t->op = ENIP_OP_UDT_FIELDS;
        *more = true;

        return PLCTAG_STATUS_OK;
    }

    if(t->op == ENIP_OP_UDT_FIELDS) {
        if(data.len > 0) {
            size_t need = (size_t)14 + t->read_off + data.len;
            uint8_t *buf = mem_realloc(t->data, (int)need);
            if(!buf) { return PLCTAG_ERR_NO_MEM; }
            t->data = buf;
            bytes_pack_into(bytes_from_buf(t->data + 14 + t->read_off, data.len), BYTES_LE, data);
            t->read_off += (uint32_t)data.len;
            t->size = (int32_t)need;
        }

        if(cip_status == CIP_STATUS_FRAG) { *more = true; }

        return PLCTAG_STATUS_OK;
    }

    return PLCTAG_ERR_UNSUPPORTED;
}

/* requested_cip_size 4002: the Large Forward Open size ControlLogix/
 * CompactLogix/GuardLogix (5580/5380/5370) and Micro800 "E" (2080-L50E/L70E)
 * grant, per vendor documentation (unverified against real hardware in this
 * tree). §16.4: the connection always tries Large Forward Open (0x5B) first
 * with this size and falls back to a plain 504-byte standard Forward Open
 * (0x54) if the target rejects 0x5B with CIP 0x08 -- so a target that can't
 * actually do 4002 (older Micro800, or anything else) just costs one extra
 * round trip on first connect, not a wrong value. */
const enip_dialect_t enip_logix_dialect = {
    .name = "logix",
    .requested_cip_size = 4002,
    .max_batch_cap = 0, /* no cap: Multiple Service Packet (0x0A) supported */
    .build = enip_logix_build,
    .apply = enip_logix_apply,
    .build_listing = enip_logix_build_listing,
    .apply_listing = enip_logix_apply_listing,
};
