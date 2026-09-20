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

#ifndef __PLCTAG_OMRON_CONN_H__
#    define __PLCTAG_OMRON_CONN_H__ 1

#    include <stdbool.h>

#    include <libplctag/protocols/cip/conn.h>
#    include <libplctag/protocols/omron/defs.h>
#    include <libplctag/protocols/omron/omron_common.h>
#    include <utils/atomic_utils.h>
#    include <utils/attr.h>
#    include <utils/mutex.h>
#    include <utils/nap.h>
#    include <utils/socket_fd.h>
#    include <utils/spinlock.h>
#    include <utils/thread.h>
#    include <utils/vector.h>

/* #define MAX_CONN_HOST    (128) */

#    define CONN_DEFAULT_TIMEOUT (2000)

#    define MAX_PACKET_SIZE_EX (44 + 4002)

#    define CONN_MIN_REQUESTS (10)
#    define CONN_INC_REQUESTS (10)


/*
 * Longest gateway string we will copy into a conn, NUL included.
 *
 * The attribute is "host[:port]" and is stored whole -- conn_open_socket() splits it at
 * connect time rather than at create time.  A DNS name is at most 253 characters in dotted
 * form (the familiar 255 is the wire encoding, which adds a length byte per label and a
 * terminating zero), so 253 + ":65535" + NUL is 260.  Rounded up to keep the following
 * fields aligned.
 */
#    define MAX_CONN_HOST_LEN (264)
#    define OMRON_CONN_EVENT_RING_SIZE (64)
#    define OMRON_CONN_EVENT_RING_MASK (OMRON_CONN_EVENT_RING_SIZE - 1)


struct omron_conn_t {
    CIP_CONN_STRUCT;

    /* Omron specific from here on. */

    plc_type_t plc_type;

    /* ring buffer of connection events for this connection's tags. */
    tag_conn_event_t conn_event_ring[OMRON_CONN_EVENT_RING_SIZE];
    atomic_int32_t conn_event_ring_write_idx;
};






uint64_t conn_get_new_seq_id(omron_conn_p sess);

extern int conn_startup(void);
extern void conn_teardown(void);

extern int conn_find_or_create(omron_conn_p *conn, attr attribs, int *is_new_conn);
extern int conn_get_max_payload(omron_conn_p conn);
extern int conn_get_available_cip_payload_space(omron_conn_p conn);
extern int conn_create_request(omron_conn_p conn, int tag_id, omron_request_p *request);
extern int conn_add_request(omron_conn_p sess, omron_request_p req);

#endif
