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

/*
 * ab_listing.c — Rockwell/AB CIP tag and UDT listing dialect.
 *
 * Class 0x6B (Symbol object):
 *   Service 0x55 (GetInstanceAttributeList) returns paged tag entries.
 *   Each entry: instance_id(u32) | sym_type(u16) | elem_len(u16) |
 *               dim[0..2](3xu32) | name_len(u16) | name(N bytes).
 *   Response status 0x06 when more tags follow; 0x00 when exhausted.
 *
 * Class 0x6C (Template object):
 *   Returns CIP "service unsupported" for all requests.  UDT template
 *   support is deferred; atomic-only tag lists work without it.
 *
 * Wire format reference: src/external_docs/TAG_ENUMERATION_PROTOCOL.md
 */

#include <stdint.h>

#include "platform.h"
#include "utils/debug.h"
#include <libplctag/protocols/enip/server/device.h>
#include <libplctag/protocols/enip/server/device_sim.h>
#include "ab_listing.h"

#define DEBUG_MOD DEBUG_MODULE_UTILS

/* CIP service code for GetInstanceAttributeList */
#define CIP_SRV_GET_INSTANCE_ATTR_LIST ((uint8_t)0x55)

/* Size of one fixed-width tag entry before the variable-length name */
#define TAG_ENTRY_FIXED_SIZE ((uint32_t)22)

/* ============================================================================
 * Forward declarations
 * ============================================================================ */

static void parse_start_instance(const uint8_t *path, uint32_t path_len,
                                 uint32_t *inst_out);
static uint16_t compute_symbol_type(tag_def_t *tag);

static int32_t handle_symbol_list(device_sim_t *sim, uint8_t service,
                                  const uint8_t *path, uint32_t path_len,
                                  const uint8_t *req, uint32_t req_len,
                                  uint8_t *resp, uint32_t resp_cap,
                                  uint32_t *resp_len, void *user_data);

static int32_t handle_template(device_sim_t *sim, uint8_t service,
                               const uint8_t *path, uint32_t path_len,
                               const uint8_t *req, uint32_t req_len,
                               uint8_t *resp, uint32_t resp_cap,
                               uint32_t *resp_len, void *user_data);

/* ============================================================================
 * Public functions
 * ============================================================================ */

extern int32_t ab_listing_register(device_sim_t *sim, device_t *dev) {
    int32_t rc;

    rc = device_sim_add_cip_object(sim, 0x6Bu, DEVICE_SIM_ANY_INSTANCE,
                                   handle_symbol_list, dev);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_ERROR, 0,
               "ab_listing_register: failed to register class 0x6B handler.");
        return rc;
    }

    rc = device_sim_add_cip_object(sim, 0x6Cu, DEVICE_SIM_ANY_INSTANCE,
                                   handle_template, dev);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_ERROR, 0,
               "ab_listing_register: failed to register class 0x6C handler.");
        return rc;
    }

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_INFO, 0,
           "ab_listing_register: registered class 0x6B/0x6C handlers.");
    return PLCTAG_STATUS_OK;
}

/* ============================================================================
 * Static functions
 * ============================================================================ */

/*
 * Walk logical CIP path bytes and extract the instance_id.
 * Handles 8-bit class (0x20) and 8-bit (0x24) or 16-bit (0x25) instance.
 */
static void parse_start_instance(const uint8_t *path, uint32_t path_len,
                                 uint32_t *inst_out) {
    uint32_t off = 0;

    *inst_out = 0;

    while(off < path_len) {
        uint8_t seg = path[off++];

        if(seg == 0x20) {          /* 8-bit class segment */
            if(off < path_len) { off++; }
        } else if(seg == 0x21) {   /* 16-bit class segment */
            /* pad byte + 2 data bytes */
            if(off + 2 < path_len) { off += 3; }
        } else if(seg == 0x24) {   /* 8-bit instance segment */
            if(off < path_len) { *inst_out = path[off]; }
            return;
        } else if(seg == 0x25) {   /* 16-bit instance segment */
            if(off + 2 < path_len) {
                off++;  /* reserved pad */
                *inst_out = (uint32_t)path[off] | ((uint32_t)path[off + 1] << 8);
            }
            return;
        } else {
            return;
        }
    }
}


/*
 * Build the 2-byte CIP symbol type field from a tag_def_t.
 *
 * Bit 15: structure flag (0 for all atomic types registered here)
 * Bits 14-13: dimension count (0=scalar, 1=1D, 2=2D, 3=3D)
 * Bit 12: system flag (0 for user-defined tags)
 * Bits 11-0: CIP type ID
 */
static uint16_t compute_symbol_type(tag_def_t *tag) {
    uint16_t type_id  = (uint16_t)(tag->tag_type & 0x0FFFu);
    uint16_t dim_bits = (uint16_t)((tag->num_dimensions & 3u) << 13);
    return dim_bits | type_id;
}


/*
 * handle_symbol_list — service 0x55 on class 0x6B.
 *
 * Packs tag entries into resp starting from start_instance (from path).
 * Returns DEVICE_SIM_MORE_DATA if the response was truncated (client should
 * re-request with last_instance_id + 1), PLCTAG_STATUS_OK when done.
 *
 * Tags are assigned 1-based instance IDs by their order in dev->tags.
 * PCCC tags (data_file_num != 0) are skipped; they are not CIP symbols.
 */
static int32_t handle_symbol_list(device_sim_t *sim, uint8_t service,
                                  const uint8_t *path, uint32_t path_len,
                                  const uint8_t *req, uint32_t req_len,
                                  uint8_t *resp, uint32_t resp_cap,
                                  uint32_t *resp_len, void *user_data) {
    device_t *dev = (device_t *)user_data;

    (void)sim; (void)req; (void)req_len;

    if(service != CIP_SRV_GET_INSTANCE_ATTR_LIST) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "ab_listing: class 0x6B does not support service 0x%02x.", (unsigned)service);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    uint32_t start_instance = 0;
    parse_start_instance(path, path_len, &start_instance);

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_DETAIL, 0,
           "ab_listing: symbol list request start_instance=%u.", (unsigned)start_instance);

    uint32_t instance_id = 0;
    tag_def_t *tag = dev->tags;
    *resp_len = 0;

    while(tag) {
        instance_id++;

        /* Skip PCCC tags and tags before the pagination start. */
        if(tag->data_file_num != 0 || instance_id < start_instance) {
            tag = tag->next_tag;
            continue;
        }

        uint32_t name_len   = (uint32_t)str_length(tag->name);
        uint32_t entry_size = TAG_ENTRY_FIXED_SIZE + name_len;

        /* No room for this entry: truncate and signal more data. */
        if(*resp_len + entry_size > resp_cap) {
            pdebug(DEBUG_MOD, PLCTAG_DEBUG_DETAIL, 0,
                   "ab_listing: truncating at instance %u; cap=%u used=%u.",
                   (unsigned)instance_id, (unsigned)resp_cap, (unsigned)*resp_len);
            return DEVICE_SIM_MORE_DATA;
        }

        uint8_t  *p       = resp + *resp_len;
        uint16_t sym_type = compute_symbol_type(tag);
        uint16_t elem_len = (uint16_t)tag->elem_size;
        uint32_t dim0 = (tag->num_dimensions >= 1) ? (uint32_t)tag->dimensions[0] : 0u;
        uint32_t dim1 = (tag->num_dimensions >= 2) ? (uint32_t)tag->dimensions[1] : 0u;
        uint32_t dim2 = (tag->num_dimensions >= 3) ? (uint32_t)tag->dimensions[2] : 0u;

        /* Pack little-endian: instance_id(u32) */
        p[0]  = (uint8_t)(instance_id & 0xFFu);
        p[1]  = (uint8_t)((instance_id >> 8) & 0xFFu);
        p[2]  = (uint8_t)((instance_id >> 16) & 0xFFu);
        p[3]  = (uint8_t)((instance_id >> 24) & 0xFFu);
        /* sym_type(u16) */
        p[4]  = (uint8_t)(sym_type & 0xFFu);
        p[5]  = (uint8_t)((sym_type >> 8) & 0xFFu);
        /* elem_len(u16) */
        p[6]  = (uint8_t)(elem_len & 0xFFu);
        p[7]  = (uint8_t)((elem_len >> 8) & 0xFFu);
        /* dim[0](u32) */
        p[8]  = (uint8_t)(dim0 & 0xFFu);
        p[9]  = (uint8_t)((dim0 >> 8) & 0xFFu);
        p[10] = (uint8_t)((dim0 >> 16) & 0xFFu);
        p[11] = (uint8_t)((dim0 >> 24) & 0xFFu);
        /* dim[1](u32) */
        p[12] = (uint8_t)(dim1 & 0xFFu);
        p[13] = (uint8_t)((dim1 >> 8) & 0xFFu);
        p[14] = (uint8_t)((dim1 >> 16) & 0xFFu);
        p[15] = (uint8_t)((dim1 >> 24) & 0xFFu);
        /* dim[2](u32) */
        p[16] = (uint8_t)(dim2 & 0xFFu);
        p[17] = (uint8_t)((dim2 >> 8) & 0xFFu);
        p[18] = (uint8_t)((dim2 >> 16) & 0xFFu);
        p[19] = (uint8_t)((dim2 >> 24) & 0xFFu);
        /* name_len(u16) */
        p[20] = (uint8_t)(name_len & 0xFFu);
        p[21] = (uint8_t)((name_len >> 8) & 0xFFu);
        /* name bytes (not null-terminated) */
        mem_copy(p + 22, tag->name, (int)name_len);

        *resp_len += entry_size;

        pdebug(DEBUG_MOD, PLCTAG_DEBUG_SPEW, 0,
               "ab_listing: packed instance %u '%s' sym_type=0x%04x.",
               (unsigned)instance_id, tag->name, (unsigned)sym_type);

        tag = tag->next_tag;
    }

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_DETAIL, 0,
           "ab_listing: symbol list complete, %u bytes.", (unsigned)*resp_len);
    return PLCTAG_STATUS_OK;
}


/*
 * handle_template — catch-all for class 0x6C (Template/UDT object).
 *
 * UDT template support is deferred to a later phase.  Returning
 * PLCTAG_ERR_UNSUPPORTED causes try_cip_object to send a CIP "service
 * unsupported" (0x08) error, which is the correct response when no UDT
 * definitions have been registered.
 */
static int32_t handle_template(device_sim_t *sim, uint8_t service,
                               const uint8_t *path, uint32_t path_len,
                               const uint8_t *req, uint32_t req_len,
                               uint8_t *resp, uint32_t resp_cap,
                               uint32_t *resp_len, void *user_data) {
    (void)sim; (void)path; (void)path_len; (void)req; (void)req_len;
    (void)resp; (void)resp_cap; (void)user_data;

    /* ponytail: UDT template not yet implemented; no UDT-typed tags in the
     * listing so the client will not query class 0x6C in practice. */
    *resp_len = 0;
    pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
           "ab_listing: class 0x6C service 0x%02x not yet supported (UDT/template).",
           (unsigned)service);
    return PLCTAG_ERR_UNSUPPORTED;
}
