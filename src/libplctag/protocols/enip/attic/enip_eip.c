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
 * EIP (EtherNet/IP) Encapsulation Layer Implementation
 *
 * STATUS: KEEP AS-IS for all phases.  This file is correct and complete.
 *
 * Used starting in Phase 1 (RegisterSession) through all subsequent phases.
 * No functional changes are planned.  The only cosmetic fix needed (Phase 0):
 * change DEBUG_MODULE_LIB to DEBUG_MODULE_ENIP in any pdebug calls if added
 * in future (there are none today, so the file is already compliant).
 */

#include <libplctag/protocols/enip/client/enip.h>
#include <libplctag/protocols/enip/client/enip_eip.h>
#include <utils/bytes.h>

/* Phase 1: correct as-is — no changes needed.
 * Used by the stream framer in enip_conn.c to decode the 24-byte header
 * and determine how many more bytes to read (header.length). */
int32_t enip_eip_parse_header(Bytes response, enip_eip_header_t *out_header) {
    if(bytes_is_null(response) || response.len < ENIP_EIP_HEADER_SIZE || !out_header) { return PLCTAG_ERR_NULL_PTR; }

    /* Extract 24-byte EIP header in little-endian using bytes_unpack */
    Bytes rest = bytes_unpack(response, BYTES_LE, &out_header->command, &out_header->length, &out_header->session_handle,
                              &out_header->status, &out_header->sender_context, &out_header->options);

    if(bytes_is_null(rest)) { return PLCTAG_ERR_BAD_DATA; }

    return PLCTAG_STATUS_OK;
}

/* Phase 1: internal helper used by enip_eip_build_request — no changes needed. */
Bytes enip_eip_pack_header(Arena *arena, const enip_eip_header_t *header) {
    if(!arena || !header) { return bytes_null(); }

    return bytes_pack(arena, BYTES_LE, header->command, header->length, header->session_handle, header->status,
                      header->sender_context, header->options);
}

/* Phase 1: correct as-is.
 * Increments *sender_context_inout after stamping the header; callers must also
 * copy the value into tag->transaction_id (Phase 6, plan §3 N) before calling
 * this, so response correlation can match by that value. */
Bytes enip_eip_build_request(Arena *arena, uint16_t command, uint32_t session_handle,
                             uint64_t *sender_context_inout, Bytes cpf_payload) {
    if(!arena || !sender_context_inout || bytes_is_null(cpf_payload)) { return bytes_null(); }

    enip_eip_header_t hdr = {.command = command,
                             .length = (uint16_t)(cpf_payload.len & 0xFFFF),
                             .session_handle = session_handle,
                             .status = 0,
                             .sender_context = *sender_context_inout,
                             .options = 0};

    Bytes eip_hdr = enip_eip_pack_header(arena, &hdr);
    if(bytes_is_null(eip_hdr)) { return bytes_null(); }

    (*sender_context_inout)++;

    return bytes_concat(arena, eip_hdr, cpf_payload);
}

/* Phase 1: correct as-is — no changes needed.
 * Used by every receive path to strip the 24-byte EIP header before passing
 * the remainder to enip_cpf_extract_udi_payload / _extract_cdi_payload. */
Bytes enip_eip_extract_cpf_payload(Bytes response) {
    if(bytes_is_null(response) || response.len < ENIP_EIP_HEADER_SIZE) { return bytes_null(); }

    return bytes_slice(response, ENIP_EIP_HEADER_SIZE, response.len - ENIP_EIP_HEADER_SIZE);
}

