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
 * The connection state both CIP modules keep.
 *
 * ab_session_t and omron_conn_t held the same thirty-nine fields; a module pastes
 * this after nothing and adds its own afterwards, the same arrangement as
 * CIP_TAG_STRUCT.  What stays module specific is the DH+ routing AB needs, and
 * each module's own way of publishing connection status to its tags -- AB has a
 * status ring with a reason code, Omron an event ring.
 *
 * NOTE: plc_type is not here.  Its type is a different enum in each module.
 */

#include <libplctag/lib/libplctag.h>
#include <libplctag/protocols/cip/tag.h>
#include <stdbool.h>
#include <stdint.h>
#include <utils/atomic_utils.h>
#include <utils/mutex.h>
#include <utils/nap.h>
#include <utils/socket.h>
#include <utils/spinlock.h>
#include <utils/thread.h>
#include <utils/vector.h>


#define CIP_CONN_STRUCT                                                                          \
    int failed;                                                                                  \
    int on_list;                                                                                 \
                                                                                                 \
    /* gateway connection related info */                                                        \
    char *host;                                                                                  \
    int port;                                                                                    \
    char *path;                                                                                  \
    sock_p sock;                                                                                 \
                                                                                                 \
    /* connection variables. */                                                                  \
    bool use_connected_msg;                                                                       \
    bool only_use_old_forward_open;                                                              \
                                                                                                 \
    /* what this PLC model can do and how big its payloads may be. */                             \
    cip_plc_config_t plc_config;                                                                 \
    uint16_t max_payload_guess;                                                                  \
    uint16_t max_payload_size;                                                                   \
                                                                                                 \
    uint32_t orig_connection_id;                                                                 \
    uint32_t targ_connection_id;                                                                 \
    uint16_t conn_seq_num;                                                                       \
    uint16_t conn_serial_number;                                                                 \
                                                                                                 \
    uint8_t *conn_path;                                                                          \
    uint8_t conn_path_size;                                                                      \
                                                                                                 \
    int connection_group_id;                                                                     \
                                                                                                 \
    /* registration info */                                                                      \
    uint32_t conn_handle;                                                                        \
                                                                                                 \
    /* Sequence ID for requests; guarded by mutex, see cip_conn_get_new_seq_id(). */              \
    uint64_t conn_seq_id;                                                                        \
                                                                                                 \
    /* list of outstanding requests for this connection */                                       \
    vector_p requests;                                                                           \
                                                                                                 \
    uint64_t resp_seq_id;                                                                        \
                                                                                                 \
    /*                                                                                           \
     * What we last put on the wire.  The response has to be an answer to the request we         \
     * actually sent, so these are snapshotted from the outgoing packet in send_eip_request()    \
     * and checked against the incoming one in recv_eip_response().                              \
     */                                                                                          \
    uint16_t req_encap_command;                                                                  \
    uint64_t req_seq_id;                                                                         \
    bool req_sent;                                                                               \
                                                                                                 \
    /* data for receiving messages */                                                            \
    uint32_t data_offset;                                                                        \
    uint32_t data_capacity;                                                                      \
    uint32_t data_size;                                                                          \
    uint8_t *data;                                                                               \
    bool data_buffer_is_static;                                                                  \
                                                                                                 \
    uint64_t packet_count;                                                                       \
                                                                                                 \
    thread_p handler_thread;                                                                     \
    atomic_int32_t terminating;                                                                  \
    mutex_p mutex;                                                                               \
    nap_p nap;                                                                                   \
                                                                                                 \
    /* connection status - readable by tags via atomics */                                       \
    atomic_int32_t connection_status;                                                            \
                                                                                                 \
    /* connection inactivity timeout - readable/writable by tags via atomics */                  \
    atomic_int32_t connection_inactivity_timeout_ms


/*
 * A connection holding just the shared fields.  Both modules' structs begin with
 * this sequence, so shared code takes one of these and each module casts.
 */
struct cip_conn_t {
    CIP_CONN_STRUCT;
};

typedef struct cip_conn_t *cip_conn_p;


/*
 * The next sequence ID for a request on this connection.
 *
 * The ID is what matches a response to the request that asked for it, so zero is
 * skipped on rollover: a zero could collide with an unset sender context.
 */
extern uint64_t cip_conn_get_new_seq_id(cip_conn_p conn);
