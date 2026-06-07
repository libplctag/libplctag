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

#include <libplctag/protocols/enip/enip.h>
#include <libplctag/protocols/enip/enip_txn.h>
#include <libplctag/protocols/enip/enip_eip.h>
#include <libplctag/protocols/enip/enip_cpf.h>
#include <libplctag/protocols/enip/enip_cip.h>
#include <libplctag/protocols/enip/enip_stream.h>
#include <platform.h>
#include <utils/debug.h>


int32_t enip_txn(enip_link_t *link, enip_session_t *session,
                  Arena *tx_arena, Arena *rx_arena,
                  enip_msg_mode_t mode, Bytes cip_request,
                  uint64_t *out_context, Bytes *out_cip_response) {

    if(!link || !session || !tx_arena || !rx_arena || !out_context || !out_cip_response) {
        return PLCTAG_ERR_NULL_PTR;
    }

    if(bytes_is_null(cip_request)) {
        return PLCTAG_ERR_NULL_PTR;
    }

    int32_t rc;
    Bytes cpf_frame;
    Bytes eip_frame;
    Bytes response;
    uint64_t used_context;

    /* Step 1: Wrap CIP request in CPF frame (mode-specific). */
    if(mode == ENIP_MSG_UNCONNECTED) {
        cpf_frame = enip_cpf_build_unconnected(tx_arena, cip_request);
    } else if(mode == ENIP_MSG_CONNECTED) {
        /* For connected mode, use the target connection ID and sequence number. */
        cpf_frame = enip_cpf_build_connected(tx_arena, session->cip_targ_conn_id,
                                             session->cip_seq_num, cip_request);
        session->cip_seq_num++;
    } else {
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(bytes_is_null(cpf_frame)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to build CPF frame");
        return PLCTAG_ERR_NO_MEM;
    }

    /* Step 2: Wrap CPF frame in EIP and build complete request.
     * enip_eip_build_request stamps sender_context and increments it. */
    uint16_t command = (mode == ENIP_MSG_UNCONNECTED)
                       ? ENIP_CMD_UNCONNECTED_SEND
                       : ENIP_CMD_CONNECTED_SEND;
    eip_frame = enip_eip_build_request(tx_arena, command, session->session_handle,
                                       &session->sender_context, cpf_frame);

    if(bytes_is_null(eip_frame)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to build EIP frame");
        return PLCTAG_ERR_NO_MEM;
    }

    /* Record the sender_context used (already stamped into the request and incremented). */
    used_context = session->sender_context - 1;
    *out_context = used_context;

    /* Encode stages: bare CIP request, then the complete EIP frame on the wire. */
    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "ENIP txn: CIP request (%d bytes):", (int)cip_request.len);
    pdebug_dump_bytes(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, cip_request.data, (int)cip_request.len);
    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "ENIP txn: outgoing EIP frame (%d bytes):", (int)eip_frame.len);
    pdebug_dump_bytes(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, eip_frame.data, (int)eip_frame.len);

    /* Step 3: Send request with restartable I/O (wake absorbed). */
    socket_wait_state_t io_state = {0};
    do {
        rc = socket_write_wait(link->socket, &eip_frame, 5000, &io_state);
    } while(rc == PLCTAG_STATUS_PENDING);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Transaction send failed: %d", rc);
        return rc;
    }

    /* Step 4: Receive response using stream framing (24-byte EIP header + length-driven body). */
    arena_reset(rx_arena);
    io_state = (socket_wait_state_t){0};
    do {
        rc = enip_recv_frame(link->socket, rx_arena, 5000, &io_state, &response);
    } while(rc == PLCTAG_STATUS_PENDING);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Transaction recv failed: %d", rc);
        return rc;
    }

    /* Decode stage 1: complete EIP frame (24-byte header + body) as received. */
    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "ENIP txn: raw EIP frame (%d bytes):", (int)response.len);
    pdebug_dump_bytes(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, response.data, (int)response.len);

    /* Step 5: Extract CPF payload from EIP response. */
    Bytes cpf_response = enip_eip_extract_cpf_payload(response);
    if(bytes_is_null(cpf_response)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to extract CPF from EIP response");
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* Decode stage 2: CPF frame (interface handle + timeout + item list). */
    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "ENIP txn: CPF payload (%d bytes):", (int)cpf_response.len);
    pdebug_dump_bytes(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, cpf_response.data, (int)cpf_response.len);

    /* Step 6: Extract CIP payload from CPF response (mode-specific). */
    Bytes cip_response;
    if(mode == ENIP_MSG_UNCONNECTED) {
        cip_response = enip_cpf_extract_udi_payload(cpf_response);
    } else {
        cip_response = enip_cpf_extract_cdi_payload(cpf_response);
    }

    if(bytes_is_null(cip_response)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
               "ENIP: Failed to extract CIP from CPF response (mode=%d)", (int)mode);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* Decode stage 3: bare CIP reply handed back to the caller for parsing. */
    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "ENIP txn: CIP response (%d bytes):", (int)cip_response.len);
    pdebug_dump_bytes(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, cip_response.data, (int)cip_response.len);

    /* Return the CIP response slice. The caller will parse status and data. */
    *out_cip_response = cip_response;

    return PLCTAG_STATUS_OK;
}
