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
 * cip.c — CIP (Common Industrial Protocol) request dispatcher.
 *
 * Implements CIP service dispatch for both connected and unconnected requests.
 * All tag data access is single-threaded (fiber model) — no mutexes needed.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "arena.h"
#include "bytes.h"
#include "cip.h"
#include "eip.h"
#include "log.h"
#include "pccc.h"
#include "plc.h"
#include "utils.h"

/* ============================================================================
 * CIP service codes
 * ============================================================================ */

#define CIP_SRV_MULTI ((uint8_t)0x0A)
#define CIP_SRV_PCCC_EXECUTE ((uint8_t)0x4B)
#define CIP_SRV_READ ((uint8_t)0x4C)
#define CIP_SRV_WRITE ((uint8_t)0x4D)
#define CIP_SRV_FORWARD_CLOSE ((uint8_t)0x4E)
#define CIP_SRV_READ_FRAG ((uint8_t)0x52)
#define CIP_SRV_WRITE_FRAG ((uint8_t)0x53)
#define CIP_SRV_FORWARD_OPEN ((uint8_t)0x54)
#define CIP_SRV_FORWARD_OPEN_EX ((uint8_t)0x5B)

/* Response bit ORed into service code for all CIP responses. */
#define CIP_DONE ((uint8_t)0x80)

/* Symbolic segment marker for tag name paths. */
#define CIP_SYMBOLIC_SEGMENT ((uint8_t)0x91)

/* ============================================================================
 * CIP error codes
 * ============================================================================ */

#define CIP_OK ((uint8_t)0x00)
#define CIP_ERR_EXT_ERR ((uint8_t)0x01)
#define CIP_ERR_INVALID_PARAM ((uint8_t)0x03)
#define CIP_ERR_PATH_SEGMENT ((uint8_t)0x04)
#define CIP_ERR_PATH_UNKNOWN ((uint8_t)0x05)
#define CIP_ERR_FRAG ((uint8_t)0x06)
#define CIP_ERR_UNSUPPORTED ((uint8_t)0x08)
#define CIP_ERR_INSUF_DATA ((uint8_t)0x13)
#define CIP_ERR_TOO_MUCH_DATA ((uint8_t)0x15)

#define CIP_ERR_EX_DUPLICATE_CONN ((uint16_t)0x0100)

/* Connection Manager object path: class 0x06, instance 0x01 */
static const uint8_t CIP_CONN_MGR_PATH[] = {0x20, 0x06, 0x24, 0x01};

/* Minimum CIP response: 4 bytes (service|0x80, reserved, status, ext_size) */
#define CIP_RESP_HDR_SIZE ((size_t)4)

/*
 * Minimum data we'll try to fit in a fragmented read response.
 * Prevents splitting sub-8-byte atomic types across packets.
 */
#define CIP_MIN_ATOMIC_SIZE ((size_t)8)

/* Maximum sub-requests in a multi-service request. */
#define MAX_SUB_REQUESTS ((uint16_t)500)

/* ============================================================================
 * Forward declarations
 * ============================================================================ */

static bool parse_cip_request(Bytes input, uint8_t *svc, Bytes *svc_path, Bytes *svc_payload);
static bool extract_path(Bytes input, size_t *offset, bool padded, Bytes *out_path);
static bool parse_tag_path(Bytes tag_path, plc_config_t *cfg, tag_def_t **tag_out, uint32_t *num_idx_out, uint32_t *indexes);
static bool calc_offsets(tag_def_t *tag, uint32_t num_idx, uint32_t *indexes, uint16_t elem_count, size_t *start_out,
                         size_t *end_out);
static Bytes cip_error(Arena *a, uint8_t svc, uint8_t err, bool ext, uint16_t ext_err);
static Bytes handle_forward_open(Arena *a, uint8_t svc, Bytes svc_path, Bytes svc_payload, eip_session_t *sess,
                                 plc_config_t *cfg);
static Bytes handle_forward_close(Arena *a, uint8_t svc, Bytes svc_path, Bytes svc_payload, eip_session_t *sess, plc_config_t *cfg);
static Bytes handle_read(Arena *a, uint8_t svc, Bytes svc_path, Bytes svc_payload, plc_config_t *cfg, size_t max_resp);
static Bytes handle_write(Arena *a, uint8_t svc, Bytes svc_path, Bytes svc_payload, plc_config_t *cfg);
static Bytes handle_multi(Arena *a, uint8_t svc, Bytes svc_payload, eip_session_t *sess, plc_config_t *cfg);

/* ============================================================================
 * Public functions
 * ============================================================================ */

extern Bytes cip_dispatch_unconnected(Arena *a, Bytes payload, eip_session_t *sess, plc_config_t *cfg) {
    uint8_t svc = 0;
    Bytes svc_path = {0};
    Bytes svc_payload = {0};

    pdlog(LOG_MODULE_CIP, LOG_LEVEL_DETAIL, "cip_dispatch_unconnected: len=%zu", payload.len);

    if(!parse_cip_request(payload, &svc, &svc_path, &svc_payload)) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "Failed to parse CIP request");
        return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
    }

    pdlog(LOG_MODULE_CIP, LOG_LEVEL_DETAIL, "CIP unconnected service=0x%02x", svc);

    switch(svc) {
        case CIP_SRV_FORWARD_OPEN:
        case CIP_SRV_FORWARD_OPEN_EX: return handle_forward_open(a, svc, svc_path, svc_payload, sess, cfg);

        case CIP_SRV_FORWARD_CLOSE: return handle_forward_close(a, svc, svc_path, svc_payload, sess, cfg);

        case CIP_SRV_PCCC_EXECUTE: return pccc_dispatch(a, payload, sess, cfg);

        case CIP_SRV_READ_FRAG: {
            /*
             * Service 0x52 is overloaded:
             *   - Path = Connection Manager (0x20 0x06 0x24 0x01): Unconnected Send wrapper.
             *     Unwrap the embedded CIP request and dispatch it recursively.
             *   - Any other path: Read Fragment targeting a tag directly.
             */
            if(svc_path.len == sizeof(CIP_CONN_MGR_PATH)
               && memcmp(svc_path.data, CIP_CONN_MGR_PATH, sizeof(CIP_CONN_MGR_PATH)) == 0) {
                uint16_t embedded_len = 0;
                Bytes embedded_rest = bytes_unpack(svc_payload, BYTES_LE, BYTES_SKIP(2), &embedded_len);
                if(bytes_is_null(embedded_rest)) {
                    pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "Unconnected Send: failed to unpack embedded length");
                    return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
                }
                Bytes embedded = bytes_slice(embedded_rest, 0, embedded_len);
                if(bytes_is_null(embedded)) {
                    pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "Unconnected Send: embedded slice failed (len=%u)",
                          (unsigned)embedded_len);
                    return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
                }
                pdlog(LOG_MODULE_CIP, LOG_LEVEL_DETAIL, "Unconnected Send: unwrapping embedded CIP request (%u bytes)",
                      (unsigned)embedded_len);
                return cip_dispatch_unconnected(a, embedded, sess, cfg);
            }
            {
                return handle_read(a, svc, svc_path, svc_payload, cfg, sess->max_cip_packet_size);
            }
        }

        case CIP_SRV_READ: {
            return handle_read(a, svc, svc_path, svc_payload, cfg, sess->max_cip_packet_size);
        }

        case CIP_SRV_WRITE:
        case CIP_SRV_WRITE_FRAG: return handle_write(a, svc, svc_path, svc_payload, cfg);

        default:
            pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "Unsupported unconnected CIP service 0x%02x", svc);
            return cip_error(a, svc, CIP_ERR_UNSUPPORTED, false, 0);
    }
}


extern Bytes cip_dispatch_connected(Arena *a, Bytes payload, eip_session_t *sess, plc_config_t *cfg, size_t max_resp) {
    uint8_t svc = 0;
    Bytes svc_path = {0};
    Bytes svc_payload = {0};

    pdlog(LOG_MODULE_CIP, LOG_LEVEL_DETAIL, "cip_dispatch_connected: len=%zu", payload.len);

    if(!parse_cip_request(payload, &svc, &svc_path, &svc_payload)) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "Failed to parse connected CIP request");
        return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
    }

    pdlog(LOG_MODULE_CIP, LOG_LEVEL_DETAIL, "CIP connected service=0x%02x", svc);

    switch(svc) {
        case CIP_SRV_MULTI: return handle_multi(a, svc, svc_payload, sess, cfg);

        case CIP_SRV_READ:
        case CIP_SRV_READ_FRAG: return handle_read(a, svc, svc_path, svc_payload, cfg, max_resp);

        case CIP_SRV_WRITE:
        case CIP_SRV_WRITE_FRAG: return handle_write(a, svc, svc_path, svc_payload, cfg);

        default:
            pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "Unsupported connected CIP service 0x%02x", svc);
            return cip_error(a, svc, CIP_ERR_UNSUPPORTED, false, 0);
    }
}

/* ============================================================================
 * Static functions
 * ============================================================================ */

/*
 * Parse a CIP request: service(1), path_len_words(1), path(2*path_len), payload(rest).
 */
static bool parse_cip_request(Bytes input, uint8_t *svc, Bytes *svc_path, Bytes *svc_payload) {
    uint8_t path_len_words = 0;

    pdlog(LOG_MODULE_CIP, LOG_LEVEL_DETAIL, "parse_cip_request: starting, input.len=%zu", input.len);

    Bytes rest = bytes_unpack(input, BYTES_LE, svc, &path_len_words);
    if(bytes_is_null(rest)) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "parse_cip_request: too short to read service and path length (len=%zu)",
              input.len);
        return false;
    }

    size_t path_bytes = (size_t)path_len_words * 2;

    pdlog(LOG_MODULE_CIP, LOG_LEVEL_DETAIL, "parse_cip_request: svc=0x%02x path_len_words=%u path_bytes=%zu rest.len=%zu",
          (unsigned)*svc, (unsigned)path_len_words, path_bytes, rest.len);

    if(path_bytes > rest.len) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "parse_cip_request: path_bytes=%zu exceeds remaining buffer rest.len=%zu",
              path_bytes, rest.len);
        return false;
    }

    *svc_path = bytes_slice(rest, 0, path_bytes);
    *svc_payload = bytes_slice(rest, path_bytes, rest.len - path_bytes);

    pdlog(LOG_MODULE_CIP, LOG_LEVEL_DETAIL, "parse_cip_request: done svc_path.len=%zu svc_payload.len=%zu", svc_path->len,
          svc_payload->len);
    return true;
}


/*
 * Extract a CIP path segment from input starting at *offset.
 * path_len_words is at *offset (padded=true adds a skip byte after it).
 * Sets out_path to the path bytes and advances *offset past them.
 */
static bool extract_path(Bytes input, size_t *offset, bool padded, Bytes *out_path) {
    uint8_t path_len_words = 0;

    pdlog(LOG_MODULE_CIP, LOG_LEVEL_DETAIL, "extract_path: starting offset=%zu input.len=%zu padded=%d", *offset, input.len,
          (int)padded);

    Bytes at_offset = bytes_slice(input, *offset, input.len - *offset);
    if(bytes_is_null(at_offset)) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "extract_path: offset=%zu beyond input.len=%zu", *offset, input.len);
        return false;
    }

    Bytes rest = bytes_unpack(at_offset, BYTES_LE, &path_len_words);
    if(bytes_is_null(rest)) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "extract_path: cannot read path_len_words at offset=%zu", *offset);
        return false;
    }
    *offset += 1;

    if(path_len_words == 0) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "extract_path: path_len_words is 0");
        return false;
    }

    if(padded) {
        /* skip one pad byte */
        Bytes after_pad = bytes_slice(input, *offset, input.len - *offset);
        if(bytes_is_null(after_pad)) {
            pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "extract_path: no room for pad byte at offset=%zu", *offset);
            return false;
        }
        after_pad = bytes_unpack(after_pad, BYTES_LE, BYTES_SKIP(1));
        if(bytes_is_null(after_pad)) {
            pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "extract_path: cannot read pad byte at offset=%zu", *offset);
            return false;
        }
        *offset += 1;
    }

    size_t path_bytes = (size_t)path_len_words * 2;

    pdlog(LOG_MODULE_CIP, LOG_LEVEL_DETAIL, "extract_path: path_len_words=%u path_bytes=%zu at offset=%zu input.len=%zu",
          (unsigned)path_len_words, path_bytes, *offset, input.len);

    if(*offset + path_bytes > input.len) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "extract_path: path extends beyond input: offset=%zu path_bytes=%zu input.len=%zu",
              *offset, path_bytes, input.len);
        return false;
    }

    *out_path = bytes_slice(input, *offset, path_bytes);
    *offset += path_bytes;

    pdlog(LOG_MODULE_CIP, LOG_LEVEL_DETAIL, "extract_path: done, path.len=%zu", out_path->len);
    return true;
}


/*
 * Parse a CIP symbolic tag path to find the matching tag_def_t and any
 * numeric index segments.  Sets *num_idx_out to the count of indexes found.
 */
static bool parse_tag_path(Bytes tag_path, plc_config_t *cfg, tag_def_t **tag_out, uint32_t *num_idx_out, uint32_t *indexes) {
    uint8_t seg_type = 0;
    uint8_t name_len_u8 = 0;

    *tag_out = NULL;
    *num_idx_out = 0;

    pdlog(LOG_MODULE_CIP, LOG_LEVEL_DETAIL, "parse_tag_path: starting, path.len=%zu", tag_path.len);

    /* Symbolic segment: 0x91 <name_len> <name_bytes> [pad] */
    Bytes rest = bytes_unpack(tag_path, BYTES_LE, &seg_type, &name_len_u8);
    if(bytes_is_null(rest)) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "parse_tag_path: too short to read segment type and name length");
        return false;
    }

    if(seg_type != CIP_SYMBOLIC_SEGMENT) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "parse_tag_path: expected symbolic segment 0x91, got 0x%02x", (unsigned)seg_type);
        return false;
    }

    size_t name_len = (size_t)name_len_u8;

    if(name_len > rest.len) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "parse_tag_path: name_len=%zu exceeds remaining=%zu", name_len, rest.len);
        return false;
    }

    const uint8_t *name_bytes = rest.data;
    rest = bytes_slice(rest, name_len, rest.len - name_len);

    /* Align to 16-bit boundary: if name_len is odd, skip one pad byte. */
    if(name_len % 2 != 0) {
        Bytes after_pad = bytes_unpack(rest, BYTES_LE, BYTES_SKIP(1));
        if(bytes_is_null(after_pad)) {
            pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "parse_tag_path: no room for alignment pad byte");
            return false;
        }
        rest = after_pad;
    }

    pdlog(LOG_MODULE_CIP, LOG_LEVEL_DETAIL, "parse_tag_path: name='%.*s' (%zu bytes)", (int32_t)name_len, name_bytes, name_len);

    /* Linear search for matching tag. */
    tag_def_t *tag = cfg->tags;
    while(tag) {
        size_t tag_name_len = strlen(tag->name);
        if(tag_name_len == name_len && memcmp(tag->name, name_bytes, name_len) == 0) { break; }
        tag = tag->next_tag;
    }

    if(!tag) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "parse_tag_path: tag '%.*s' not found", (int32_t)name_len, name_bytes);
        return false;
    }

    *tag_out = tag;

    /* Parse optional numeric index segments: 0x28(1B val), 0x29(pad+2B val), 0x2A(pad+4B val). */
    while(rest.len > 0) {
        uint8_t idx_type = 0;
        uint8_t idx_val8 = 0;
        uint16_t idx_val16 = 0;
        uint32_t idx_val32 = 0;

        if(*num_idx_out >= 3) {
            pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "parse_tag_path: too many index segments");
            return false;
        }

        Bytes after_type = bytes_unpack(rest, BYTES_LE, &idx_type);
        if(bytes_is_null(after_type)) { break; }

        switch(idx_type) {
            case 0x28: {
                /* 8-bit index: type(1) val(1) */
                Bytes after_val = bytes_unpack(after_type, BYTES_LE, &idx_val8);
                if(bytes_is_null(after_val)) {
                    pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "parse_tag_path: truncated 8-bit index segment");
                    return false;
                }
                indexes[(*num_idx_out)++] = (uint32_t)idx_val8;
                rest = after_val;
                break;
            }

            case 0x29: {
                /* 16-bit index: type(1) pad(1) val(2) */
                Bytes after_pad = bytes_unpack(after_type, BYTES_LE, BYTES_SKIP(1));
                if(bytes_is_null(after_pad)) {
                    pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "parse_tag_path: truncated pad in 16-bit index segment");
                    return false;
                }
                Bytes after_val = bytes_unpack(after_pad, BYTES_LE, &idx_val16);
                if(bytes_is_null(after_val)) {
                    pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "parse_tag_path: truncated 16-bit index value");
                    return false;
                }
                indexes[(*num_idx_out)++] = (uint32_t)idx_val16;
                rest = after_val;
                break;
            }

            case 0x2A: {
                /* 32-bit index: type(1) pad(1) val(4) */
                Bytes after_pad = bytes_unpack(after_type, BYTES_LE, BYTES_SKIP(1));
                if(bytes_is_null(after_pad)) {
                    pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "parse_tag_path: truncated pad in 32-bit index segment");
                    return false;
                }
                Bytes after_val = bytes_unpack(after_pad, BYTES_LE, &idx_val32);
                if(bytes_is_null(after_val)) {
                    pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "parse_tag_path: truncated 32-bit index value");
                    return false;
                }
                indexes[(*num_idx_out)++] = idx_val32;
                rest = after_val;
                break;
            }

            default:
                pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "parse_tag_path: unknown index segment type 0x%02x", (unsigned)idx_type);
                return false;
        }
    }

    /* Must have zero indexes (whole tag) or exactly num_dimensions indexes. */
    if(*num_idx_out != 0 && *num_idx_out != tag->num_dimensions) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "parse_tag_path: wrong index count: got %u expected 0 or %zu", *num_idx_out,
              tag->num_dimensions);
        return false;
    }

    pdlog(LOG_MODULE_CIP, LOG_LEVEL_DETAIL, "parse_tag_path: done, tag='%s' num_idx=%u", tag->name, *num_idx_out);
    return true;
}


/*
 * Calculate byte start/end offsets for a tag read or write request.
 * num_idx must be 0 (whole tag from start) or match tag->num_dimensions.
 */
static bool calc_offsets(tag_def_t *tag, uint32_t num_idx, uint32_t *indexes, uint16_t elem_count, size_t *start_out,
                         size_t *end_out) {
    size_t total_elems = 1;
    size_t elem_offset = 0;

    for(size_t d = 0; d < tag->num_dimensions; d++) { total_elems *= tag->dimensions[d]; }

    /* Check individual index bounds. */
    for(uint32_t i = 0; i < num_idx; i++) {
        if(indexes[i] >= tag->dimensions[i]) {
            pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "Index %u out of bounds for dim %u", indexes[i], i);
            return false;
        }
    }

    switch(num_idx) {
        case 0: elem_offset = 0; break;
        case 1: elem_offset = indexes[0]; break;
        case 2: elem_offset = indexes[0] * tag->dimensions[1] + indexes[1]; break;
        case 3:
            elem_offset = indexes[0] * tag->dimensions[1] * tag->dimensions[2] + indexes[1] * tag->dimensions[2] + indexes[2];
            break;
        default: return false;
    }

    *start_out = elem_offset * tag->elem_size;
    *end_out = *start_out + (size_t)elem_count * tag->elem_size;

    if(*end_out > total_elems * tag->elem_size) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "Request end offset %zu exceeds tag size %zu", *end_out,
              total_elems * tag->elem_size);
        return false;
    }

    return true;
}


/*
 * Build a CIP error response.
 * With ext=false: 4 bytes.  With ext=true: 6 bytes (adds 2-byte extended status).
 */
static Bytes cip_error(Arena *a, uint8_t svc, uint8_t err, bool ext, uint16_t ext_err) {
    if(ext) { return bytes_pack(a, BYTES_LE, (uint8_t)(svc | CIP_DONE), (uint8_t)0, err, (uint8_t)1, ext_err); }
    return bytes_pack(a, BYTES_LE, (uint8_t)(svc | CIP_DONE), (uint8_t)0, err, (uint8_t)0);
}


/*
 * Forward Open / Forward Open Extended.
 *
 * Parses the 42-byte (standard) or 46-byte (extended) FO body, validates the
 * connection path against plc_config->path, handles rejection, stores
 * connection state into eip_session_t, and returns the FO success response.
 */
static Bytes handle_forward_open(Arena *a, uint8_t svc, Bytes svc_path, Bytes svc_payload, eip_session_t *sess,
                                 plc_config_t *cfg) {
    uint8_t secs_per_tick = 0;
    uint8_t timeout_ticks = 0;
    uint32_t server_conn_id = 0;
    uint32_t client_conn_id = 0;
    uint16_t conn_serial = 0;
    uint16_t orig_vendor_id = 0;
    uint32_t orig_serial = 0;
    uint8_t conn_timeout_mult = 0;
    uint32_t c2s_rpi = 0;
    uint32_t s2c_rpi = 0;
    uint32_t c2s_params = 0;
    uint32_t s2c_params = 0;
    uint8_t transport_class = 0;
    Bytes conn_path = {0};
    size_t path_offset = 0;

    /* Verify path targets the Connection Manager object. */
    if(svc_path.len < sizeof(CIP_CONN_MGR_PATH) || memcmp(svc_path.data, CIP_CONN_MGR_PATH, sizeof(CIP_CONN_MGR_PATH)) != 0) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "Forward Open: bad connection manager path");
        return cip_error(a, svc, CIP_ERR_UNSUPPORTED, false, 0);
    }

    /* Parse fixed fields. */
    Bytes rest = bytes_unpack(svc_payload, BYTES_LE, &secs_per_tick, &timeout_ticks, &server_conn_id, &client_conn_id,
                              &conn_serial, &orig_vendor_id, &orig_serial, &conn_timeout_mult, BYTES_SKIP(3), &c2s_rpi);
    if(bytes_is_null(rest)) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "Forward Open: fixed field unpack failed");
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    /* c2s_params and s2c_params differ in width: 2B for standard FO, 4B for extended. */
    if(svc == CIP_SRV_FORWARD_OPEN) {
        uint16_t p1 = 0, p2 = 0;
        rest = bytes_unpack(rest, BYTES_LE, &p1, &s2c_rpi, &p2);
        c2s_params = p1;
        s2c_params = p2;
    } else {
        rest = bytes_unpack(rest, BYTES_LE, &c2s_params, &s2c_rpi, &s2c_params);
    }

    if(bytes_is_null(rest)) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "Forward Open: conn params unpack failed");
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    rest = bytes_unpack(rest, BYTES_LE, &transport_class);
    if(bytes_is_null(rest)) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "Forward Open: transport class unpack failed");
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    /* Extract connection path and verify it matches our configured path. */
    path_offset = 0;
    if(!extract_path(rest, &path_offset, false, &conn_path)) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "Forward Open: connection path extract failed");
        return cip_error(a, svc, CIP_ERR_PATH_SEGMENT, false, 0);
    }

    if(conn_path.len != cfg->path_len || memcmp(conn_path.data, cfg->path, cfg->path_len) != 0) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "Forward Open: connection path mismatch");
        return cip_error(a, svc, CIP_ERR_PATH_UNKNOWN, false, 0);
    }

    /* Handle rejection for testing. */
    if(sess->reject_fo_count > 0) {
        sess->reject_fo_count--;
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_INFO, "Forward Open: rejecting (count remaining: %d)", sess->reject_fo_count);
        return cip_error(a, svc, CIP_ERR_EXT_ERR, true, CIP_ERR_EX_DUPLICATE_CONN);
    }

    /* Store connection state. */
    sess->client_connection_id = client_conn_id;
    sess->client_connection_serial_number = conn_serial;
    sess->client_vendor_id = orig_vendor_id;
    sess->client_serial_number = orig_serial;
    sess->client_to_server_rpi = c2s_rpi;
    sess->server_to_client_rpi = s2c_rpi;

    /* Generate server-side connection identifiers (simple counter). */
    static uint32_t s_conn_id_counter = 1;
    sess->server_connection_id = s_conn_id_counter++;
    if(s_conn_id_counter == 0) { s_conn_id_counter = 1; }

    static uint16_t s_conn_seq_counter = 1;
    sess->server_connection_seq = s_conn_seq_counter++;

    /* Extract packet size limits from connection params. */
    uint32_t pkt_mask = (svc == CIP_SRV_FORWARD_OPEN) ? 0x1FFu : 0x0FFFu;
    sess->client_to_server_max_packet = c2s_params & pkt_mask;
    sess->server_to_client_max_packet = s2c_params & pkt_mask;

    eip_session_set_connected_sizes(sess, sess->server_to_client_max_packet);

    pdlog(LOG_MODULE_CIP, LOG_LEVEL_INFO, "Forward Open success: server_conn_id=0x%08x seq=0x%04x", sess->server_connection_id,
          sess->server_connection_seq);

    /* Build success response (30 bytes):
     *   CIP header (4): svc|0x80, reserved, status, ext_size
     *   O→T conn ID (4), T→O conn ID (4)
     *   conn serial (2), orig vendor ID (2), orig serial (4)
     *   O→T API / c2s_rpi (4), T→O API / s2c_rpi (4)
     *   app reply size (1), reserved (1)
     */
    return bytes_pack(a, BYTES_LE, (uint8_t)(svc | CIP_DONE), (uint8_t)0, CIP_OK, (uint8_t)0, sess->server_connection_id,
                      sess->client_connection_id, conn_serial, orig_vendor_id, orig_serial, c2s_rpi, s2c_rpi, (uint8_t)0,
                      (uint8_t)0);
}


/*
 * Forward Close.
 *
 * Validates serial number, vendor ID, and serial number against session state,
 * verifies the connection path, clears session state, and returns the FC response.
 */
static Bytes handle_forward_close(Arena *a, uint8_t svc, Bytes svc_path, Bytes svc_payload, eip_session_t *sess, plc_config_t *cfg) {
    uint8_t secs_per_tick = 0;
    uint8_t timeout_ticks = 0;
    uint16_t conn_serial = 0;
    uint16_t vendor_id = 0;
    uint32_t client_serial = 0;
    Bytes conn_path = {0};
    size_t path_offset = 0;

    if(svc_path.len < sizeof(CIP_CONN_MGR_PATH) || memcmp(svc_path.data, CIP_CONN_MGR_PATH, sizeof(CIP_CONN_MGR_PATH)) != 0) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "Forward Close: bad connection manager path");
        return cip_error(a, svc, CIP_ERR_UNSUPPORTED, false, 0);
    }

    Bytes rest = bytes_unpack(svc_payload, BYTES_LE, &secs_per_tick, &timeout_ticks, &conn_serial, &vendor_id, &client_serial);
    if(bytes_is_null(rest)) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "Forward Close: header unpack failed");
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    /* The FC path has a pad byte after the length (padded=true). */
    if(!extract_path(rest, &path_offset, true, &conn_path)) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "Forward Close: path extract failed");
        return cip_error(a, svc, CIP_ERR_PATH_SEGMENT, false, 0);
    }

    /* Validate connection identifiers. */
    if(conn_serial != sess->client_connection_serial_number || vendor_id != sess->client_vendor_id
       || client_serial != sess->client_serial_number) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "Forward Close: connection ID mismatch");
        return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
    }

    pdlog(LOG_MODULE_CIP, LOG_LEVEL_INFO, "Forward Close: closing connection");

    /* Clear connection state. */
    sess->server_connection_id = 0;
    sess->client_connection_id = 0;
    sess->server_connection_seq = 0;
    sess->client_connection_seq = 0;

    eip_session_set_unconnected_sizes(sess, cfg->server_to_client_max_packet);

    /* Build success response (14 bytes). */
    return bytes_pack(a, BYTES_LE, (uint8_t)(svc | CIP_DONE), (uint8_t)0, CIP_OK, (uint8_t)0, conn_serial, vendor_id,
                      client_serial, (uint8_t)0, (uint8_t)0);
}


/*
 * Read (0x4C) and Read Fragmented (0x52).
 *
 * Payload: elem_count(2) [frag_offset(4)]
 * Response: service|0x80 reserved status ext_size type_code(2) data...
 */
static Bytes handle_read(Arena *a, uint8_t svc, Bytes svc_path, Bytes svc_payload, plc_config_t *cfg, size_t max_resp) {
    tag_def_t *tag = NULL;
    uint32_t num_idx = 3;
    uint32_t indexes[3] = {0};
    uint16_t elem_count = 0;
    uint32_t frag_offset = 0;
    size_t byte_start = 0;
    size_t byte_end = 0;
    size_t copy_len = 0;
    bool fragmented = false;
    int64_t t_start = 0;

    if(!parse_tag_path(svc_path, cfg, &tag, &num_idx, indexes)) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "handle_read: failed to parse tag path");
        return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
    }

    t_start = util_time_us();

    Bytes rest = bytes_unpack(svc_payload, BYTES_LE, &elem_count);
    if(bytes_is_null(rest)) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "handle_read: failed to unpack elem_count");
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    if(svc == CIP_SRV_READ_FRAG) {
        rest = bytes_unpack(rest, BYTES_LE, &frag_offset);
        if(bytes_is_null(rest)) {
            pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "handle_read: failed to unpack frag_offset");
            return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
        }
    }

    if(!calc_offsets(tag, num_idx, indexes, elem_count, &byte_start, &byte_end)) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "handle_read: calc_offsets failed for tag '%s' elem_count=%u", tag->name,
              (unsigned)elem_count);
        return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
    }

    byte_start += frag_offset;
    if(byte_start > byte_end) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "handle_read: frag_offset 0x%08x pushes start past end", (unsigned)frag_offset);
        return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
    }

    /* max_resp is the per-response budget set by the caller (session max or multi-sub cap). */
    size_t resp_overhead = CIP_RESP_HDR_SIZE + 2;
    size_t max_data = (max_resp > resp_overhead) ? (max_resp - resp_overhead) : 0;

    /* Align max_data down to an element boundary to avoid splitting elements. */
    size_t elem_bytes = (tag->elem_size < 8) ? tag->elem_size : 8;
    if(tag->elem_size > 0 && max_data > 0) { max_data = (max_data / elem_bytes) * elem_bytes; }

    copy_len = byte_end - byte_start;
    if(copy_len > max_data) { copy_len = max_data; }

    /* Align copy_len down to element boundary. */
    if(tag->elem_size > 0 && copy_len > 0) { copy_len = (copy_len / elem_bytes) * elem_bytes; }

    /* No room for even one aligned element — signal partial data, no type included. */
    if(copy_len == 0) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "handle_read: no room for even one element (max_data=%zu elem_bytes=%zu)", max_data,
              elem_bytes);
        return cip_error(a, svc, CIP_ERR_FRAG, false, 0);
    }

    fragmented = (byte_start + copy_len < byte_end);

    /* Build response: header(4) + type(2) + data. */
    Bytes hdr = bytes_pack(a, BYTES_LE, (uint8_t)(svc | CIP_DONE), (uint8_t)0, (uint8_t)(fragmented ? CIP_ERR_FRAG : CIP_OK),
                           (uint8_t)0, (int16_t)tag->tag_type);
    if(bytes_is_null(hdr)) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "handle_read: arena alloc failed for response header");
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    Bytes data = bytes_from_buf(tag->data + byte_start, copy_len);
    Bytes resp = bytes_concat(a, hdr, data);

    /* Update per-tag stats. */
    int64_t latency = util_time_us() - t_start;
    tag->request_count++;
    tag->total_latency_us += latency;
    if(tag->min_latency_us == 0 || latency < tag->min_latency_us) { tag->min_latency_us = latency; }
    if(latency > tag->max_latency_us) { tag->max_latency_us = latency; }

    pdlog(LOG_MODULE_CIP, LOG_LEVEL_DETAIL, "Read '%s': %zu bytes %s", tag->name, copy_len, fragmented ? "(fragmented)" : "");

    return resp;
}


/*
 * Write (0x4D) and Write Fragmented (0x53).
 *
 * Payload: type(2) elem_count(2) [frag_offset(4)] data...
 * Response: service|0x80 reserved status ext_size
 */
static Bytes handle_write(Arena *a, uint8_t svc, Bytes svc_path, Bytes svc_payload, plc_config_t *cfg) {
    tag_def_t *tag = NULL;
    uint32_t num_idx = 3;
    uint32_t indexes[3] = {0};
    uint16_t req_type = 0;
    uint16_t elem_count = 0;
    uint32_t frag_offset = 0;
    size_t byte_start = 0;
    size_t byte_end = 0;
    int64_t t_start = 0;

    if(!parse_tag_path(svc_path, cfg, &tag, &num_idx, indexes)) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "handle_write: failed to parse tag path");
        return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
    }

    t_start = util_time_us();

    Bytes rest = bytes_unpack(svc_payload, BYTES_LE, &req_type, &elem_count);
    if(bytes_is_null(rest)) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "handle_write: failed to unpack type and elem_count");
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    if(req_type != tag->tag_type) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "Write type mismatch: got 0x%04x expected 0x%04x", req_type, tag->tag_type);
        return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
    }

    if(svc == CIP_SRV_WRITE_FRAG) {
        rest = bytes_unpack(rest, BYTES_LE, &frag_offset);
        if(bytes_is_null(rest)) {
            pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "handle_write: failed to unpack frag_offset");
            return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
        }
    }

    if(!calc_offsets(tag, num_idx, indexes, elem_count, &byte_start, &byte_end)) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "handle_write: calc_offsets failed for tag '%s' elem_count=%u", tag->name,
              (unsigned)elem_count);
        return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
    }

    byte_start += frag_offset;

    size_t write_len = rest.len;
    if(write_len > byte_end - byte_start) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "Write data (%zu) exceeds tag slice (%zu)", write_len, byte_end - byte_start);
        return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
    }

    memcpy(tag->data + byte_start, rest.data, write_len);

    /* Update per-tag stats. */
    int64_t latency = util_time_us() - t_start;
    tag->request_count++;
    tag->total_latency_us += latency;
    if(tag->min_latency_us == 0 || latency < tag->min_latency_us) { tag->min_latency_us = latency; }
    if(latency > tag->max_latency_us) { tag->max_latency_us = latency; }

    pdlog(LOG_MODULE_CIP, LOG_LEVEL_DETAIL, "Write '%s': %zu bytes", tag->name, write_len);

    return bytes_pack(a, BYTES_LE, (uint8_t)(svc | CIP_DONE), (uint8_t)0, CIP_OK, (uint8_t)0);
}


/*
 * Multi-Service Request (0x0A).
 *
 * Payload: service_count(2) offsets[service_count](2 each) sub-requests...
 * Response: service|0x80 reserved status ext_size
 *           service_count(2) response_offsets[service_count](2 each) sub-responses...
 *
 * Offsets in both request and response are measured from the start of the
 * service_count word (i.e., from byte 0 of the payload after the CPF strip).
 */
static Bytes handle_multi(Arena *a, uint8_t svc, Bytes svc_payload, eip_session_t *sess, plc_config_t *cfg) {
    uint16_t svc_count = 0;

    Bytes payload_rest = bytes_unpack(svc_payload, BYTES_LE, &svc_count);
    if(bytes_is_null(payload_rest)) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "handle_multi: failed to unpack service count");
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    if(svc_count == 0 || svc_count > MAX_SUB_REQUESTS) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "handle_multi: invalid service count %u (max %u)", (unsigned)svc_count,
              (unsigned)MAX_SUB_REQUESTS);
        return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
    }

    /* Allocate per-request arrays from the arena to avoid large stack frames. */
    uint16_t *req_offsets = (uint16_t *)arena_alloc(a, (size_t)svc_count * sizeof(uint16_t));
    if(!req_offsets) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "handle_multi: arena alloc failed for req_offsets (%u entries)",
              (unsigned)svc_count);
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    payload_rest = bytes_unpack(payload_rest, BYTES_LE, BYTES_ARRAY(req_offsets, (size_t)svc_count));
    if(bytes_is_null(payload_rest)) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "handle_multi: failed to unpack %u request offsets", (unsigned)svc_count);
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    /*
     * Budget allocation — all svc_count entries must appear in the response.
     *
     * Each entry costs at minimum 6 bytes: 2 (offset table entry) + 4 (min
     * CIP response header, status 0x06 with no data).
     *
     * max_payload = max_packet - CIP_RESP_HDR_SIZE(4) - service_count_field(2)
     *
     * Greedy allocation per entry i (0-indexed, remaining_entries = N-i):
     *
     *   cap_i = max_payload - remaining_entries * 6
     *
     * After entry i uses r_i bytes of sub-response:
     *   max_payload -= (2 + r_i)     [2 for offset entry, r_i for sub-response]
     *
     * This guarantees cap_i >= CIP_RESP_HDR_SIZE for all i as long as
     * max_payload >= svc_count * 6 on entry.  First entries receive the bulk
     * of the space; later entries fall back to a 4-byte 0x06 (partial data)
     * response once the data budget is exhausted.
     */
    size_t max_packet = sess->max_cip_packet_size;

    if(max_packet < CIP_RESP_HDR_SIZE + 2) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "handle_multi: max_packet=%zu too small for any response", max_packet);
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }
    size_t max_payload = max_packet - CIP_RESP_HDR_SIZE - 2;

    if(max_payload < (size_t)svc_count * 6) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "handle_multi: max_payload=%zu too small for %u sub-responses (need %zu)",
              max_payload, (unsigned)svc_count, (size_t)svc_count * 6);
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    Bytes *sub_responses = (Bytes *)arena_alloc(a, (size_t)svc_count * sizeof(Bytes));
    uint16_t *response_offsets = (uint16_t *)arena_alloc(a, (size_t)svc_count * sizeof(uint16_t));
    if(!sub_responses || !response_offsets) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "handle_multi: arena alloc failed for response arrays (%u entries)",
              (unsigned)svc_count);
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    /* running_offset: from the service_count word, starts after the offset table. */
    size_t running_offset = 2 + (size_t)svc_count * 2;

    for(uint16_t i = 0; i < svc_count; i++) {
        uint16_t req_off = req_offsets[i];
        uint16_t next_off = (i + 1 < svc_count) ? req_offsets[i + 1] : (uint16_t)svc_payload.len;

        if(req_off >= svc_payload.len || next_off > svc_payload.len || req_off >= next_off) {
            pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "handle_multi: sub-request %u has invalid offsets [%u,%u) payload_len=%zu",
                  (unsigned)i, (unsigned)req_off, (unsigned)next_off, svc_payload.len);
            return cip_error(a, svc, CIP_ERR_INVALID_PARAM, false, 0);
        }

        size_t remaining_entries = (size_t)(svc_count - i);
        size_t cap = max_payload - remaining_entries * 6;

        Bytes sub_req = bytes_slice(svc_payload, req_off, (size_t)(next_off - req_off));
        Bytes sub_resp;
        if(cap < CIP_RESP_HDR_SIZE) {
            /* Budget exhausted — 0x06 response with no data. */
            uint8_t sub_svc = (sub_req.len > 0) ? sub_req.data[0] : 0;
            sub_resp = cip_error(a, sub_svc, CIP_ERR_FRAG, false, 0);
        } else {
            sub_resp = cip_dispatch_connected(a, sub_req, sess, cfg, cap);
            if(bytes_is_null(sub_resp)) {
                uint8_t sub_svc = (sub_req.len > 0) ? sub_req.data[0] : 0;
                sub_resp = cip_error(a, sub_svc, CIP_ERR_FRAG, false, 0);
            }
        }

        response_offsets[i] = (uint16_t)running_offset;
        sub_responses[i] = sub_resp;
        running_offset += sub_resp.len;
        max_payload -= (2 + sub_resp.len);
    }

    /*
     * Build the entire response in one allocation.
     *
     * Layout: CIP hdr(4) | svc_count(2) | offset_table(2*N) | sub-responses
     */
    size_t resp_size = CIP_RESP_HDR_SIZE + running_offset;
    Bytes result = bytes_alloc(a, resp_size);
    if(bytes_is_null(result)) {
        pdlog(LOG_MODULE_CIP, LOG_LEVEL_WARN, "handle_multi: arena alloc failed for %zu-byte response", resp_size);
        return cip_error(a, svc, CIP_ERR_INSUF_DATA, false, 0);
    }

    Bytes rest = bytes_pack_into(result, BYTES_LE, (uint8_t)(svc | CIP_DONE), (uint8_t)0, CIP_OK, (uint8_t)0, svc_count);

    rest = bytes_pack_into(rest, BYTES_LE, BYTES_ARRAY(response_offsets, (size_t)svc_count));

    for(uint16_t i = 0; i < svc_count; i++) {
        if(sub_responses[i].len > 0) {
            memcpy(rest.data, sub_responses[i].data, sub_responses[i].len);
            rest = bytes_slice(rest, sub_responses[i].len, rest.len - sub_responses[i].len);
        }
    }

    return result;
}
