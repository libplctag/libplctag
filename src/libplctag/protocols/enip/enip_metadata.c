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
 * ENIP Metadata Cache Management
 *
 * STATUS: MOSTLY CORRECT.  Two fixes required; otherwise keep as-is.
 *
 * Phase 7 (Metadata & identity-driven feature enable):
 *   Fix enip_metadata_fetch_root_symbols: add system-tag filtering (plan §3 L):
 *     skip entries where name starts with "__", name contains ":", or
 *     (symbol_type & 0x1000) is set.  Record (symbol_type & 0x8000) as
 *     "is_struct" in enip_root_symbol_entry_t.
 *   No other changes to this file are needed for Phases 1-6.
 *
 * Two-phase metadata strategy:
 *   Phase 1 (eager, connection startup): enip_metadata_fetch_root_symbols fetches
 *     ALL tag names and instance IDs using GetInstanceAttributeList (service 0x55)
 *     on Symbol Class 0x6B, requesting ONLY attribute 0x01 (name).
 *     Wire format per entry: instance_id(4) + string_len(2) + name_bytes.
 *     Results stored in root_symbol_cache keyed by hash(name).
 *
 *   Phase 2 (lazy, first use): enip_metadata_fetch_tag_info fetches symbol_type,
 *     element_size, and array_dims for a specific instance_id using
 *     GetInstanceAttributeList on that instance, requesting attributes 0x02, 0x07, 0x08.
 *     Results cached in metadata_cache keyed by instance_id.
 *
 * Phase 1 fix required in enip_metadata_fetch_root_symbols:
 *   Change the CIP request to ask for only attribute 0x01 (attr_count=1).
 *   Change the wire parse: remove unpack of sym_type, elem_sz, dims;
 *   minimum entry size drops from 22 to 6 bytes (instance_id(4) + name_len(2)).
 *   Remove sym_type/elem_sz/dims from enip_root_symbol_store call/signature.
 *   Upgrade instance_id field from uint16_t to uint32_t (Logix v20+ returns 32-bit IDs).
 *
 * Debug module: this file correctly uses DEBUG_MODULE_ENIP throughout.
 */

#include <libplctag/protocols/enip/enip.h>
#include <libplctag/protocols/enip/enip_cip.h>
#include <libplctag/protocols/enip/enip_conn.h>
#include <libplctag/protocols/enip/enip_cpf.h>
#include <libplctag/protocols/enip/enip_eip.h>
#include <platform.h>
#include <utils/arena.h>
#include <utils/bytes.h>
#include <utils/debug.h>
#include <utils/enip_wait.h>
#include <utils/hash.h>
#include <utils/hashtable.h>
#include <string.h>

/* ============================================================================
 * Lazy metadata cache (instance_id -> symbol_type / element_size / array_dims)
 * Populated by enip_metadata_fetch_tag_info on first use of each tag.
 * ============================================================================ */

/* Phase 1: correct as-is — no changes needed. */
static int32_t enip_metadata_get_cached(enip_connection_t *conn, uint32_t tag_instance_id,
                                         uint16_t *symbol_type_out, uint16_t *element_size_out,
                                         uint32_t *array_dims_out) {
    if(!conn || !conn->metadata_cache) { return PLCTAG_ERR_NULL_PTR; }

    int64_t key = (int64_t)tag_instance_id;

    mutex_lock(conn->metadata_cache_mutex);
    enip_metadata_cache_entry_t *entry =
        (enip_metadata_cache_entry_t *)hashtable_get(conn->metadata_cache, key);

    if(entry) {
        *symbol_type_out  = entry->symbol_type;
        *element_size_out = entry->element_size;
        memcpy(array_dims_out, entry->array_dims, sizeof(entry->array_dims));
        mutex_unlock(conn->metadata_cache_mutex);
        pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0,
               "ENIP: metadata cache hit for instance %u", tag_instance_id);
        return PLCTAG_STATUS_OK;
    }

    mutex_unlock(conn->metadata_cache_mutex);
    return PLCTAG_ERR_NOT_FOUND;
}

/* Phase 1: correct as-is — no changes needed. */
static int32_t enip_metadata_set_cached(enip_connection_t *conn, uint32_t tag_instance_id,
                                         uint16_t symbol_type, uint16_t element_size,
                                         const uint32_t *array_dims) {
    if(!conn || !conn->metadata_cache || !array_dims) { return PLCTAG_ERR_NULL_PTR; }

    int64_t key = (int64_t)tag_instance_id;

    enip_metadata_cache_entry_t *entry =
        (enip_metadata_cache_entry_t *)mem_alloc(sizeof(enip_metadata_cache_entry_t));
    if(!entry) { return PLCTAG_ERR_NO_MEM; }

    entry->symbol_type  = symbol_type;
    entry->element_size = element_size;
    memcpy(entry->array_dims, array_dims, sizeof(entry->array_dims));

    mutex_lock(conn->metadata_cache_mutex);
    int32_t rc = (int32_t)hashtable_put(conn->metadata_cache, key, entry);
    mutex_unlock(conn->metadata_cache_mutex);

    if(rc != 0) {
        mem_free(entry);
        return PLCTAG_ERR_NO_MEM;
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0,
           "ENIP: metadata cached for instance %u (type=0x%04x size=%u)",
           tag_instance_id, symbol_type, element_size);
    return PLCTAG_STATUS_OK;
}

/* Phase 6: call this on every disconnect/reconnect to force re-fetch on next connect. */
void enip_metadata_cache_clear(enip_connection_t *conn) {
    if(!conn || !conn->metadata_cache) { return; }

    mutex_lock(conn->metadata_cache_mutex);

    for(int32_t i = 0; i < hashtable_capacity(conn->metadata_cache); i++) {
        enip_metadata_cache_entry_t *entry =
            (enip_metadata_cache_entry_t *)hashtable_get_index(conn->metadata_cache, i);
        if(entry) { mem_free(entry); }
    }

    hashtable_destroy(conn->metadata_cache);
    conn->metadata_cache = NULL;

    mutex_unlock(conn->metadata_cache_mutex);

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: Phase-2 metadata cache cleared");
}

/* ============================================================================
 * Phase-1 root symbol cache (hash(name) -> enip_root_symbol_entry_t)
 * ============================================================================ */

/* Phase 6: call this alongside enip_metadata_cache_clear on disconnect. */
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

/* Phase 7: correct as-is — no changes needed.
 * Called by the engine (Phase 6) to resolve tag->tag_name -> tag->tag_instance_id
 * after the root symbol inventory completes. */
enip_root_symbol_entry_t *enip_metadata_find_root_symbol(enip_connection_t *conn,
                                                          const char *name) {
    if(!conn || !conn->root_symbol_cache || !name) { return NULL; }

    uint32_t h = hash((uint8_t *)name, strlen(name), 0);
    int64_t key = (int64_t)(uint64_t)h;

    mutex_lock(conn->root_symbol_mutex);
    enip_root_symbol_entry_t *entry =
        (enip_root_symbol_entry_t *)hashtable_get(conn->root_symbol_cache, key);
    mutex_unlock(conn->root_symbol_mutex);

    /* Check name to guard against hash collisions */
    if(entry && strcmp(entry->name, name) == 0) { return entry; }
    return NULL;
}

/* Phase 1: SIMPLIFY signature — remove symbol_type, element_size, array_dims parameters.
 * Phase 1: Remove the lines that set entry->symbol_type, entry->element_size, entry->array_dims.
 * New signature: enip_root_symbol_store(conn, instance_id, name, name_len).
 * Phase 7: add system-tag filter in the CALLER (fetch_root_symbols) before this call. */
static int32_t enip_root_symbol_store(enip_connection_t *conn, uint32_t instance_id,
                                       uint16_t symbol_type, uint16_t element_size,
                                       const uint32_t *array_dims,
                                       const char *name, uint16_t name_len) {
    if(!conn || !conn->root_symbol_cache || !name || name_len == 0) {
        return PLCTAG_ERR_NULL_PTR;
    }
    if(name_len >= (uint16_t)sizeof(((enip_root_symbol_entry_t *)0)->name)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
               "ENIP: root symbol name too long (%u bytes), skipping", (uint32_t)name_len);
        return PLCTAG_STATUS_OK;
    }

    uint32_t h = hash((uint8_t *)name, (size_t)name_len, 0);
    int64_t key = (int64_t)(uint64_t)h;

    enip_root_symbol_entry_t *entry =
        (enip_root_symbol_entry_t *)mem_alloc(sizeof(enip_root_symbol_entry_t));
    if(!entry) { return PLCTAG_ERR_NO_MEM; }

    entry->instance_id  = instance_id;
    entry->symbol_type  = symbol_type;
    entry->element_size = element_size;
    memcpy(entry->array_dims, array_dims, sizeof(entry->array_dims));
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
 *
 * Phase 1: change to request ONLY attribute 0x01 (name).  Set attr_count=1.
 * Wire format per entry then becomes: instance_id(4) + name_len(2) + name_bytes.
 * Minimum entry size = 6 bytes (not 22).  Remove unpack of sym_type/elem_sz/dims.
 * Remove those arguments from the enip_root_symbol_store call.
 * Upgrade local instance_id variable from uint16_t to uint32_t.
 *
 * Fragmentation: start at instance_id 0, record last_instance_id from each
 * response, repeat from last_instance_id+1 until zero entries returned.
 * CIP status 0x06 (partial) = more packets follow.  0x00 = last packet.
 * ============================================================================ */

/* Phase 1 REWRITE: change attr_count from 4 to 1, request only 0x01 (name).
 * Phase 1: upgrade instance_id variable to uint32_t.
 * Phase 1: change while(entries.len >= 22) to while(entries.len >= 6).
 * Phase 1: remove sym_type/elem_sz/dims from bytes_unpack call and store call.
 * Phase 7: add system-tag filter after name extraction:
 *   if(name_len >= 2 && name[0]=='_' && name[1]=='_') continue;
 *   if(memchr(name, ':', name_len)) continue;
 */
int32_t enip_metadata_fetch_root_symbols(enip_connection_t *conn) {
    if(!conn || !conn->socket || !conn->session_established) { return PLCTAG_ERR_NULL_PTR; }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0,
           "ENIP: fetching root symbol inventory (service 0x55 on class 0x6B)");

    uint16_t instance_id  = 0;
    int32_t  total        = 0;
    int32_t  fragment     = 0;

    while(true) {
        arena_reset(&conn->tx_arena);

        /* CIP request: service(1) + path_size_words(1) + path(6) + attr_count(2) + attrs(8) */
        Bytes cip = bytes_pack(&conn->tx_arena, BYTES_LE,
                               (uint8_t)CIP_SVC_GET_INSTANCE_ATTR_LIST,
                               (uint8_t)0x03,        /* path_size_words = 3 words = 6 bytes */
                               (uint8_t)0x20,        /* class segment tag */
                               (uint8_t)0x6B,        /* Symbol class */
                               (uint8_t)0x25,        /* 16-bit instance segment tag */
                               (uint8_t)0x00,        /* pad */
                               (uint16_t)instance_id,
                               (uint16_t)4,          /* attribute count */
                               (uint16_t)0x02,       /* symbol_type */
                               (uint16_t)0x07,       /* element_size */
                               (uint16_t)0x08,       /* array_dims */
                               (uint16_t)0x01);      /* name */

        if(bytes_is_null(cip)) { return PLCTAG_ERR_NO_MEM; }

        Bytes cpf = enip_cpf_build_unconnected(&conn->tx_arena, cip);
        if(bytes_is_null(cpf)) { return PLCTAG_ERR_NO_MEM; }

        Bytes frame = enip_eip_build_request(&conn->tx_arena, ENIP_CMD_UNCONNECTED_SEND,
                                             conn->session_handle, &conn->sender_context, cpf);
        if(bytes_is_null(frame)) { return PLCTAG_ERR_NO_MEM; }

        socket_wait_state_t io_state = {0};
        int32_t rc = socket_write_wait(conn->socket, &frame, 5000, &io_state);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
                   "ENIP: root symbol send failed (fragment %d): %d", fragment, rc);
            return rc;
        }
        conn->messages_sent++;

        arena_reset(&conn->rx_arena);
        Bytes response = bytes_alloc(&conn->rx_arena, 4096);
        if(bytes_is_null(response)) { return PLCTAG_ERR_NO_MEM; }

        rc = socket_read_wait(conn->socket, &response, 5000, &io_state);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
                   "ENIP: root symbol read failed (fragment %d): %d", fragment, rc);
            return rc;
        }
        conn->messages_received++;

        Bytes cpf_payload = enip_eip_extract_cpf_payload(response);
        if(bytes_is_null(cpf_payload)) { return PLCTAG_ERR_REMOTE_ERR; }

        Bytes cip_payload = enip_cpf_extract_udi_payload(cpf_payload);
        if(bytes_is_null(cip_payload)) { return PLCTAG_ERR_REMOTE_ERR; }

        uint8_t cip_status = 0;
        uint8_t ext_sz = 0;
        Bytes entries;
        Bytes parsed = enip_cip_parse_response(cip_payload, &cip_status, &ext_sz, &entries);

        if(bytes_is_null(parsed)) { return PLCTAG_ERR_REMOTE_ERR; }

        if(cip_status != CIP_STATUS_SUCCESS && cip_status != CIP_STATUS_PARTIAL) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
                   "ENIP: root symbol CIP status 0x%02x (fragment %d)", cip_status, fragment);
            /* PLCs that do not support this query return an error here; treat as non-fatal */
            return PLCTAG_STATUS_OK;
        }

        /* Parse tag_list_entry records.
         * Fixed part: instance_id(4)+symbol_type(2)+element_size(2)+dims(12)+name_len(2)=22 */
        int32_t  count_this = 0;
        uint32_t last_id    = (uint32_t)instance_id;

        while(entries.len >= 22) {
            uint32_t inst_id = 0;
            uint16_t sym_type = 0;
            uint16_t elem_sz  = 0;
            uint32_t dims[3]  = {0, 0, 0};
            uint16_t name_len = 0;

            Bytes rest = bytes_unpack(entries, BYTES_LE,
                                      &inst_id, &sym_type, &elem_sz,
                                      &dims[0], &dims[1], &dims[2],
                                      &name_len);

            if(bytes_is_null(rest) || rest.len < (size_t)name_len) { break; }

            if(name_len > 0 && name_len < 128) {
                rc = enip_root_symbol_store(conn, inst_id, sym_type, elem_sz, dims,
                                            (const char *)rest.data, name_len);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
                           "ENIP: failed to store root symbol instance %u: %d", inst_id, rc);
                }

                pdebug(DEBUG_MODULE_ENIP, DEBUG_SPEW, 0,
                       "ENIP: symbol instance=%u type=0x%04x size=%u name='%.*s'",
                       inst_id, sym_type, elem_sz, (int32_t)name_len, rest.data);
            }

            last_id = inst_id;
            count_this++;
            total++;

            entries = bytes_slice(rest, (size_t)name_len, rest.len - (size_t)name_len);
        }

        pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0,
               "ENIP: fragment %d: %d entries (last_id=%u status=0x%02x)",
               fragment, count_this, last_id, cip_status);

        fragment++;

        if(count_this == 0 || cip_status == CIP_STATUS_SUCCESS) { break; }

        instance_id = (uint16_t)(last_id + 1u);
        if(instance_id == 0u) { break; } /* wrapped */
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0,
           "ENIP: root symbol inventory complete: %d symbols in %d fragments",
           total, fragment);

    return PLCTAG_STATUS_OK;
}

/* ============================================================================
 * Lazy Fetch: Per-Tag Type/Size/Dims
 * Called the first time a tag is used, after Phase 1 resolved instance_id.
 * ============================================================================ */

/* Phase 1: correct as-is — no changes needed.
 * Fetches attributes 0x02 (symbol_type), 0x07 (element_size), 0x08 (array_dims)
 * for a specific instance_id.  Call this from the engine when tag->metadata_phase2_ready
 * is false, before issuing the first read/write for the tag.
 * Results are cached so each instance_id is only fetched once per session. */
int32_t enip_metadata_fetch_tag_info(enip_connection_t *conn, uint32_t tag_instance_id,
                                      uint16_t *symbol_type_out, uint16_t *element_size_out,
                                      uint32_t *array_dims_out) {
    if(!conn || !conn->socket || !conn->session_established) { return PLCTAG_ERR_NULL_PTR; }
    if(!symbol_type_out || !element_size_out || !array_dims_out) { return PLCTAG_ERR_NULL_PTR; }

    if(conn->metadata_cache) {
        int32_t rc = enip_metadata_get_cached(conn, tag_instance_id, symbol_type_out,
                                               element_size_out, array_dims_out);
        if(rc == PLCTAG_STATUS_OK) { return PLCTAG_STATUS_OK; }
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0,
           "ENIP: fetching Phase-2 metadata for instance %u", tag_instance_id);

    arena_reset(&conn->tx_arena);

    Bytes cip = bytes_pack(&conn->tx_arena, BYTES_LE,
                           (uint8_t)CIP_SVC_GET_INSTANCE_ATTR_LIST,
                           (uint8_t)0x03,
                           (uint8_t)0x20,  (uint8_t)0x6B,
                           (uint8_t)0x25,  (uint8_t)0x00,
                           (uint16_t)tag_instance_id,
                           (uint16_t)3,
                           (uint16_t)0x02,
                           (uint16_t)0x07,
                           (uint16_t)0x08);

    if(bytes_is_null(cip)) { return PLCTAG_ERR_NO_MEM; }

    Bytes cpf = enip_cpf_build_unconnected(&conn->tx_arena, cip);
    if(bytes_is_null(cpf)) { return PLCTAG_ERR_NO_MEM; }

    Bytes frame = enip_eip_build_request(&conn->tx_arena, ENIP_CMD_UNCONNECTED_SEND,
                                         conn->session_handle, &conn->sender_context, cpf);
    if(bytes_is_null(frame)) { return PLCTAG_ERR_NO_MEM; }

    socket_wait_state_t io_state = {0};
    int32_t rc = socket_write_wait(conn->socket, &frame, 5000, &io_state);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
               "ENIP: Phase-2 send failed for instance %u: %d", tag_instance_id, rc);
        return rc;
    }
    conn->messages_sent++;

    arena_reset(&conn->rx_arena);
    Bytes response = bytes_alloc(&conn->rx_arena, 512);
    if(bytes_is_null(response)) { return PLCTAG_ERR_NO_MEM; }

    rc = socket_read_wait(conn->socket, &response, 5000, &io_state);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
               "ENIP: Phase-2 read failed for instance %u: %d", tag_instance_id, rc);
        return rc;
    }
    conn->messages_received++;

    Bytes cpf_payload = enip_eip_extract_cpf_payload(response);
    if(bytes_is_null(cpf_payload)) { return PLCTAG_ERR_REMOTE_ERR; }

    Bytes cip_payload = enip_cpf_extract_udi_payload(cpf_payload);
    if(bytes_is_null(cip_payload)) { return PLCTAG_ERR_REMOTE_ERR; }

    uint8_t cip_status = 0;
    uint8_t ext_sz = 0;
    Bytes data;
    Bytes parsed = enip_cip_parse_response(cip_payload, &cip_status, &ext_sz, &data);

    if(bytes_is_null(parsed) || cip_status != CIP_STATUS_SUCCESS) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
               "ENIP: Phase-2 CIP status 0x%02x for instance %u", cip_status, tag_instance_id);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* Response payload: symbol_type(2) + element_size(2) + dims[3](12) = 16 bytes */
    if(data.len < 16) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
               "ENIP: Phase-2 response too short (%zu bytes) for instance %u",
               data.len, tag_instance_id);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    uint16_t sym_type = 0;
    uint16_t elem_sz  = 0;
    uint32_t dims[3]  = {0, 0, 0};

    bytes_unpack(data, BYTES_LE, &sym_type, &elem_sz, &dims[0], &dims[1], &dims[2]);

    *symbol_type_out  = sym_type;
    *element_size_out = elem_sz;
    memcpy(array_dims_out, dims, sizeof(dims));

    enip_metadata_set_cached(conn, tag_instance_id, sym_type, elem_sz, dims);

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0,
           "ENIP: Phase-2 for instance %u: type=0x%04x size=%u dims=[%u,%u,%u]",
           tag_instance_id, sym_type, elem_sz, dims[0], dims[1], dims[2]);

    return PLCTAG_STATUS_OK;
}
