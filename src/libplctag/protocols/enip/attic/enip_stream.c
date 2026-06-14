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
 * ENIP Stream Framing Implementation
 *
 * Reads EIP frames from socket using stream framing protocol:
 * - First read: 24-byte EIP header
 * - Parse length field at offset 2-3 (little-endian uint16_t)
 * - Second read: exactly that many bytes of payload
 * - Return: complete frame as Bytes (header + body concatenated)
 *
 * The stream framing approach handles partial reads gracefully via
 * socket_read_wait's restartable I/O state parameter.
 */

#include <libplctag/protocols/enip/enip_stream.h>
#include <libplctag/protocols/enip/enip_eip.h>
#include <utils/debug.h>

/* Read exactly 24-byte EIP header, parse payload length, read body.
 *
 * @param socket      - Connected TCP socket
 * @param arena       - Arena for allocating frame bytes
 * @param timeout_ms  - Timeout per read operation (not total time)
 * @param io          - Restartable I/O state (for Phase 6 async)
 * @param out_frame   - Output: complete frame (header + body)
 *
 * @return PLCTAG_STATUS_OK on success, error code on failure.
 *
 * Flow:
 * 1. Allocate and read 24-byte header
 * 2. Extract length field (offset 2-3, little-endian)
 * 3. Allocate body buffer of exact length
 * 4. Read body data
 * 5. Concatenate header + body
 * 6. Return result via out_frame
 */
int32_t enip_recv_frame(sock_p socket, Arena *arena, int timeout_ms,
                        socket_wait_state_t *io, Bytes *out_frame) {
    Bytes header, body, complete_frame;
    int32_t rc;
    uint16_t payload_length;

    /* Input validation. */
    if(!socket || !arena || !io || !out_frame) {
        return PLCTAG_ERR_NULL_PTR;
    }

    /* Step 1: Read 24-byte EIP header. */
    header = bytes_alloc(arena, ENIP_EIP_HEADER_SIZE);
    if(bytes_is_null(header)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Arena OOM for frame header");
        return PLCTAG_ERR_NO_MEM;
    }

    rc = socket_read_wait(socket, &header, timeout_ms, io);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to read frame header: %d", rc);
        return rc;
    }

    /* Step 2: Extract payload length from EIP header (offset 2-3, little-endian).
     * EIP Header layout:
     *   Offset 0-1: command
     *   Offset 2-3: length (payload size in bytes, not including this 24-byte header)
     *   Offset 4+:  rest of header
     */
    Bytes length_field = bytes_slice(header, 2, 2);
    if(bytes_is_null(length_field)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to extract length field from header");
        return PLCTAG_ERR_REMOTE_ERR;
    }

    bytes_unpack(length_field, BYTES_LE, &payload_length);

    /* Step 3: Read payload (body) of exact length. */
    body = bytes_alloc(arena, payload_length);
    if(bytes_is_null(body)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Arena OOM for frame body (%u bytes)", payload_length);
        return PLCTAG_ERR_NO_MEM;
    }

    rc = socket_read_wait(socket, &body, timeout_ms, io);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to read frame body: %d", rc);
        return rc;
    }

    /* Step 4: Concatenate header and body to create complete frame. */
    complete_frame = bytes_concat(arena, header, body);
    if(bytes_is_null(complete_frame)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to concatenate frame");
        return PLCTAG_ERR_NO_MEM;
    }

    *out_frame = complete_frame;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "ENIP: Received complete frame (%u bytes header + %u bytes body)",
           ENIP_EIP_HEADER_SIZE, payload_length);

    return PLCTAG_STATUS_OK;
}
