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
#include <libplctag/modules/omron/defs.h>
#include <libplctag/modules/omron/omron_common.h>
#include <utils/atomic_utils.h>
#include <utils/rc.h>
#include <utils/vector.h>

/* #define MAX_CONN_HOST    (128) */

#define SESSION_DISCONNECT_TIMEOUT (OMRON_EIP_CONN_TIMEOUT_MS - 1000)

#define MAX_PACKET_SIZE_EX (44 + 4002)

#define SESSION_MIN_REQUESTS (10)
#define SESSION_INC_REQUESTS (10)

#define MAX_CONN_PATH (260) /* 256 plus padding. */
#define MAX_IP_ADDR_SEG_LEN (16)

/*
 * Longest gateway string we will copy into a conn, NUL included.
 *
 * The attribute is "host[:port]" and is stored whole -- session_open_socket() splits it at
 * connect time rather than at create time.  A DNS name is at most 253 characters in dotted
 * form (the familiar 255 is the wire encoding, which adds a length byte per label and a
 * terminating zero), so 253 + ":65535" + NUL is 260.  Rounded up to keep the following
 * fields aligned.
 */
#define MAX_CONN_HOST_LEN (264)






uint64_t session_get_new_seq_id_unsafe(omron_conn_p sess);
uint64_t session_get_new_seq_id(omron_conn_p sess);

extern int conn_startup(void);
extern void conn_teardown(void);

extern int conn_find_or_create(omron_conn_p *conn, attr attribs, int *is_new_conn);
extern int session_get_available_cip_payload_space(omron_conn_p conn);
extern int conn_create_request(omron_conn_p conn, int tag_id, omron_request_p *request);
extern int conn_add_request(omron_conn_p sess, omron_request_p req);
