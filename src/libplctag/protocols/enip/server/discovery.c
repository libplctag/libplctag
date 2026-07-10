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
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

/*
 * discovery.c — EIP UDP discovery responder.
 *
 * Listens on UDP 44818 for List Identity (0x0063), List Services (0x0004),
 * and List Interfaces (0x0064) broadcasts/unicasts and replies appropriately.
 *
 * TCP port 44818 is already bound by server_listener; TCP and UDP share the
 * port number space independently, so SO_REUSEADDR lets both bind to 44818.
 */

#include <stddef.h>
#include <stdint.h>

#include <libplctag/lib/libplctag.h>
#include "platform.h"
#include "utils/arena.h"
#include "utils/bytes.h"
#include "utils/debug.h"
#include "device.h"
#include <libplctag/protocols/enip/common/identity.h>
#include "server.h"
#include "discovery.h"

#define DEBUG_MOD DEBUG_MODULE_UTILS

/* ============================================================================
 * Constants
 * ============================================================================ */

#define UDP_BUF_SIZE     ((int32_t)1500)
#define SRC_HOST_LEN     ((int32_t)64)
#define RECV_TIMEOUT_MS  ((int32_t)1000)
#define DISC_ARENA_SIZE  ((size_t)4096)

/* EIP header size: cmd(2) len(2) session(4) status(4) context(8) options(4) = 24 */
#define EIP_HDR_SIZE     ((size_t)24)

/* CPF item type for List Identity response */
#define CPF_ITEM_LIST_ID ((uint16_t)0x000C)

/* CPF item type for List Services response */
#define CPF_ITEM_SERVICES ((uint16_t)0x0100)

/* List Services: version + capability + 16-byte name */
#define SERVICES_CAPABILITY ((uint16_t)0x0120) /* TCP + UDP listening */

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

static Bytes build_eip_udp_reply(Arena *a, uint16_t cmd, uint64_t sender_ctx, Bytes cpf_body) {
    uint16_t payload_len = (uint16_t)cpf_body.len;
    Bytes hdr = bytes_pack(a, BYTES_LE,
                            cmd, payload_len,
                            (uint32_t)0, (uint32_t)0,
                            sender_ctx,
                            (uint32_t)0);
    if(bytes_is_null(hdr)) { return (Bytes){NULL, 0}; }
    return bytes_concat(a, hdr, cpf_body);
}

/* ============================================================================
 * Public CPF builders — also used by eip.c for TCP List Identity
 * ============================================================================ */

extern Bytes discovery_list_identity_cpf(Arena *a, device_t *dev, uint32_t local_ipv4) {
    identity_t id;
    mutex_lock(dev->identity_mutex);
    id = dev->identity;
    mutex_unlock(dev->identity_mutex);
    Bytes item_body = identity_encode_listid_item(a, &id, local_ipv4, dev->port);
    if(bytes_is_null(item_body)) { return (Bytes){NULL, 0}; }

    Bytes cpf = bytes_pack(a, BYTES_LE,
                            (uint16_t)1,                    /* item_count */
                            CPF_ITEM_LIST_ID,               /* item_type  */
                            (uint16_t)item_body.len);       /* item_len   */
    if(bytes_is_null(cpf)) { return (Bytes){NULL, 0}; }
    return bytes_concat(a, cpf, item_body);
}


extern Bytes discovery_list_services_cpf(Arena *a) {
    /* Service name: "Communications" padded to 16 bytes with nulls */
    static const uint8_t svc_name[16] = {
        'C','o','m','m','u','n','i','c','a','t','i','o','n','s',0,0
    };
    Bytes name_bytes = bytes_from_buf(svc_name, sizeof(svc_name));

    /* item body: version(u16LE) + capability(u16LE) + name[16] */
    Bytes fixed = bytes_pack(a, BYTES_LE, (uint16_t)1, SERVICES_CAPABILITY);
    if(bytes_is_null(fixed)) { return (Bytes){NULL, 0}; }
    Bytes item_body = bytes_concat(a, fixed, name_bytes);
    if(bytes_is_null(item_body)) { return (Bytes){NULL, 0}; }

    Bytes cpf = bytes_pack(a, BYTES_LE,
                            (uint16_t)1,
                            CPF_ITEM_SERVICES,
                            (uint16_t)item_body.len);
    if(bytes_is_null(cpf)) { return (Bytes){NULL, 0}; }
    return bytes_concat(a, cpf, item_body);
}


extern Bytes discovery_list_interfaces_cpf(Arena *a) {
    /* Empty item list — no interface-specific items */
    return bytes_pack(a, BYTES_LE, (uint16_t)0);
}

/* ============================================================================
 * Discovery thread
 * ============================================================================ */

extern THREAD_FUNC(discovery_thread) {
    discovery_ctx_t *dctx  = (discovery_ctx_t *)arg;
    device_t        *dev   = dctx->device;
    registry_t      *reg   = dctx->registry;

    sock_p  udp = NULL;
    int32_t rc;
    Arena   arena;

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_INFO, 0,
           "Discovery thread starting on UDP port %u.", (unsigned)dev->port);

    rc = socket_create(&udp);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_ERROR, 0, "Discovery: socket_create failed %d.", rc);
        THREAD_RETURN(0);
    }

    rc = socket_open_udp(udp, dev->bind_addr, dev->port, true);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_ERROR, 0,
               "Discovery: socket_open_udp port=%u failed %d.", (unsigned)dev->port, rc);
        socket_destroy(&udp);
        THREAD_RETURN(0);
    }

    registry_add(reg, udp);

    if(arena_init(&arena, DISC_ARENA_SIZE) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_ERROR, 0, "Discovery: arena_init failed.");
        registry_remove(reg, udp);
        socket_close(udp);
        socket_destroy(&udp);
        THREAD_RETURN(0);
    }

    uint8_t buf[UDP_BUF_SIZE];
    char    src_host[SRC_HOST_LEN];

    while(!atomic_get_bool(&dev->terminate)) {
        arena_reset(&arena);

        uint16_t src_port = 0;
        rc = socket_recv_from(udp, buf, UDP_BUF_SIZE,
                              src_host, SRC_HOST_LEN, &src_port, RECV_TIMEOUT_MS);

        if(rc == PLCTAG_ERR_TIMEOUT) { continue; }
        if(rc == PLCTAG_ERR_ABORT)   { break; }
        if(rc < 0) {
            pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "Discovery: recv_from error %d.", rc);
            continue;
        }

        size_t pkt_len = (size_t)rc;
        if(pkt_len < EIP_HDR_SIZE) {
            pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
                   "Discovery: packet too short (%zu bytes).", pkt_len);
            continue;
        }

        Bytes pkt = bytes_from_buf(buf, pkt_len);

        uint16_t cmd        = 0;
        uint16_t req_len    = 0;
        uint32_t session    = 0;
        uint32_t status     = 0;
        uint64_t sender_ctx = 0;
        uint32_t options    = 0;
        bytes_unpack(pkt, BYTES_LE, &cmd, &req_len, &session, &status, &sender_ctx, &options);

        pdebug(DEBUG_MOD, PLCTAG_DEBUG_DETAIL, 0,
               "Discovery: cmd=0x%04x from %s:%u.", (unsigned)cmd, src_host, (unsigned)src_port);

        Bytes cpf = {NULL, 0};
        switch(cmd) {
            case EIP_CMD_LIST_IDENTITY: {
                /* Report the egress IP that reaches this querier, so the reply's
                 * embedded socket address is one the client can connect back to. */
                uint32_t reply_ip = dev->local_ipv4;
                socket_local_ipv4_to_peer(src_host, &reply_ip);
                cpf = discovery_list_identity_cpf(&arena, dev, reply_ip);
                break;
            }
            case EIP_CMD_LIST_SERVICES:
                cpf = discovery_list_services_cpf(&arena);
                break;
            case EIP_CMD_LIST_INTERFACES:
                cpf = discovery_list_interfaces_cpf(&arena);
                break;
            default:
                pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
                       "Discovery: unknown cmd 0x%04x — ignored.", (unsigned)cmd);
                continue;
        }

        if(bytes_is_null(cpf)) {
            pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
                   "Discovery: CPF build failed for cmd=0x%04x.", (unsigned)cmd);
            continue;
        }

        Bytes resp = build_eip_udp_reply(&arena, cmd, sender_ctx, cpf);
        if(bytes_is_null(resp)) {
            pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "Discovery: arena OOM building reply.");
            continue;
        }

        rc = socket_send_to(udp, resp.data, (int32_t)resp.len, src_host, src_port);
        if(rc < 0) {
            pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
                   "Discovery: send_to %s:%u failed %d.", src_host, (unsigned)src_port, rc);
        }
    }

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_INFO, 0, "Discovery thread stopping.");

    arena_free(&arena);
    registry_remove(reg, udp);
    socket_close(udp);
    socket_destroy(&udp);

    THREAD_RETURN(0);
}
