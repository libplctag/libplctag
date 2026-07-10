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
 * CPF (Common Packet Format) Layer Implementation
 *
 * STATUS: KEEP AS-IS for Phases 1-3.  Phase 3 activates the connected path.
 *
 * Phase 2: enip_cpf_build_unconnected + enip_cpf_extract_udi_payload are used
 *          for all unconnected messaging (GetIdentity, metadata, unconnected reads).
 * Phase 3: enip_cpf_build_connected + enip_cpf_extract_cdi_payload are activated
 *          once ForwardOpen succeeds (plan §3 F).  No code changes to this file
 *          are needed; the functions exist and are correct.  The caller
 *          (enip_conn.c) must start calling the connected variants and passing
 *          conn->cip_targ_conn_id and conn->cip_conn_seq_num.
 *
 * No debug-module issues: this file uses DEBUG_MODULE_ENIP correctly.
 */

#include <libplctag/protocols/enip/client/enip.h>
#include <libplctag/protocols/enip/client/enip_cpf.h>
#include <utils/bytes.h>
#include <utils/debug.h>

/* Phase 2: internal helper — no changes needed. */
static Bytes enip_cpf_pack_header(Arena *arena, uint32_t interface_handle,
                                  uint16_t router_timeout, uint16_t item_count) {
    if(!arena) { return bytes_null(); }
    return bytes_pack(arena, BYTES_LE, interface_handle, router_timeout, item_count);
}

/* Phase 2: internal helper — no changes needed. */
static Bytes enip_cpf_pack_nai(Arena *arena) {
    if(!arena) { return bytes_null(); }
    return bytes_pack(arena, BYTES_LE, (uint16_t)ENIP_CPF_ITEM_NAI, (uint16_t)0);
}

/* Phase 2: internal helper — no changes needed. */
static Bytes enip_cpf_pack_udi_header(Arena *arena, size_t payload_len) {
    if(!arena) { return bytes_null(); }
    return bytes_pack(arena, BYTES_LE, (uint16_t)ENIP_CPF_ITEM_UDI,
                      (uint16_t)(payload_len & 0xFFFF));
}

/* Phase 2: correct as-is — no changes needed.
 * Used for all unconnected sends: GetIdentity, metadata, and unrouted reads. */
Bytes enip_cpf_build_unconnected(Arena *arena, Bytes cip_payload) {
    if(!arena || bytes_is_null(cip_payload)) { return bytes_null(); }

    Bytes cpf_hdr = enip_cpf_pack_header(arena, 0, 0, 2);
    if(bytes_is_null(cpf_hdr)) { return bytes_null(); }

    Bytes nai = enip_cpf_pack_nai(arena);
    if(bytes_is_null(nai)) { return bytes_null(); }

    Bytes udi_hdr = enip_cpf_pack_udi_header(arena, cip_payload.len);
    if(bytes_is_null(udi_hdr)) { return bytes_null(); }

    return bytes_concat(arena, cpf_hdr, nai, udi_hdr, cip_payload);
}

/* Phase 3: correct as-is — no changes needed.
 * Activated once ForwardOpen succeeds.  Caller passes conn->cip_targ_conn_id
 * (the O->T connection ID returned by the PLC) and conn->cip_conn_seq_num
 * (increment before each call).  Plan §3 E: the Connected Address Item carries
 * the O->T connection id (first field of the FO response), NOT the T->O id. */
Bytes enip_cpf_build_connected(Arena *arena, uint32_t connection_id, uint16_t seq_num,
                               Bytes cip_payload) {
    if(!arena || bytes_is_null(cip_payload)) { return bytes_null(); }

    /* item_count = 2: Connected Address Item + Connected Data Item */
    Bytes cpf_hdr = enip_cpf_pack_header(arena, 0, 0, 2);
    if(bytes_is_null(cpf_hdr)) { return bytes_null(); }

    /* CAI: type(2)=0x00A1 + length(2)=4 + conn_id(4) */
    Bytes cai = bytes_pack(arena, BYTES_LE, (uint16_t)ENIP_CPF_ITEM_CONN_ADDR,
                           (uint16_t)4, (uint32_t)connection_id);
    if(bytes_is_null(cai)) { return bytes_null(); }

    /* CDI header: type(2)=0x00B1 + length(2)=(seq_num+payload)
     * length field counts seq_num(2) + cip_payload bytes */
    uint16_t cdi_length = (uint16_t)((2 + cip_payload.len) & 0xFFFF);
    Bytes cdi_hdr = bytes_pack(arena, BYTES_LE, (uint16_t)ENIP_CPF_ITEM_CONN_DATA, cdi_length);
    if(bytes_is_null(cdi_hdr)) { return bytes_null(); }

    Bytes seq = bytes_pack(arena, BYTES_LE, (uint16_t)seq_num);
    if(bytes_is_null(seq)) { return bytes_null(); }

    return bytes_concat(arena, cpf_hdr, cai, cdi_hdr, seq, cip_payload);
}

/* Phase 2: correct as-is — no changes needed. */
int32_t enip_cpf_parse_header(Bytes cpf_frame, enip_cpf_header_t *out_header) {
    if(bytes_is_null(cpf_frame) || cpf_frame.len < 8 || !out_header) {
        return PLCTAG_ERR_NULL_PTR;
    }

    Bytes rest = bytes_unpack(cpf_frame, BYTES_LE, &out_header->interface_handle,
                              &out_header->router_timeout, &out_header->item_count);

    if(bytes_is_null(rest)) { return PLCTAG_ERR_BAD_DATA; }

    return PLCTAG_STATUS_OK;
}

/* Phase 2: internal helper — no changes needed. */
static Bytes enip_cpf_find_item(Bytes cpf_frame, uint16_t target_type) {
    if(bytes_is_null(cpf_frame) || cpf_frame.len < 8) { return bytes_null(); }

    enip_cpf_header_t hdr = {0};
    if(enip_cpf_parse_header(cpf_frame, &hdr) != PLCTAG_STATUS_OK) { return bytes_null(); }

    Bytes items = bytes_slice(cpf_frame, 8, cpf_frame.len - 8);

    for(uint16_t i = 0; i < hdr.item_count && !bytes_is_null(items); i++) {
        uint16_t item_type = 0;
        uint16_t item_length = 0;

        Bytes rest = bytes_unpack(items, BYTES_LE, &item_type, &item_length);
        if(bytes_is_null(rest)) { return bytes_null(); }

        if(item_type == target_type) {
            if(rest.len >= item_length) { return bytes_slice(rest, 0, item_length); }
            return bytes_null();
        }

        if(rest.len >= item_length) {
            items = bytes_slice(rest, item_length, rest.len - item_length);
        } else {
            return bytes_null();
        }
    }

    return bytes_null();
}

/* Phase 2: correct as-is — no changes needed. */
Bytes enip_cpf_extract_udi_payload(Bytes cpf_frame) {
    return enip_cpf_find_item(cpf_frame, ENIP_CPF_ITEM_UDI);
}

/* Phase 3: correct as-is — no changes needed.
 * Returns the CDI payload with the 2-byte sequence number already stripped. */
Bytes enip_cpf_extract_cdi_payload(Bytes cpf_frame) {
    Bytes cdi = enip_cpf_find_item(cpf_frame, ENIP_CPF_ITEM_CONN_DATA);

    /* CDI payload begins with a 2-byte sequence number; skip it */
    if(bytes_is_null(cdi) || cdi.len < 2) { return bytes_null(); }

    return bytes_slice(cdi, 2, cdi.len - 2);
}
