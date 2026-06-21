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
 * ENIP Metadata Fetch
 *
 * Phase-1: root symbol inventory (name -> instance_id, connection-wide).
 * Phase-2: per-tag type/size/dims (lazy, stored in tag->meta, plan §3.3).
 *
 * The per-connection metadata_cache (hashtable) has been removed.  Results
 * from phase-2 fetches are now written directly into tag->meta by the caller
 * (the engine, Phase 4).  enip_metadata_fetch_tag_info returns the values
 * via out-parameters; the engine is responsible for storing them.
 *
 * Phase 7: add system-tag filtering in enip_metadata_fetch_root_symbols:
 *   skip entries where name starts with "__", name contains ":", or
 *   (symbol_type & 0x1000) is set.
 */

#include <libplctag/protocols/enip/enip.h>
#include <libplctag/protocols/enip/enip_cip.h>
#include <libplctag/protocols/enip/enip_conn.h>
#include <libplctag/protocols/enip/enip_cpf.h>
#include <libplctag/protocols/enip/enip_eip.h>
#include <libplctag/protocols/enip/enip_txn.h>
#include <inttypes.h>
#include <platform.h>
#include <utils/arena.h>
#include <utils/bytes.h>
#include <utils/debug.h>
#include <utils/enip_wait.h>
#include <utils/hash.h>
#include <utils/hashtable.h>
#include <string.h>

/* ============================================================================
 * Phase-1 root symbol cache (hash(name) -> enip_root_symbol_entry_t)
 * ============================================================================ */

void enip_root_symbol_cache_clear(enip_connection_t *conn) {
    if(!conn || !conn->root_symbol_cache) { return; }

    mutex_lock(conn->root_symbol_mutex);

    for(int32_t i = 0; i < hashtable_capacity(conn->root_symbol_cache); i++) {
        enip_root_symbol_entry_t *entry =
            (enip_root_symbol_entry_t *)hashtable_get_index(conn->root_symbol_cache, i);
        if(entry) { mem_free(entry); }
    }

    hashtable_destroy(conn->root_symbol_cache);
    conn->root_symbol_cache = NULL;

    mutex_unlock(conn->root_symbol_mutex);

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: Phase-1 root symbol cache cleared");
}

enip_root_symbol_entry_t *enip_metadata_find_root_symbol(enip_connection_t *conn,
                                                          const char *name) {
    if(!conn || !conn->root_symbol_cache || !name) { return NULL; }

    uint32_t h = hash((uint8_t *)name, strlen(name), 0);
    int64_t key = (int64_t)(uint64_t)h;

    mutex_lock(conn->root_symbol_mutex);
    enip_root_symbol_entry_t *entry =
        (enip_root_symbol_entry_t *)hashtable_get(conn->root_symbol_cache, key);
    mutex_unlock(conn->root_symbol_mutex);

    if(entry && strcmp(entry->name, name) == 0) { return entry; }
    return NULL;
}

static int32_t enip_root_symbol_store(enip_connection_t *conn, uint32_t instance_id,
                                       const char *name, uint16_t name_len) {
    if(!conn || !conn->root_symbol_cache || !name || name_len == 0) {
        return PLCTAG_ERR_NULL_PTR;
    }
    if(name_len >= (uint16_t)sizeof(((enip_root_symbol_entry_t *)0)->name)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
               "ENIP: root symbol name too long (%" PRIu16 " bytes), skipping", name_len);
        return PLCTAG_STATUS_OK;
    }

    uint32_t h = hash((uint8_t *)name, (size_t)name_len, 0);
    int64_t key = (int64_t)(uint64_t)h;

    enip_root_symbol_entry_t *entry =
        (enip_root_symbol_entry_t *)mem_alloc(sizeof(enip_root_symbol_entry_t));
    if(!entry) { return PLCTAG_ERR_NO_MEM; }

    entry->instance_id = instance_id;
    memcpy(entry->name, name, (size_t)name_len);
    entry->name[name_len] = '\0';

    mutex_lock(conn->root_symbol_mutex);
    int32_t rc = (int32_t)hashtable_put(conn->root_symbol_cache, key, entry);
    mutex_unlock(conn->root_symbol_mutex);

    if(rc != 0) {
        mem_free(entry);
        return PLCTAG_ERR_NO_MEM;
    }

    return PLCTAG_STATUS_OK;
}

/* ============================================================================
 * Phase-1 Fetch: Root Symbol Inventory
 *
 * Service 0x55 (GetInstanceAttributeList) on Symbol Class 0x6B.
 * Requests ONLY attribute 0x01 (name).
 * Wire format per entry: instance_id(4) + name_len(2) + name_bytes.
 *
 * Phase 7: add system-tag filter: skip names starting with "__" or containing ":".
 * ============================================================================ */
int32_t enip_metadata_fetch_root_symbols(enip_connection_t *conn) {
    if(!conn || !conn->link.socket || !conn->session.established) { return PLCTAG_ERR_NULL_PTR; }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0,
           "ENIP: fetching root symbol inventory (service 0x55 on class 0x6B)");

    /* The name->instance_id cache is created lazily here on each (re)connect.  A fresh
     * walk replaces any stale contents, so clear an existing table before refilling. */
    if(conn->root_symbol_cache) {
        enip_root_symbol_cache_clear(conn);
    }
    conn->root_symbol_cache = hashtable_create(64);
    if(!conn->root_symbol_cache) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_ERROR, 0, "ENIP: failed to create root symbol cache");
        return PLCTAG_ERR_NO_MEM;
    }

    uint32_t instance_id = 0;
    int32_t  total       = 0;
    int32_t  fragment    = 0;

    while(true) {
        arena_reset(&conn->tx_arena);

        /* CIP request: only attribute 0x01 (name) */
        Bytes cip = bytes_pack(&conn->tx_arena, BYTES_LE,
                               (uint8_t)CIP_SVC_GET_INSTANCE_ATTR_LIST,
                               (uint8_t)0x03,         /* path_size_words = 3 */
                               (uint8_t)0x20,
                               (uint8_t)0x6B,         /* Symbol class */
                               (uint8_t)0x25,
                               (uint8_t)0x00,
                               (uint16_t)instance_id,
                               (uint16_t)1,           /* attr_count = 1 */
                               (uint16_t)0x01);       /* name */

        if(bytes_is_null(cip)) { return PLCTAG_ERR_NO_MEM; }

        /* Use the transaction seam: it owns CPF+EIP framing, restartable send, and
         * length-driven frame reception (a fixed-size socket read would block until the
         * timeout because the reply is smaller than the buffer). */
        uint64_t used_ctx = 0;
        Bytes cip_payload;
        int32_t rc = enip_txn(&conn->link, &conn->session, &conn->tx_arena, &conn->rx_arena,
                              ENIP_MSG_UNCONNECTED, cip, &used_ctx, &cip_payload);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
                   "ENIP: root symbol transaction failed (fragment %d): %d", fragment, rc);
            return rc;
        }
        conn->messages_sent++;
        conn->messages_received++;

        uint8_t cip_status = 0;
        uint8_t ext_sz = 0;
        Bytes entries;
        Bytes parsed = enip_cip_parse_response(cip_payload, &cip_status, &ext_sz, &entries);

        if(bytes_is_null(parsed)) { return PLCTAG_ERR_REMOTE_ERR; }

        /* Status interpretation: shared code does NOT hard-code 0x06 (plan §9.1).
         * The walk continues while entries are returned; stop on empty response. */
        if(cip_status != CIP_STATUS_SUCCESS && cip_status != CIP_STATUS_PARTIAL) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
                   "ENIP: root symbol CIP status 0x%02" PRIx8 " (fragment %d)", cip_status, fragment);
            return PLCTAG_STATUS_OK;  /* non-fatal: device may not support this */
        }

        /* Each entry: instance_id(4) + name_len(2) + name_bytes */
        int32_t  count_this = 0;
        uint32_t last_id    = instance_id;

        while(entries.len >= 6) {
            uint32_t inst_id  = 0;
            uint16_t name_len = 0;

            Bytes rest = bytes_unpack(entries, BYTES_LE, &inst_id, &name_len);

            if(bytes_is_null(rest) || rest.len < (size_t)name_len) { break; }

            if(name_len > 0 && name_len < 128) {
                rc = enip_root_symbol_store(conn, inst_id,
                                            (const char *)rest.data, name_len);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
                           "ENIP: failed to store root symbol instance %" PRIu32 ": %d",
                           inst_id, rc);
                }

                pdebug(DEBUG_MODULE_ENIP, DEBUG_SPEW, 0,
                       "ENIP: symbol instance=%" PRIu32 " name='%.*s'",
                       inst_id, (int)name_len, rest.data);
            }

            last_id = inst_id;
            count_this++;
            total++;

            entries = bytes_slice(rest, (size_t)name_len, rest.len - (size_t)name_len);
        }

        pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0,
               "ENIP: fragment %d: %d entries (last_id=%" PRIu32 " status=0x%02" PRIx8 ")",
               fragment, count_this, last_id, cip_status);

        fragment++;

        if(count_this == 0 || cip_status == CIP_STATUS_SUCCESS) { break; }

        instance_id = last_id + 1u;
        if(instance_id == 0u) { break; }
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0,
           "ENIP: root symbol inventory complete: %d symbols in %d fragments",
           total, fragment);

    return PLCTAG_STATUS_OK;
}

/* ============================================================================
 * Phase-2: Per-Tag Type / Size / Dims
 *
 * Caller (engine, Phase 4) stores results in tag->meta.
 * ============================================================================ */
int32_t enip_metadata_fetch_tag_info(enip_connection_t *conn, uint32_t tag_instance_id,
                                      uint16_t *symbol_type_out, uint16_t *element_size_out,
                                      uint32_t *array_dims_out) {
    if(!conn || !conn->link.socket || !conn->session.established) { return PLCTAG_ERR_NULL_PTR; }
    if(!symbol_type_out || !element_size_out || !array_dims_out) { return PLCTAG_ERR_NULL_PTR; }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0,
           "ENIP: fetching Phase-2 metadata for instance %" PRIu32, tag_instance_id);

    arena_reset(&conn->tx_arena);

    Bytes cip = bytes_pack(&conn->tx_arena, BYTES_LE,
                           (uint8_t)CIP_SVC_GET_INSTANCE_ATTR_LIST,
                           (uint8_t)0x03,
                           (uint8_t)0x20,  (uint8_t)0x6B,
                           (uint8_t)0x25,  (uint8_t)0x00,
                           (uint16_t)tag_instance_id,
                           (uint16_t)3,
                           (uint16_t)0x02,   /* symbol_type */
                           (uint16_t)0x07,   /* element_size */
                           (uint16_t)0x08);  /* array_dims   */

    if(bytes_is_null(cip)) { return PLCTAG_ERR_NO_MEM; }

    /* Use the transaction seam (owns framing + length-driven receive). */
    uint64_t used_ctx = 0;
    Bytes cip_payload;
    int32_t rc = enip_txn(&conn->link, &conn->session, &conn->tx_arena, &conn->rx_arena,
                          ENIP_MSG_UNCONNECTED, cip, &used_ctx, &cip_payload);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
               "ENIP: Phase-2 transaction failed for instance %" PRIu32 ": %d", tag_instance_id, rc);
        return rc;
    }
    conn->messages_sent++;
    conn->messages_received++;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "ENIP: Phase-2 raw CIP reply (%d bytes):", (int)cip_payload.len);
    pdebug_dump_bytes(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, cip_payload.data, (int)cip_payload.len);

    uint8_t cip_status = 0;
    uint8_t ext_sz = 0;
    Bytes data;
    Bytes parsed = enip_cip_parse_response(cip_payload, &cip_status, &ext_sz, &data);

    /* 0x06 = "partial transfer / more data" is expected when GetInstanceAttributeList
     * (0x55) is used on a single instance; the requested instance's data is still present. */
    if(bytes_is_null(parsed)
       || (cip_status != CIP_STATUS_SUCCESS && cip_status != CIP_STATUS_PARTIAL)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
               "ENIP: Phase-2 CIP status 0x%02" PRIx8 " for instance %" PRIu32,
               cip_status, tag_instance_id);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "ENIP: Phase-2 data after CIP header (%d bytes):", (int)data.len);
    pdebug_dump_bytes(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, data.data, (int)data.len);

    /* GetInstanceAttributeList (0x55) returns a list of instance entries; we only need
     * the first (the instance we asked to start from).  Each entry is:
     *   instance_id(4) + attr2 symbol_type(2) + attr7 element_size(2) + attr8 dims(3*4=12)
     * = 20 bytes.  The leading instance_id must be skipped before the attribute values. */
    if(data.len < 20) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
               "ENIP: Phase-2 response too short (%zu bytes) for instance %" PRIu32,
               data.len, tag_instance_id);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    uint16_t sym_type = 0;
    uint16_t elem_sz  = 0;
    uint32_t dims[3]  = {0, 0, 0};

    bytes_unpack(data, BYTES_LE, BYTES_SKIP(4), &sym_type, &elem_sz, &dims[0], &dims[1], &dims[2]);

    *symbol_type_out  = sym_type;
    *element_size_out = elem_sz;
    memcpy(array_dims_out, dims, sizeof(dims));

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0,
           "ENIP: Phase-2 for instance %" PRIu32
           ": type=0x%04" PRIx16 " size=%" PRIu16
           " dims=[%" PRIu32 ",%" PRIu32 ",%" PRIu32 "]",
           tag_instance_id, sym_type, elem_sz, dims[0], dims[1], dims[2]);

    return PLCTAG_STATUS_OK;
}
