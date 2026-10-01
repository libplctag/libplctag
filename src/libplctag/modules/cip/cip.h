#pragma once

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
 * Connection Manager packet encoding and decoding: Forward Open, Extended Forward Open and
 * Forward Close.
 *
 * These touch no socket.  An encoder fills conn->data and sets conn->data_size; a decoder
 * reads conn->data over conn->data_size bytes and updates the connection fields the reply
 * carries.  Sending, receiving, retrying and the connection state machine are conn.c's.
 */

#include <libplctag/modules/cip/conn.h>
#include <stdint.h>

/*
 * Encode a CIP route timeout as the priority/tick and tick-count pair the Connection
 * Manager expects.  The low nibble of the tick byte gives the tick size as 2^n
 * milliseconds and the count byte says how many ticks.  The smallest tick whose 255-count
 * maximum reaches the timeout is chosen and the count rounded up, so the encoded value is
 * never shorter than asked for.
 */
extern void cip_encode_route_timeout(int timeout_ms, uint8_t *secs_per_tick, uint8_t *timeout_ticks);

/* Build an Extended Forward Open (service 0x5B) request into conn->data. */
extern int cip_encode_forward_open_ex(cip_conn_p conn);

/* Build a Forward Open (service 0x54) request into conn->data. */
extern int cip_encode_forward_open_old(cip_conn_p conn);

/* Build a Forward Close (service 0x4E) request into conn->data. */
extern int cip_encode_forward_close(cip_conn_p conn);

/*
 * Decode a Forward Open reply.  On success the negotiated connection IDs and payload size
 * are stored on the connection.  A size rejection comes back as PLCTAG_ERR_TOO_LARGE with
 * conn->max_payload_guess lowered to what the PLC offered, so the caller can retry.
 */
extern int cip_decode_forward_open_response(cip_conn_p conn);

/* Decode a Forward Close reply. */
extern int cip_decode_forward_close_response(cip_conn_p conn);
