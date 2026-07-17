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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "platform.h"
#include "utils/atomic_utils.h"
#include "device_sim.h"   /* public POD types: device_sim_t, enip_plc_type_t,
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
 * udt_template_t — one per registered UDT/structure type (device_sim_add_udt_type)
 * ============================================================================ */

typedef struct udt_template_s {
    struct udt_template_s *next;
    char        *name;           /* owned copy of struct_name; used by role=server's udt=/elem_type=@name lookup */
    uint16_t     template_id;    /* low 12 bits of a structure tag's symbol type; class 0x6C instance id */
    uint16_t     handle;         /* structure "CRC" handle, attribute 1 */
    uint32_t     instance_size;  /* bytes, attribute 5 */
    uint16_t     num_members;    /* attribute 2 */
    uint8_t     *definition;     /* pre-encoded member-info array + NUL-delimited name blob (class 0x6C service 0x4C) */
    uint32_t     definition_len;
} udt_template_t;

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

    /* Server-tag (role=server, SERVER_TAGS.md) event delivery.  Set by the
     * listener thread in handle_read/handle_write (server/common/cip.c),
     * already under data_mutex there — no new lock. Cleared by the owning
     * plc_tag's own tickler vtable function, which takes data_mutex itself to
     * check these two plain bools before deciding to raise READ_COMPLETED /
     * WRITE_COMPLETED. Unused (always false) for device_sim_* tags, which
     * have no owning plc_tag to notify. */
    bool               pending_read_event;
    bool               pending_write_event;

    /* Fault injection (SERVER_TAGS.md sim_fault=): when nonzero, handle_read/
     * handle_write (common/cip.c) return this CIP general-status code instead
     * of touching data or the tag's read/write callbacks. Set once at tag
     * creation (eip_server_tag_create); read-only after, so no lock needed. */
    uint8_t            fault_status;
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
    enip_plc_type_t plc_type;
    uint16_t     port;
    const char  *bind_addr;
    uint32_t     local_ipv4;   /* host-byte-order; in List Identity replies */

    uint32_t     client_to_server_max_packet;
    uint32_t     server_to_client_max_packet;

    int32_t      response_delay_ms;

    /* Tag list.  device_sim_* builds this once before device_sim_start() and
     * never mutates it again (append-only via device_tags_append below).
     * Server tags (role=server) may additionally append to an already-running
     * endpoint's list, and may later remove their own entry, concurrently
     * with readers (other connections' requests) walking it — tags_mutex
     * protects every access (append, remove, and read-side walks in
     * common/cip.c, dialects/pccc/pccc.c, dialects/rockwell/ab_listing.c).
     * tags_tail makes append O(1). Removed tag_def_t entries are unlinked but
     * NOT freed until the whole endpoint is torn down (device_sim_destroy) —
     * a deliberate, bounded leak for the lifetime of the endpoint that avoids
     * needing to prove no in-flight request still holds the raw pointer a
     * lookup returned; see device_tags_remove() below. */
    tag_def_t   *tags;
    tag_def_t   *tags_tail;
    mutex_p      tags_mutex;

    /* UDT/structure template registry (device_sim_add_udt_type). Built once
     * before device_sim_start() like cip_objects below -- read-only after,
     * so no lock needed by the class 0x6C handler's lookups. */
    udt_template_t *udt_templates;
    uint16_t         next_template_id;

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

/* ============================================================================
 * tags list accessors — the single point of contact with device_t.tags for
 * every reader/writer across common/, server/, and dialects/.  All three take
 * dev->tags_mutex; there is no lock-free code anywhere in this list.
 * ============================================================================ */

/* Append at the tail (O(1) via tags_tail), under tags_mutex. Safe to call on
 * an already-running endpoint (role=server) or before device_sim_start()
 * (device_sim_*) alike. */
static inline void device_tags_append(device_t *dev, tag_def_t *tag) {
    critical_block(dev->tags_mutex) {
        tag->next_tag = NULL;
        if(dev->tags_tail) {
            dev->tags_tail->next_tag = tag;
        } else {
            dev->tags = tag;
        }
        dev->tags_tail = tag;
    }
}

/* Unlink tag from the list under tags_mutex.  Does NOT free tag — see the
 * tags field comment above for why entries are intentionally leaked between
 * removal and whole-endpoint teardown. */
static inline void device_tags_remove(device_t *dev, tag_def_t *tag) {
    critical_block(dev->tags_mutex) {
        if(dev->tags == tag) {
            dev->tags = tag->next_tag;
        } else {
            tag_def_t *p = dev->tags;
            while(p && p->next_tag != tag) { p = p->next_tag; }
            if(p) { p->next_tag = tag->next_tag; }
        }
        if(dev->tags_tail == tag) {
            tag_def_t *p = dev->tags;
            dev->tags_tail = NULL;
            while(p) { dev->tags_tail = p; p = p->next_tag; }
        }
    }
}

/* Internal-only accessor (device_sim_t itself stays opaque outside
 * device_sim.c): lets server/endpoint.c and server/eip_server_tag.c reach the
 * device_t behind a device_sim_t returned by endpoint_find_or_create()
 * without exposing the struct layout through the public device_sim.h. */
extern device_t *device_sim_get_device(device_sim_t *sim);

/* Allocate a tag_def_t owning its own data buffer + data_mutex (elem_size
 * bytes/element, elem_count elements). Used by device_sim_add_tag/
 * device_sim_add_pccc_tag and by server/eip_server_tag.c's constructor; the
 * caller fills in num_dimensions/dimensions and appends via
 * device_tags_append() above. NULL on invalid args or allocation failure. */
extern tag_def_t *device_tag_alloc(const char *name, tag_type_t type, size_t elem_size, size_t elem_count,
                                   device_sim_tag_cb read_cb, device_sim_tag_cb write_cb, void *user_data);

/* Look up a registered UDT template by id (the low 12 bits of a structure
 * tag's symbol type / the class 0x6C instance id), or by struct_name (for
 * role=server's udt=/elem_type=@name attributes -- ENIP-UPDATES-PLAN.md item
 * 2). Both take dev->tags_mutex: unlike the C API (device_sim_add_udt_type),
 * which is documented pre-start-only, role=server tags can add a UDT template
 * to an already-running endpoint, so a concurrent class 0x6C reader on
 * another connection is a real possibility, not just a documented misuse.
 * NULL if not found. */
extern udt_template_t *device_udt_find(device_t *dev, uint16_t template_id);
extern udt_template_t *device_udt_find_by_name(device_t *dev, const char *struct_name);

/* Byte size of one element of an atomic CIP/PCCC tag_type_t (0 if t is a
 * structure type or unknown). Used by dialects/omron/omron_listing.c to
 * report a UDT member's size_in_memory without duplicating the type table. */
extern size_t device_elem_size_for_type(tag_type_t t);
