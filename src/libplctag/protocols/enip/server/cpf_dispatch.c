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
 * cpf_dispatch.c — server-side CPF request handlers. Split out of
 * common/cpf.c (3.a) so the direction-agnostic codec (common/cpf.c) has no
 * device_t/eip_session_t dependency and can be built unconditionally with
 * ENIP, while this file (needing device_t, cip dispatch) stays server-only.
 */

#include <stdint.h>

#include "utils/arena.h"
#include "utils/bytes.h"
#include "utils/debug.h"
#include <libplctag/protocols/enip/common/cip.h>
#include <libplctag/protocols/enip/common/cpf.h>
#include "cpf_dispatch.h"

extern Bytes cpf_handle_unconnected(Arena *a, Bytes payload, eip_session_t *sess, device_t *dev) {
    uint16_t seq = 0;
    Bytes cip_data = {0};

    pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_DETAIL, 0,
           "cpf_handle_unconnected: payload len=%zu.", payload.len);

    if(!cpf_unwrap(payload, false, NULL, &seq, &cip_data)) { return (Bytes){0}; }

    Bytes cip_response = cip_dispatch_unconnected(a, cip_data, sess, dev);
    if(bytes_is_null(cip_response)) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0, "CPF unconnected: CIP dispatch returned null.");
        return (Bytes){0};
    }

    return cpf_wrap_unconnected(a, cip_response);
}


extern Bytes cpf_handle_connected(Arena *a, Bytes payload, eip_session_t *sess, device_t *dev) {
    uint32_t conn_id = 0;
    uint16_t seq = 0;
    Bytes cip_data = {0};

    pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_DETAIL, 0,
           "cpf_handle_connected: payload len=%zu.", payload.len);

    if(!cpf_unwrap(payload, true, &conn_id, &seq, &cip_data)) { return (Bytes){0}; }

    if(conn_id != sess->server_connection_id) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0,
               "CPF connected: connection ID mismatch: got 0x%08x expected 0x%08x.",
               (unsigned)conn_id, (unsigned)sess->server_connection_id);
        return (Bytes){0};
    }

    sess->client_connection_seq = seq;
    sess->server_connection_seq++;

    Bytes cip_response = cip_dispatch_connected(a, cip_data, sess, dev, sess->max_cip_packet_size);
    if(bytes_is_null(cip_response)) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0, "CPF connected: CIP dispatch returned null.");
        return (Bytes){0};
    }

    return cpf_wrap_connected(a, sess->server_connection_id, sess->server_connection_seq, cip_response);
}
