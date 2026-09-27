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

#pragma once

#include <stdbool.h>

#include <libplctag/modules/cip/conn.h>
#include <libplctag/modules/ab/ab_common.h>
#include <libplctag/modules/ab/defs.h>
#include <utils/atomic_utils.h>
#include <utils/rc.h>
#include <utils/vector.h>

/* #define MAX_SESSION_HOST    (128) */

#define SESSION_DISCONNECT_TIMEOUT (AB_EIP_CONN_TIMEOUT_MS - 1000)

#define MAX_PACKET_SIZE_EX (44 + 4002)

#define SESSION_MIN_REQUESTS (10)
#define SESSION_INC_REQUESTS (10)

#define MAX_CONN_PATH (260) /* 256 plus padding. */
#define MAX_IP_ADDR_SEG_LEN (16)

/*
 * Longest gateway string we will copy into a session, NUL included.
 *
 * The attribute is "host[:port]" and is stored whole -- session_open_socket() splits it at
 * connect time rather than at create time.  A DNS name is at most 253 characters in dotted
 * form (the familiar 255 is the wire encoding, which adds a length byte per label and a
 * terminating zero), so 253 + ":65535" + NUL is 260.  Rounded up to keep the following
 * fields aligned.
 */
#define MAX_SESSION_HOST_LEN (264)





uint64_t session_get_new_seq_id_unsafe(ab_session_p sess);
uint64_t session_get_new_seq_id(ab_session_p sess);

extern int session_startup(void);
extern void session_teardown(void);

extern int session_find_or_create(ab_session_p *session, attr attribs, int *is_new_session);
extern int session_get_available_cip_payload_space(ab_session_p session);
extern int session_create_request(ab_session_p session, int tag_id, ab_request_p *request);
extern int session_add_request_unsafe(ab_session_p sess, ab_request_p req);
extern int session_add_request(ab_session_p sess, ab_request_p req);
