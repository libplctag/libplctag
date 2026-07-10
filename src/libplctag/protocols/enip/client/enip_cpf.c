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

#include <libplctag/protocols/enip/client/enip_cpf.h>

#define ENIP_CPF_HEADER_SIZE ((size_t)8) /* iface_handle(4) + timeout(2) + item_count(2) */

Bytes enip_cpf_wrap_unconnected(Arena *a, Bytes cip) {
    if(!a || bytes_is_null(cip)) { return bytes_null(); }

    /* header: iface_handle(4)=0, timeout(2)=0, item_count(2)=2 */
    /* Null Address Item: type(2)=0x0000, length(2)=0 */
    /* Unconnected Data Item header: type(2)=0x00B2, length(2)=cip.len */
    return bytes_pack(a, BYTES_LE, (uint32_t)0, (uint16_t)0, (uint16_t)2, CPF_NULL_ADDR, (uint16_t)0, CPF_UCONN_DATA,
                       (uint16_t)cip.len, cip);
}

Bytes enip_cpf_wrap_connected(Arena *a, uint32_t conn_id, uint16_t seq, Bytes cip) {
    if(!a || bytes_is_null(cip)) { return bytes_null(); }

    /* header: iface_handle(4)=0, timeout(2)=0, item_count(2)=2 */
    /* Connected Address Item: type(2)=0x00A1, length(2)=4, conn_id(4) */
    /* Connected Data Item header: type(2)=0x00B1, length(2)=(seq(2)+cip.len) */
    uint16_t cdi_len = (uint16_t)(2 + cip.len);

    return bytes_pack(a, BYTES_LE, (uint32_t)0, (uint16_t)0, (uint16_t)2, CPF_CONN_ADDR, (uint16_t)4, conn_id, CPF_CONN_DATA,
                       cdi_len, seq, cip);
}

bool enip_cpf_unwrap(Bytes in, bool connected, uint16_t *seq_out, Bytes *cip_out) {
    if(bytes_is_null(in) || in.len < ENIP_CPF_HEADER_SIZE || !seq_out || !cip_out) { return false; }

    uint32_t iface_handle = 0;
    uint16_t router_timeout = 0;
    uint16_t item_count = 0;

    Bytes items = bytes_unpack(in, BYTES_LE, &iface_handle, &router_timeout, &item_count);
    if(bytes_is_null(items)) { return false; }

    uint16_t target_type = connected ? CPF_CONN_DATA : CPF_UCONN_DATA;

    for(uint16_t i = 0; i < item_count; i++) {
        uint16_t item_type = 0;
        uint16_t item_len = 0;

        Bytes rest = bytes_unpack(items, BYTES_LE, &item_type, &item_len);
        if(bytes_is_null(rest) || rest.len < item_len) { return false; }

        if(item_type == target_type) {
            Bytes data = bytes_slice(rest, 0, item_len);

            if(connected) {
                if(data.len < 2) { return false; }
                Bytes payload = bytes_unpack(data, BYTES_LE, seq_out);
                if(bytes_is_null(payload)) { return false; }
                *cip_out = payload;
            } else {
                *seq_out = 0;
                *cip_out = data;
            }

            return true;
        }

        items = bytes_slice(rest, item_len, rest.len - item_len);
    }

    return false;
}
