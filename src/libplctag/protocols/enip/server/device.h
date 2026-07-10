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
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "platform.h"
#include "utils/atomic_utils.h"
#include "device_sim.h"   /* public POD types: device_sim_t, plc_type_t,
                             tag_type_t, identity_t, the callback typedefs */

/* ============================================================================
 * One registry entry — singly-linked, built before start, read-only after.
 * ============================================================================ */

typedef struct cip_obj_entry_s {
    struct cip_obj_entry_s *next;
    uint32_t            class_id;
    uint32_t            instance_id;
    device_sim_cip_cb   cb;
    void               *user_data;
} cip_obj_entry_t;

/* ============================================================================
 * tag_def_t — one per configured tag
 * ============================================================================ */

typedef struct tag_def_s {
    struct tag_def_s  *next_tag;
    char              *name;
    tag_type_t         tag_type;
    size_t             elem_size;
    size_t             elem_count;
    size_t             data_file_num;   /* PCCC only; 0 for CIP */
    size_t             num_dimensions;
    size_t             dimensions[3];
    uint8_t           *data;
    mutex_p            data_mutex;
    device_sim_tag_cb  read_cb;
    device_sim_tag_cb  write_cb;
    void              *user_data;
} tag_def_t;

/* ============================================================================
 * eip_session_t — per-connection EIP/CIP state (lives on the thread stack)
 * ============================================================================ */

typedef struct {
    uint32_t session_handle;
    uint64_t sender_context;

    uint32_t server_connection_id;
    uint16_t server_connection_seq;
    uint32_t client_connection_id;
    uint16_t client_connection_seq;
    uint16_t client_connection_serial_number;
    uint16_t client_vendor_id;
    uint32_t client_serial_number;
    uint32_t client_to_server_rpi;
    uint32_t server_to_client_rpi;

    uint32_t client_to_server_max_packet;
    uint32_t server_to_client_max_packet;

    uint32_t raw_packet_size;
    size_t   max_eip_packet_size;
    size_t   max_cpf_packet_size;
    size_t   max_cip_packet_size;

    uint16_t pccc_seq_id;
    int32_t  reject_fo_count;

    uint32_t local_ipv4;   /* host-byte-order local address of this TCP socket; reported in TCP List Identity */
} eip_session_t;

/* ============================================================================
 * device_t — shared runtime state; read-only after start except:
 *   - terminate (atomic write by device_sim_stop)
 *   - identity  (protected by identity_mutex)
 *   - tag data  (each tag protected by its own data_mutex)
 * ============================================================================ */

typedef struct {
    plc_type_t   plc_type;
    uint16_t     port;
    const char  *bind_addr;
    uint32_t     local_ipv4;   /* host-byte-order; in List Identity replies */

    uint32_t     client_to_server_max_packet;
    uint32_t     server_to_client_max_packet;

    int32_t      response_delay_ms;

    tag_def_t   *tags;

    /* Back-pointer to the owning device_sim_t — set once at create, never changes.
     * Lets protocol handlers pass the public handle to tag callbacks without
     * knowing the full struct layout. */
    device_sim_t *sim;

    /* Generic CIP object registry — built before start, read-only after. */
    cip_obj_entry_t   *cip_objects;

    /* Connect/disconnect callbacks — set before device_sim_start, no locking needed. */
    device_sim_conn_cb connect_cb;
    void              *connect_user_data;
    device_sim_conn_cb disconnect_cb;
    void              *disconnect_user_data;

    /* Per-device shutdown flag; replaces process-global g_terminate. */
    atomic_bool  terminate;

    /* Identity object — mutable via device_sim_set_identity, mutex-protected. */
    identity_t   identity;
    mutex_p      identity_mutex;
} device_t;
