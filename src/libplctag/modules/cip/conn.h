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
 * The EtherNet/IP/CIP connection state that every CIP dialect shares.
 *
 * CIP_CONN_BASE_STRUCT follows the TAG_BASE_STRUCT idiom in lib/tag.h and the
 * CIP_TAG_BASE_STRUCT idiom in modules/cip/tag.h: a macro expanded into each
 * dialect's connection struct rather than a nested struct, so field accesses
 * stay direct.
 *
 * NOTE: expanding this into two dialect structs makes their common prefix
 * identical by construction.  That is deliberate, but nothing may cast one
 * dialect's connection to another's -- the fields past this macro differ, and
 * so does the meaning of plc_type, which each dialect declares itself with its
 * own enum.
 */

#include <libplctag/lib/conn_watch.h>
#include <platform.h>
#include <stdbool.h>
#include <stdint.h>
#include <utils/atomic_utils.h>
#include <utils/vector.h>


#define CIP_CONN_BASE_STRUCT                                                                        \
    int on_list;                                                                                    \
                                                                                                    \
    /* gateway connection related info */                                                           \
    char *host;                                                                                     \
    int port;                                                                                       \
    char *path;                                                                                     \
    sock_p sock;                                                                                    \
                                                                                                    \
    /* connection variables. */                                                                     \
    bool use_connected_msg;                                                                         \
    bool only_use_old_forward_open;                                                                 \
    int fo_conn_size;    /* old FO max connection size */                                           \
    int fo_ex_conn_size; /* extended FO max connection size */                                      \
    uint16_t max_payload_guess;                                                                     \
    uint16_t max_payload_size;                                                                      \
                                                                                                    \
    uint32_t orig_connection_id;                                                                    \
    uint32_t targ_connection_id;                                                                    \
    uint16_t conn_seq_num;                                                                          \
    uint16_t conn_serial_number;                                                                    \
                                                                                                    \
    uint8_t *conn_path;                                                                             \
    uint8_t conn_path_size;                                                                         \
    uint16_t dhp_dest;                                                                              \
    int is_dhp;                                                                                     \
                                                                                                    \
    int connection_group_id;                                                                        \
                                                                                                    \
    /* EIP session handle, from RegisterSession */                                                  \
    uint32_t session_handle;                                                                        \
                                                                                                    \
    /* sequence ID for requests */                                                                  \
    uint64_t session_seq_id;                                                                        \
                                                                                                    \
    /* list of outstanding requests for this connection */                                          \
    vector_p requests;                                                                              \
                                                                                                    \
    uint64_t resp_seq_id;                                                                           \
                                                                                                    \
    /*                                                                                              \
     * What we last put on the wire.  The response has to be an answer to the request we            \
     * actually sent, so these are snapshotted from the outgoing packet in send_eip_request()       \
     * and checked against the incoming one in recv_eip_response().                                 \
     */                                                                                             \
    uint16_t req_encap_command;                                                                     \
    uint64_t req_seq_id;                                                                            \
    bool req_sent;                                                                                  \
                                                                                                    \
    /* data for receiving messages */                                                               \
    uint32_t data_offset;                                                                           \
    uint32_t data_capacity;                                                                         \
    uint32_t data_size;                                                                             \
    uint8_t *data;                                                                                  \
    bool data_buffer_is_static;                                                                     \
                                                                                                    \
    uint64_t packet_count;                                                                          \
                                                                                                    \
    thread_p handler_thread;                                                                        \
    atomic_int32_t terminating;                                                                     \
    mutex_p session_mutex;                                                                          \
    cond_p session_wait_cond;                                                                       \
                                                                                                    \
    /* connection status and event ring - what connection tags observe */                           \
    conn_watch_t watch;                                                                             \
                                                                                                    \
    /* connection inactivity timeout - readable/writable by tags via atomics */                     \
    atomic_int32_t connection_inactivity_timeout_ms /* milliseconds */
