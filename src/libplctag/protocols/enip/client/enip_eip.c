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

#include <libplctag/protocols/enip/client/enip_eip.h>

Bytes enip_eip_encode(Arena *a, enip_eip_hdr_t *h, Bytes payload) {
    if(!a || !h) { return bytes_null(); }

    h->length = (uint16_t)payload.len;

    Bytes hdr = bytes_pack(a, BYTES_LE, h->command, h->length, h->session_handle, h->status, h->sender_context, h->options);
    if(bytes_is_null(hdr)) { return bytes_null(); }

    if(bytes_is_null(payload) || payload.len == 0) { return hdr; }

    return bytes_concat(a, hdr, payload);
}

bool enip_eip_decode(Bytes in, enip_eip_hdr_t *h, Bytes *payload) {
    if(bytes_is_null(in) || in.len < ENIP_EIP_HEADER_SIZE || !h || !payload) { return false; }

    Bytes rest = bytes_unpack(in, BYTES_LE, &h->command, &h->length, &h->session_handle, &h->status, &h->sender_context,
                               &h->options);
    if(bytes_is_null(rest)) { return false; }

    if(rest.len < h->length) { return false; }

    *payload = bytes_slice(rest, 0, h->length);

    return true;
}

Bytes enip_eip_register_session(Arena *a) {
    if(!a) { return bytes_null(); }

    /* payload: protocol_version(2)=1, options(2)=0 */
    Bytes payload = bytes_pack(a, BYTES_LE, (uint16_t)1, (uint16_t)0);
    if(bytes_is_null(payload)) { return bytes_null(); }

    enip_eip_hdr_t hdr = {
        .command = ENIP_CMD_REGISTER_SESSION,
        .length = 0,
        .session_handle = 0,
        .status = 0,
        .sender_context = 0,
        .options = 0,
    };

    return enip_eip_encode(a, &hdr, payload);
}

Bytes enip_eip_list_identity(Arena *a) {
    if(!a) { return bytes_null(); }

    enip_eip_hdr_t hdr = {
        .command = ENIP_CMD_LIST_IDENTITY,
        .length = 0,
        .session_handle = 0,
        .status = 0,
        .sender_context = 0,
        .options = 0,
    };

    return enip_eip_encode(a, &hdr, bytes_null());
}

Bytes enip_eip_unregister_session(Arena *a, uint32_t session_handle) {
    if(!a) { return bytes_null(); }

    enip_eip_hdr_t hdr = {
        .command = ENIP_CMD_UNREGISTER_SESSION,
        .length = 0,
        .session_handle = session_handle,
        .status = 0,
        .sender_context = 0,
        .options = 0,
    };

    return enip_eip_encode(a, &hdr, bytes_null());
}

Bytes enip_eip_send_rr_data(Arena *a, uint32_t session_handle, Bytes cpf) {
    if(!a || bytes_is_null(cpf)) { return bytes_null(); }

    enip_eip_hdr_t hdr = {
        .command = ENIP_CMD_UNCONNECTED_SEND,
        .length = 0,
        .session_handle = session_handle,
        .status = 0,
        .sender_context = 0,
        .options = 0,
    };

    return enip_eip_encode(a, &hdr, cpf);
}

Bytes enip_eip_send_unit_data(Arena *a, uint32_t session_handle, Bytes cpf) {
    if(!a || bytes_is_null(cpf)) { return bytes_null(); }

    enip_eip_hdr_t hdr = {
        .command = ENIP_CMD_CONNECTED_SEND,
        .length = 0,
        .session_handle = session_handle,
        .status = 0,
        .sender_context = 0,
        .options = 0,
    };

    return enip_eip_encode(a, &hdr, cpf);
}
