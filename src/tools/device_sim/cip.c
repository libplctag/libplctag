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
 * cip.c — CIP (Common Industrial Protocol) request dispatcher.
 * Adapted from src/poc/ab_server_fiber/cip.c.
 *
 * Changes from fiber version:
 *   - plc_config_t → device_t
 *   - pdlog → pdebug
 *   - removed per-tag request stats (tag_def_t has no stats fields)
 *   - removed ForwardOpen connection path validation (accept any path)
 *   - mem_copy/mem_cmp/str_length in place of memcpy/memcmp/strlen
 */

#include <stdbool.h>
#include <stdint.h>

#include "platform.h"
#include "utils/arena.h"
#include "utils/atomic_utils.h"
#include "utils/bytes.h"
#include "utils/debug.h"
#include "cip.h"
#include "device.h"
#include "eip.h"
#include "identity.h"
#include "pccc.h"

#define DEBUG_MOD DEBUG_MODULE_UTILS

/* ============================================================================
 * CIP service codes
 * ============================================================================ */

#define CIP_SRV_GET_ATTRS_ALL   ((uint8_t)0x01)
#define CIP_SRV_GET_ATTR_SINGLE ((uint8_t)0x0E)
#define CIP_SRV_MULTI           ((uint8_t)0x0A)
#define CIP_SRV_PCCC_EXECUTE  ((uint8_t)0x4B)
#define CIP_SRV_READ          ((uint8_t)0x4C)
#define CIP_SRV_WRITE         ((uint8_t)0x4D)
#define CIP_SRV_FORWARD_CLOSE ((uint8_t)0x4E)
#define CIP_SRV_READ_FRAG     ((uint8_t)0x52)
#define CIP_SRV_WRITE_FRAG    ((uint8_t)0x53)
#define CIP_SRV_FORWARD_OPEN  ((uint8_t)0x54)
#define CIP_SRV_FORWARD_OPEN_EX ((uint8_t)0x5B)

#define CIP_DONE ((uint8_t)0x80)

#define CIP_SYMBOLIC_SEGMENT ((uint8_t)0x91)

/* ============================================================================
 * CIP error codes
 * ============================================================================ */

#define CIP_OK              ((uint8_t)0x00)
#define CIP_ERR_EXT_ERR     ((uint8_t)0x01)
#define CIP_ERR_INVALID_PARAM ((uint8_t)0x03)
#define CIP_ERR_PATH_SEGMENT  ((uint8_t)0x04)
#define CIP_ERR_PATH_UNKNOWN  ((uint8_t)0x05)
#define CIP_ERR_FRAG          ((uint8_t)0x06)
#define CIP_ERR_UNSUPPORTED   ((uint8_t)0x08)
#define CIP_ERR_INSUF_DATA    ((uint8_t)0x13)
#define CIP_ERR_TOO_MUCH_DATA ((uint8_t)0x15)

#define CIP_ERR_EX_DUPLICATE_CONN ((uint16_t)0x0100)

/* Connection Manager object path: class 0x06, instance 0x01 */
static const uint8_t CIP_CONN_MGR_PATH[] = {0x20, 0x06, 0x24, 0x01};

#define CIP_RESP_HDR_SIZE   ((size_t)4)
#define CIP_MIN_ATOMIC_SIZE ((size_t)8)
#define MAX_SUB_REQUESTS    ((uint16_t)500)

/* ============================================================================
 * Counters
 * ============================================================================ */

static atomic_int32_t s_conn_id_counter  = 1;
static atomic_int32_t s_conn_seq_counter = 1;

/* ============================================================================
 * Forward declarations
 * ============================================================================ */

static bool  parse_cip_request(Bytes input, uint8_t *svc, Bytes *svc_path, Bytes *svc_payload);
static bool  extract_path(Bytes input, size_t *offset, bool padded, Bytes *out_path);
static bool  parse_tag_path(Bytes tag_path, device_t *dev, tag_def_t **tag_out,
                             uint32_t *num_idx_out, uint32_t *indexes);
static bool  calc_offsets(tag_def_t *tag, uint32_t num_idx, uint32_t *indexes,
                          uint16_t elem_count, size_t *start_out, size_t *end_out);
static Bytes cip_error(Arena *a, uint8_t svc, uint8_t err, bool ext, uint16_t ext_err);
static Bytes handle_forward_open(Arena *a, uint8_t svc, Bytes svc_path, Bytes svc_payload,
                                 eip_session_t *sess, device_t *dev);
static Bytes handle_forward_close(Arena *a, uint8_t svc, Bytes svc_path, Bytes svc_payload,
                                  eip_session_t *sess, device_t *dev);
static Bytes handle_read(Arena *a, uint8_t svc, Bytes svc_path, Bytes svc_payload,
                         device_t *dev, size_t max_resp);
static Bytes handle_write(Arena *a, uint8_t svc, Bytes svc_path, Bytes svc_payload,
                          device_t *dev);
static Bytes handle_multi(Arena *a, uint8_t svc, Bytes svc_payload,
                          eip_session_t *sess, device_t *dev);
static bool  parse_class_instance_path(Bytes path, uint8_t *class_id, uint8_t *instance_id,
                                       uint8_t *attr_id);
static Bytes handle_identity(Arena *a, uint8_t svc, Bytes svc_path, Bytes svc_payload,
                              device_t *dev);

/* ============================================================================
 * Public functions
 * ============================================================================ */

extern Bytes cip_dispatch_unconnected(Arena *a, Bytes payload, eip_session_t *sess, device_t *dev) {
    uint8_t svc = 0;
    Bytes svc_path    = {0};
    Bytes svc_payload = {0};

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_DETAIL, 0, "cip_dispatch_unconnected: len=%zu.", payload.len);

    if(!parse_cip_request(payload, &svc, &svc_path, &svc_payload)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "Failed to parse CIP request.");
        return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
    }

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_DETAIL, 0, "CIP unconnected service=0x%02x.", (unsigned)svc);

    switch(svc) {
        case CIP_SRV_GET_ATTRS_ALL:
        case CIP_SRV_GET_ATTR_SINGLE:
            return handle_identity(a, svc, svc_path, svc_payload, dev);

        case CIP_SRV_FORWARD_OPEN:
        case CIP_SRV_FORWARD_OPEN_EX:
            return handle_forward_open(a, svc, svc_path, svc_payload, sess, dev);

        case CIP_SRV_FORWARD_CLOSE:
            return handle_forward_close(a, svc, svc_path, svc_payload, sess, dev);

        case CIP_SRV_PCCC_EXECUTE:
            return pccc_dispatch(a, payload, sess, dev);

        case CIP_SRV_READ_FRAG: {
            /* 0x52 is overloaded: path=ConnMgr → Unconnected Send wrapper; else Read Frag. */
            if(mem_cmp((void*)svc_path.data, (int)svc_path.len,
                       (void*)CIP_CONN_MGR_PATH, (int)sizeof(CIP_CONN_MGR_PATH)) == 0) {
                uint16_t embedded_len = 0;
                Bytes embedded_rest = bytes_unpack(svc_payload, BYTES_LE, BYTES_SKIP(2), &embedded_len);
                if(bytes_is_null(embedded_rest)) {
                    pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
                           "Unconnected Send: failed to unpack embedded length.");
                    return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
                }
                Bytes embedded = bytes_slice(embedded_rest, 0, embedded_len);
                if(bytes_is_null(embedded)) {
                    pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
                           "Unconnected Send: embedded slice failed (len=%u).",
                           (unsigned)embedded_len);
                    return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
                }
                pdebug(DEBUG_MOD, PLCTAG_DEBUG_DETAIL, 0,
                       "Unconnected Send: unwrapping embedded CIP request (%u bytes).",
                       (unsigned)embedded_len);
                return cip_dispatch_unconnected(a, embedded, sess, dev);
            }
            return handle_read(a, svc, svc_path, svc_payload, dev, sess->max_cip_packet_size);
        }

        case CIP_SRV_READ:
            return handle_read(a, svc, svc_path, svc_payload, dev, sess->max_cip_packet_size);

        case CIP_SRV_WRITE:
        case CIP_SRV_WRITE_FRAG:
            return handle_write(a, svc, svc_path, svc_payload, dev);

        default:
            pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
                   "Unsupported unconnected CIP service 0x%02x.", (unsigned)svc);
            return cip_error(a, svc, CIP_ERR_UNSUPPORTED, false, 0);
    }
}


extern Bytes cip_dispatch_connected(Arena *a, Bytes payload, eip_session_t *sess, device_t *dev, size_t max_resp) {
    uint8_t svc = 0;
    Bytes svc_path    = {0};
    Bytes svc_payload = {0};

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_DETAIL, 0, "cip_dispatch_connected: len=%zu.", payload.len);

    if(!parse_cip_request(payload, &svc, &svc_path, &svc_payload)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "Failed to parse connected CIP request.");
        return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
    }

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_DETAIL, 0, "CIP connected service=0x%02x.", (unsigned)svc);

    switch(svc) {
        case CIP_SRV_GET_ATTRS_ALL:
        case CIP_SRV_GET_ATTR_SINGLE:
            return handle_identity(a, svc, svc_path, svc_payload, dev);

        case CIP_SRV_MULTI:
            return handle_multi(a, svc, svc_payload, sess, dev);

        case CIP_SRV_READ:
        case CIP_SRV_READ_FRAG:
            return handle_read(a, svc, svc_path, svc_payload, dev, max_resp);

        case CIP_SRV_WRITE:
        case CIP_SRV_WRITE_FRAG:
            return handle_write(a, svc, svc_path, svc_payload, dev);

        default:
            pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
                   "Unsupported connected CIP service 0x%02x.", (unsigned)svc);
            return cip_error(a, svc, CIP_ERR_UNSUPPORTED, false, 0);
    }
}

/* ============================================================================
 * Static functions
 * ============================================================================ */

static bool parse_cip_request(Bytes input, uint8_t *svc, Bytes *svc_path, Bytes *svc_payload) {
    uint8_t path_len_words = 0;

    Bytes rest = bytes_unpack(input, BYTES_LE, svc, &path_len_words);
    if(bytes_is_null(rest)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "parse_cip_request: too short (len=%zu).", input.len);
        return false;
    }

    size_t path_bytes = (size_t)path_len_words * 2;
    if(path_bytes > rest.len) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "parse_cip_request: path_bytes=%zu exceeds rest.len=%zu.", path_bytes, rest.len);
        return false;
    }

    *svc_path    = bytes_slice(rest, 0, path_bytes);
    *svc_payload = bytes_slice(rest, path_bytes, rest.len - path_bytes);
    return true;
}


static bool extract_path(Bytes input, size_t *offset, bool padded, Bytes *out_path) {
    uint8_t path_len_words = 0;

    Bytes at_offset = bytes_slice(input, *offset, input.len - *offset);
    if(bytes_is_null(at_offset)) { return false; }

    Bytes rest = bytes_unpack(at_offset, BYTES_LE, &path_len_words);
    if(bytes_is_null(rest)) { return false; }
    *offset += 1;

    if(path_len_words == 0) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "extract_path: path_len_words is 0.");
        return false;
    }

    if(padded) {
        Bytes after_pad = bytes_slice(input, *offset, input.len - *offset);
        if(bytes_is_null(after_pad)) { return false; }
        after_pad = bytes_unpack(after_pad, BYTES_LE, BYTES_SKIP(1));
        if(bytes_is_null(after_pad)) { return false; }
        *offset += 1;
    }

    size_t path_bytes = (size_t)path_len_words * 2;
    if(*offset + path_bytes > input.len) { return false; }

    *out_path = bytes_slice(input, *offset, path_bytes);
    *offset  += path_bytes;
    return true;
}


static bool parse_tag_path(Bytes tag_path, device_t *dev, tag_def_t **tag_out,
                            uint32_t *num_idx_out, uint32_t *indexes) {
    uint8_t seg_type   = 0;
    uint8_t name_len_u8 = 0;

    *tag_out     = NULL;
    *num_idx_out = 0;

    Bytes rest = bytes_unpack(tag_path, BYTES_LE, &seg_type, &name_len_u8);
    if(bytes_is_null(rest)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "parse_tag_path: too short.");
        return false;
    }

    if(seg_type != CIP_SYMBOLIC_SEGMENT) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "parse_tag_path: expected symbolic segment 0x91, got 0x%02x.", (unsigned)seg_type);
        return false;
    }

    size_t name_len = (size_t)name_len_u8;
    if(name_len > rest.len) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "parse_tag_path: name_len=%zu exceeds remaining=%zu.", name_len, rest.len);
        return false;
    }

    const uint8_t *name_bytes = rest.data;
    rest = bytes_slice(rest, name_len, rest.len - name_len);

    if(name_len % 2 != 0) {
        Bytes after_pad = bytes_unpack(rest, BYTES_LE, BYTES_SKIP(1));
        if(bytes_is_null(after_pad)) { return false; }
        rest = after_pad;
    }

    /* Linear search for matching tag. */
    tag_def_t *tag = dev->tags;
    while(tag) {
        int32_t tag_name_len = str_length(tag->name);
        if(tag_name_len == (int32_t)name_len
           && mem_cmp((void*)tag->name, tag_name_len, (void*)name_bytes, (int)name_len) == 0) {
            break;
        }
        tag = tag->next_tag;
    }

    if(!tag) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "parse_tag_path: tag '%.*s' not found.", (int)name_len, name_bytes);
        return false;
    }

    *tag_out = tag;

    /* Parse optional numeric index segments. */
    while(rest.len > 0) {
        uint8_t idx_type = 0;
        uint8_t  idx_val8  = 0;
        uint16_t idx_val16 = 0;
        uint32_t idx_val32 = 0;

        if(*num_idx_out >= 3) {
            pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "parse_tag_path: too many index segments.");
            return false;
        }

        Bytes after_type = bytes_unpack(rest, BYTES_LE, &idx_type);
        if(bytes_is_null(after_type)) { break; }

        switch(idx_type) {
            case 0x28: {
                Bytes after_val = bytes_unpack(after_type, BYTES_LE, &idx_val8);
                if(bytes_is_null(after_val)) { return false; }
                indexes[(*num_idx_out)++] = (uint32_t)idx_val8;
                rest = after_val;
                break;
            }
            case 0x29: {
                Bytes after_pad = bytes_unpack(after_type, BYTES_LE, BYTES_SKIP(1));
                if(bytes_is_null(after_pad)) { return false; }
                Bytes after_val = bytes_unpack(after_pad, BYTES_LE, &idx_val16);
                if(bytes_is_null(after_val)) { return false; }
                indexes[(*num_idx_out)++] = (uint32_t)idx_val16;
                rest = after_val;
                break;
            }
            case 0x2A: {
                Bytes after_pad = bytes_unpack(after_type, BYTES_LE, BYTES_SKIP(1));
                if(bytes_is_null(after_pad)) { return false; }
                Bytes after_val = bytes_unpack(after_pad, BYTES_LE, &idx_val32);
                if(bytes_is_null(after_val)) { return false; }
                indexes[(*num_idx_out)++] = idx_val32;
                rest = after_val;
                break;
            }
            default:
                pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
                       "parse_tag_path: unknown index segment type 0x%02x.", (unsigned)idx_type);
                return false;
        }
    }

    if(*num_idx_out != 0 && *num_idx_out != (uint32_t)tag->num_dimensions) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "parse_tag_path: wrong index count: got %u expected 0 or %zu.",
               *num_idx_out, tag->num_dimensions);
        return false;
    }

    return true;
}


static bool calc_offsets(tag_def_t *tag, uint32_t num_idx, uint32_t *indexes,
                         uint16_t elem_count, size_t *start_out, size_t *end_out) {
    size_t total_elems = 1;
    size_t elem_offset = 0;

    for(size_t d = 0; d < tag->num_dimensions; d++) { total_elems *= tag->dimensions[d]; }

    for(uint32_t i = 0; i < num_idx; i++) {
        if(indexes[i] >= tag->dimensions[i]) {
            pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
                   "Index %u out of bounds for dim %u.", (unsigned)indexes[i], (unsigned)i);
            return false;
        }
    }

    switch(num_idx) {
        case 0: elem_offset = 0; break;
        case 1: elem_offset = indexes[0]; break;
        case 2: elem_offset = indexes[0] * tag->dimensions[1] + indexes[1]; break;
        case 3:
            elem_offset = indexes[0] * tag->dimensions[1] * tag->dimensions[2]
                        + indexes[1] * tag->dimensions[2]
                        + indexes[2];
            break;
        default: return false;
    }

    *start_out = elem_offset * tag->elem_size;
    *end_out   = *start_out + (size_t)elem_count * tag->elem_size;

    if(*end_out > total_elems * tag->elem_size) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "Request end offset %zu exceeds tag size %zu.",
               *end_out, total_elems * tag->elem_size);
        return false;
    }

    return true;
}


static Bytes cip_error(Arena *a, uint8_t svc, uint8_t err, bool ext, uint16_t ext_err) {
    if(ext) {
        return bytes_pack(a, BYTES_LE, (uint8_t)(svc | CIP_DONE), (uint8_t)0, err, (uint8_t)1, ext_err);
    }
    return bytes_pack(a, BYTES_LE, (uint8_t)(svc | CIP_DONE), (uint8_t)0, err, (uint8_t)0);
}


static Bytes handle_forward_open(Arena *a, uint8_t svc, Bytes svc_path, Bytes svc_payload,
                                 eip_session_t *sess, device_t *dev) {
    (void)dev; /* path validation deferred; device not needed in this phase */
    uint8_t  secs_per_tick    = 0;
    uint8_t  timeout_ticks    = 0;
    uint32_t server_conn_id   = 0;
    uint32_t client_conn_id   = 0;
    uint16_t conn_serial      = 0;
    uint16_t orig_vendor_id   = 0;
    uint32_t orig_serial      = 0;
    uint8_t  conn_timeout_mult = 0;
    uint32_t c2s_rpi          = 0;
    uint32_t s2c_rpi          = 0;
    uint32_t c2s_params       = 0;
    uint32_t s2c_params       = 0;
    uint8_t  transport_class  = 0;
    Bytes    conn_path        = {0};
    size_t   path_offset      = 0;

    /* svc_path must target the Connection Manager. */
    if(mem_cmp((void*)svc_path.data, (int)svc_path.len,
               (void*)CIP_CONN_MGR_PATH, (int)sizeof(CIP_CONN_MGR_PATH)) != 0) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "Forward Open: bad connection manager path.");
        return cip_error(a, svc, CIP_ERR_UNSUPPORTED, false, 0);
    }

    Bytes rest = bytes_unpack(svc_payload, BYTES_LE,
                              &secs_per_tick, &timeout_ticks,
                              &server_conn_id, &client_conn_id,
                              &conn_serial, &orig_vendor_id, &orig_serial,
                              &conn_timeout_mult, BYTES_SKIP(3),
                              &c2s_rpi);
    if(bytes_is_null(rest)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "Forward Open: fixed field unpack failed.");
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    if(svc == CIP_SRV_FORWARD_OPEN) {
        uint16_t p1 = 0, p2 = 0;
        rest = bytes_unpack(rest, BYTES_LE, &p1, &s2c_rpi, &p2);
        c2s_params = p1;
        s2c_params = p2;
    } else {
        rest = bytes_unpack(rest, BYTES_LE, &c2s_params, &s2c_rpi, &s2c_params);
    }

    if(bytes_is_null(rest)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "Forward Open: conn params unpack failed.");
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    rest = bytes_unpack(rest, BYTES_LE, &transport_class);
    if(bytes_is_null(rest)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "Forward Open: transport class unpack failed.");
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    /* Extract connection path; we accept any valid path. */
    if(!extract_path(rest, &path_offset, false, &conn_path)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "Forward Open: connection path extract failed.");
        return cip_error(a, svc, CIP_ERR_PATH_SEGMENT, false, 0);
    }

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_DETAIL, 0,
           "Forward Open: connection path len=%zu (accepted).", conn_path.len);

    /* Rejection counter for testing (starts at 0 = never reject). */
    if(sess->reject_fo_count > 0) {
        sess->reject_fo_count--;
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_INFO, 0,
               "Forward Open: rejecting (count remaining: %d).", (int)sess->reject_fo_count);
        return cip_error(a, svc, CIP_ERR_EXT_ERR, true, CIP_ERR_EX_DUPLICATE_CONN);
    }

    sess->client_connection_id            = client_conn_id;
    sess->client_connection_serial_number = conn_serial;
    sess->client_vendor_id                = orig_vendor_id;
    sess->client_serial_number            = orig_serial;
    sess->client_to_server_rpi            = c2s_rpi;
    sess->server_to_client_rpi            = s2c_rpi;

    int32_t cid = atomic_add_int32(&s_conn_id_counter, 1);
    if(cid == 0) { cid = atomic_add_int32(&s_conn_id_counter, 1); }
    sess->server_connection_id = (uint32_t)cid;

    int32_t cseq = atomic_add_int32(&s_conn_seq_counter, 1);
    sess->server_connection_seq = (uint16_t)(uint32_t)cseq;

    uint32_t pkt_mask = (svc == CIP_SRV_FORWARD_OPEN) ? 0x1FFu : 0x0FFFu;
    sess->client_to_server_max_packet = c2s_params & pkt_mask;
    sess->server_to_client_max_packet = s2c_params & pkt_mask;

    eip_session_set_connected_sizes(sess, sess->server_to_client_max_packet);

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_INFO, 0,
           "Forward Open success: server_conn_id=0x%08x seq=0x%04x.",
           (unsigned)sess->server_connection_id, (unsigned)sess->server_connection_seq);

    return bytes_pack(a, BYTES_LE,
                      (uint8_t)(svc | CIP_DONE), (uint8_t)0, CIP_OK, (uint8_t)0,
                      sess->server_connection_id, sess->client_connection_id,
                      conn_serial, orig_vendor_id, orig_serial,
                      c2s_rpi, s2c_rpi,
                      (uint8_t)0, (uint8_t)0);
}


static Bytes handle_forward_close(Arena *a, uint8_t svc, Bytes svc_path, Bytes svc_payload,
                                  eip_session_t *sess, device_t *dev) {
    uint8_t  secs_per_tick  = 0;
    uint8_t  timeout_ticks  = 0;
    uint16_t conn_serial    = 0;
    uint16_t vendor_id      = 0;
    uint32_t client_serial  = 0;
    Bytes    conn_path      = {0};
    size_t   path_offset    = 0;

    if(mem_cmp((void*)svc_path.data, (int)svc_path.len,
               (void*)CIP_CONN_MGR_PATH, (int)sizeof(CIP_CONN_MGR_PATH)) != 0) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "Forward Close: bad connection manager path.");
        return cip_error(a, svc, CIP_ERR_UNSUPPORTED, false, 0);
    }

    Bytes rest = bytes_unpack(svc_payload, BYTES_LE,
                              &secs_per_tick, &timeout_ticks,
                              &conn_serial, &vendor_id, &client_serial);
    if(bytes_is_null(rest)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "Forward Close: header unpack failed.");
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    if(!extract_path(rest, &path_offset, true, &conn_path)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "Forward Close: path extract failed.");
        return cip_error(a, svc, CIP_ERR_PATH_SEGMENT, false, 0);
    }

    if(conn_serial != sess->client_connection_serial_number
       || vendor_id != sess->client_vendor_id
       || client_serial != sess->client_serial_number) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "Forward Close: connection ID mismatch.");
        return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
    }

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_INFO, 0, "Forward Close: closing connection.");

    sess->server_connection_id  = 0;
    sess->client_connection_id  = 0;
    sess->server_connection_seq = 0;
    sess->client_connection_seq = 0;

    eip_session_set_unconnected_sizes(sess, dev->server_to_client_max_packet);

    return bytes_pack(a, BYTES_LE,
                      (uint8_t)(svc | CIP_DONE), (uint8_t)0, CIP_OK, (uint8_t)0,
                      conn_serial, vendor_id, client_serial,
                      (uint8_t)0, (uint8_t)0);
}


static Bytes handle_read(Arena *a, uint8_t svc, Bytes svc_path, Bytes svc_payload,
                         device_t *dev, size_t max_resp) {
    tag_def_t *tag    = NULL;
    uint32_t num_idx  = 3;
    uint32_t indexes[3] = {0};
    uint16_t elem_count  = 0;
    uint32_t frag_offset = 0;
    size_t byte_start    = 0;
    size_t byte_end      = 0;
    size_t copy_len      = 0;
    bool   fragmented    = false;

    if(!parse_tag_path(svc_path, dev, &tag, &num_idx, indexes)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "handle_read: failed to parse tag path.");
        return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
    }

    Bytes rest = bytes_unpack(svc_payload, BYTES_LE, &elem_count);
    if(bytes_is_null(rest)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "handle_read: failed to unpack elem_count.");
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    if(svc == CIP_SRV_READ_FRAG) {
        rest = bytes_unpack(rest, BYTES_LE, &frag_offset);
        if(bytes_is_null(rest)) {
            pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "handle_read: failed to unpack frag_offset.");
            return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
        }
    }

    if(!calc_offsets(tag, num_idx, indexes, elem_count, &byte_start, &byte_end)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "handle_read: calc_offsets failed for tag '%s' elem_count=%u.",
               tag->name, (unsigned)elem_count);
        return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
    }

    byte_start += frag_offset;
    if(byte_start > byte_end) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "handle_read: frag_offset 0x%08x pushes start past end.", (unsigned)frag_offset);
        return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
    }

    size_t resp_overhead = CIP_RESP_HDR_SIZE + 2;
    size_t max_data = (max_resp > resp_overhead) ? (max_resp - resp_overhead) : 0;

    size_t elem_bytes = (tag->elem_size < 8) ? tag->elem_size : 8;
    if(tag->elem_size > 0 && max_data > 0) { max_data = (max_data / elem_bytes) * elem_bytes; }

    copy_len = byte_end - byte_start;
    if(copy_len > max_data) { copy_len = max_data; }

    if(tag->elem_size > 0 && copy_len > 0) { copy_len = (copy_len / elem_bytes) * elem_bytes; }

    if(copy_len == 0) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "handle_read: no room for even one element (max_data=%zu elem_bytes=%zu).",
               max_data, elem_bytes);
        return cip_error(a, svc, CIP_ERR_FRAG, false, 0);
    }

    fragmented = (byte_start + copy_len < byte_end);

    Bytes hdr = bytes_pack(a, BYTES_LE,
                           (uint8_t)(svc | CIP_DONE), (uint8_t)0,
                           (uint8_t)(fragmented ? CIP_ERR_FRAG : CIP_OK), (uint8_t)0,
                           (int16_t)tag->tag_type);
    if(bytes_is_null(hdr)) {
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    Bytes resp;
    mutex_lock(tag->data_mutex);
    Bytes data = bytes_from_buf(tag->data + byte_start, copy_len);
    resp = bytes_concat(a, hdr, data);
    mutex_unlock(tag->data_mutex);

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_DETAIL, 0,
           "Read '%s': %zu bytes%s.", tag->name, copy_len, fragmented ? " (fragmented)" : "");

    return resp;
}


static Bytes handle_write(Arena *a, uint8_t svc, Bytes svc_path, Bytes svc_payload,
                          device_t *dev) {
    tag_def_t *tag   = NULL;
    uint32_t num_idx = 3;
    uint32_t indexes[3] = {0};
    uint16_t req_type    = 0;
    uint16_t elem_count  = 0;
    uint32_t frag_offset = 0;
    size_t byte_start    = 0;
    size_t byte_end      = 0;

    if(!parse_tag_path(svc_path, dev, &tag, &num_idx, indexes)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "handle_write: failed to parse tag path.");
        return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
    }

    Bytes rest = bytes_unpack(svc_payload, BYTES_LE, &req_type, &elem_count);
    if(bytes_is_null(rest)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "handle_write: failed to unpack type/elem_count.");
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    if(req_type != tag->tag_type) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "Write type mismatch: got 0x%04x expected 0x%04x.",
               (unsigned)req_type, (unsigned)tag->tag_type);
        return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
    }

    if(svc == CIP_SRV_WRITE_FRAG) {
        rest = bytes_unpack(rest, BYTES_LE, &frag_offset);
        if(bytes_is_null(rest)) {
            pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "handle_write: failed to unpack frag_offset.");
            return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
        }
    }

    if(!calc_offsets(tag, num_idx, indexes, elem_count, &byte_start, &byte_end)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "handle_write: calc_offsets failed for tag '%s' elem_count=%u.",
               tag->name, (unsigned)elem_count);
        return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
    }

    byte_start += frag_offset;

    size_t write_len = rest.len;
    if(write_len > byte_end - byte_start) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "Write data (%zu) exceeds tag slice (%zu).", write_len, byte_end - byte_start);
        return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
    }

    mutex_lock(tag->data_mutex);
    mem_copy(tag->data + byte_start, rest.data, (int)write_len);
    mutex_unlock(tag->data_mutex);

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_DETAIL, 0, "Write '%s': %zu bytes.", tag->name, write_len);

    return bytes_pack(a, BYTES_LE, (uint8_t)(svc | CIP_DONE), (uint8_t)0, CIP_OK, (uint8_t)0);
}


static bool parse_class_instance_path(Bytes path, uint8_t *class_id, uint8_t *instance_id,
                                       uint8_t *attr_id) {
    *attr_id = 0;
    if(path.len < 4) { return false; }
    /* Logical class segment 8-bit: 0x20 + class_id */
    if(path.data[0] != 0x20) { return false; }
    *class_id = path.data[1];
    /* Logical instance segment 8-bit: 0x24 + instance_id */
    if(path.data[2] != 0x24) { return false; }
    *instance_id = path.data[3];
    /* Optional logical attribute segment 8-bit: 0x30 + attr_id */
    if(path.len >= 6 && path.data[4] == 0x30) {
        *attr_id = path.data[5];
    }
    return true;
}


static Bytes handle_identity(Arena *a, uint8_t svc, Bytes svc_path, Bytes svc_payload,
                              device_t *dev) {
    (void)svc_payload;
    uint8_t class_id = 0, instance_id = 0, attr_id = 0;

    if(!parse_class_instance_path(svc_path, &class_id, &instance_id, &attr_id)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "handle_identity: malformed class/instance path (len=%zu).", svc_path.len);
        return cip_error(a, svc, CIP_ERR_PATH_SEGMENT, false, 0);
    }

    if(class_id != 0x01 || instance_id != 1) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "handle_identity: unsupported class=0x%02x instance=%u.",
               (unsigned)class_id, (unsigned)instance_id);
        return cip_error(a, svc, CIP_ERR_PATH_UNKNOWN, false, 0);
    }

    const identity_t *id = identity_for_plc_type(dev->plc_type);

    if(svc == CIP_SRV_GET_ATTRS_ALL) {
        Bytes obj = identity_encode_get_attrs_all(a, id);
        if(bytes_is_null(obj)) { return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0); }
        Bytes hdr = bytes_pack(a, BYTES_LE,
                               (uint8_t)(svc | CIP_DONE), (uint8_t)0, CIP_OK, (uint8_t)0);
        if(bytes_is_null(hdr)) { return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0); }
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_DETAIL, 0, "Identity GetAttributesAll OK.");
        return bytes_concat(a, hdr, obj);
    }

    if(svc == CIP_SRV_GET_ATTR_SINGLE) {
        if(attr_id == 0) {
            pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
                   "handle_identity: GetAttributeSingle — no attribute in path.");
            return cip_error(a, svc, CIP_ERR_PATH_SEGMENT, false, 0);
        }
        Bytes val = identity_encode_get_attr_single(a, (uint16_t)attr_id, id);
        if(bytes_is_null(val)) {
            pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
                   "handle_identity: unsupported attribute %u.", (unsigned)attr_id);
            return cip_error(a, svc, CIP_ERR_PATH_UNKNOWN, false, 0);
        }
        Bytes hdr = bytes_pack(a, BYTES_LE,
                               (uint8_t)(svc | CIP_DONE), (uint8_t)0, CIP_OK, (uint8_t)0);
        if(bytes_is_null(hdr)) { return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0); }
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_DETAIL, 0,
               "Identity GetAttributeSingle attr=%u OK.", (unsigned)attr_id);
        return bytes_concat(a, hdr, val);
    }

    return cip_error(a, svc, CIP_ERR_UNSUPPORTED, false, 0);
}


static Bytes handle_multi(Arena *a, uint8_t svc, Bytes svc_payload,
                          eip_session_t *sess, device_t *dev) {
    uint16_t svc_count = 0;

    Bytes payload_rest = bytes_unpack(svc_payload, BYTES_LE, &svc_count);
    if(bytes_is_null(payload_rest)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "handle_multi: failed to unpack service count.");
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    if(svc_count == 0 || svc_count > MAX_SUB_REQUESTS) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "handle_multi: invalid service count %u (max %u).",
               (unsigned)svc_count, (unsigned)MAX_SUB_REQUESTS);
        return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
    }

    uint16_t *req_offsets = (uint16_t *)arena_alloc(a, (size_t)svc_count * sizeof(uint16_t));
    if(!req_offsets) {
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    payload_rest = bytes_unpack(payload_rest, BYTES_LE, BYTES_ARRAY(req_offsets, (size_t)svc_count));
    if(bytes_is_null(payload_rest)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "handle_multi: failed to unpack %u request offsets.", (unsigned)svc_count);
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    size_t max_packet = sess->max_cip_packet_size;
    if(max_packet < CIP_RESP_HDR_SIZE + 2) {
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }
    size_t max_payload = max_packet - CIP_RESP_HDR_SIZE - 2;

    if(max_payload < (size_t)svc_count * 6) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "handle_multi: max_payload=%zu too small for %u sub-responses.",
               max_payload, (unsigned)svc_count);
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    Bytes   *sub_responses   = (Bytes    *)arena_alloc(a, (size_t)svc_count * sizeof(Bytes));
    uint16_t *response_offsets = (uint16_t *)arena_alloc(a, (size_t)svc_count * sizeof(uint16_t));
    if(!sub_responses || !response_offsets) {
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    size_t running_offset = 2 + (size_t)svc_count * 2;

    for(uint16_t i = 0; i < svc_count; i++) {
        uint16_t req_off  = req_offsets[i];
        uint16_t next_off = (i + 1 < svc_count) ? req_offsets[i + 1] : (uint16_t)svc_payload.len;

        if(req_off >= svc_payload.len || next_off > svc_payload.len || req_off >= next_off) {
            pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
                   "handle_multi: sub-request %u has invalid offsets [%u,%u).",
                   (unsigned)i, (unsigned)req_off, (unsigned)next_off);
            return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
        }

        size_t remaining_entries = (size_t)(svc_count - i);
        size_t cap = max_payload - remaining_entries * 6;

        Bytes sub_req = bytes_slice(svc_payload, req_off, (size_t)(next_off - req_off));
        Bytes sub_resp;
        if(cap < CIP_RESP_HDR_SIZE) {
            uint8_t sub_svc = (sub_req.len > 0) ? sub_req.data[0] : 0;
            sub_resp = cip_error(a, sub_svc, CIP_ERR_FRAG, false, 0);
        } else {
            sub_resp = cip_dispatch_connected(a, sub_req, sess, dev, cap);
            if(bytes_is_null(sub_resp)) {
                uint8_t sub_svc = (sub_req.len > 0) ? sub_req.data[0] : 0;
                sub_resp = cip_error(a, sub_svc, CIP_ERR_FRAG, false, 0);
            }
        }

        response_offsets[i] = (uint16_t)running_offset;
        sub_responses[i]    = sub_resp;
        running_offset     += sub_resp.len;
        max_payload        -= (2 + sub_resp.len);
    }

    size_t resp_size = CIP_RESP_HDR_SIZE + running_offset;
    Bytes result = bytes_alloc(a, resp_size);
    if(bytes_is_null(result)) {
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    Bytes rest = bytes_pack_into(result, BYTES_LE,
                                 (uint8_t)(svc | CIP_DONE), (uint8_t)0, CIP_OK, (uint8_t)0,
                                 svc_count);
    rest = bytes_pack_into(rest, BYTES_LE, BYTES_ARRAY(response_offsets, (size_t)svc_count));

    for(uint16_t i = 0; i < svc_count; i++) {
        if(sub_responses[i].len > 0) {
            mem_copy(rest.data, sub_responses[i].data, (int)sub_responses[i].len);
            rest = bytes_slice(rest, sub_responses[i].len, rest.len - sub_responses[i].len);
        }
    }

    return result;
}
