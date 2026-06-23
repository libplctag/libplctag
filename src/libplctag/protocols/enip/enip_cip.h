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
 * CIP requests and parsing (design doc §14.5).
 *
 * This header grows incrementally as the MVP build order (§14.10) proceeds.
 * Currently implemented: enip_cip_encode_path, enip_cip_encode_route,
 * enip_cip_read, enip_cip_parse_reply.
 */

#include <stdbool.h>
#include <stdint.h>
#include <utils/arena.h>
#include <utils/bytes.h>

/* CIP service codes */
#define CIP_READ        ((uint8_t)0x4C)
#define CIP_WRITE       ((uint8_t)0x4D)
#define CIP_READ_FRAG   ((uint8_t)0x52)
#define CIP_FWD_OPEN    ((uint8_t)0x54)
#define CIP_FWD_OPEN_LG ((uint8_t)0x5B)
#define CIP_FWD_CLOSE   ((uint8_t)0x4E)
#define CIP_UNCONN_SEND ((uint8_t)0x52) /* Connection Manager, same code as ReadFrag */
#define CIP_MULTI_SVC   ((uint8_t)0x0A) /* Multiple Service Packet */
#define CIP_LIST_TAGS   ((uint8_t)0x55) /* Get_Instance_Attribute_List (tag/symbol listing) */
#define CIP_GET_ATTR_LIST ((uint8_t)0x03) /* Get_Attribute_List (UDT template metadata) */

/* CIP general status: partial transfer ("too much data"); reissue from the
 * next instance id / byte offset until a non-FRAG status is returned. */
#define CIP_STATUS_FRAG ((uint8_t)0x06)

/* service+reserved+status+ext_size, present in every CIP reply (§11.3) */
#define CIP_READ_REPLY_OVERHEAD ((size_t)4)

/* Connected Data Item header (enip_cpf_wrap_connected): item type(2) +
 * item length(2) + connection sequence count(2) = 6 bytes. The negotiated
 * ForwardOpen connection size (conn->max_cip_packet_size) bounds this whole
 * item, so the CIP reply (CIP_READ_REPLY_OVERHEAD + header_len + elem_size)
 * capacity is conn->max_cip_packet_size - CIP_CONNECTED_ITEM_OVERHEAD;
 * window/size calculations (§11.3) must subtract this term. */
#define CIP_CONNECTED_ITEM_OVERHEAD ((size_t)6)

/* service+path_size_words+element_count, present in every CIP write request
 * (§11.5); mirrors CIP_READ_REPLY_OVERHEAD's shape but for the request side.
 * Unlike a read reply, a write request also carries the path and type header
 * inline, so window calculations must add those separately. */
#define CIP_WRITE_REQUEST_OVERHEAD ((size_t)4)

/* Fixed overhead of the Multi-Service CIP request payload (before the per-tag
 * offset table and sub-requests): service(1)+path_size_words(1)+MR path(4)+
 * request_count(2) = 8 bytes. */
#define ENIP_MS_REQ_FIXED ((size_t)8)

/* Fixed overhead of the Multi-Service CIP reply payload (outer CIP reply
 * header(4) + response_count(2) = 6 bytes, before the per-tag offset table
 * and sub-replies). */
#define ENIP_MS_RESP_FIXED ((size_t)6)

/* Minimum possible CIP sub-request size (used to compute max batch count):
 *   service(1) + path_size_words(1) + 0x91 segment type(1) + name_len(1)
 *   + 1-byte name(1) + element_count(2) + offset_table_entry(2) = 9 bytes.
 * Response minimum (4-byte CIP header + 2-byte offset) = 6 bytes; request
 * minimum is the binding constraint. */
#define ENIP_MS_MIN_SUB_REQ_SIZE ((size_t)9)

/* Parsed CIP reply header. */
typedef struct {
    uint8_t service;     /* reply service: request service | 0x80     */
    uint8_t status;      /* general status; 0 == success               */
    uint16_t ext_status; /* first extended-status word, or 0 if none   */
    Bytes data;          /* zero-copy slice of whatever follows         */
} cip_reply_t;

/*
 * Encode a tag name into an ANSI CIP Extended Symbol path (IOI).
 *
 * "Foo.Bar[3]" -> 0x91 symbolic segments (one per dot-separated name, each
 * padded to an even length) interleaved with 0x28/0x29/0x2A array-index
 * segments for each [N].  The result is always an even number of bytes
 * (word-aligned), ready to prefix with a path_size_words byte for the CIP
 * request.
 *
 * Returns the encoded path bytes (arena-allocated), or bytes_null() on
 * error (empty name, a segment over 255 bytes, or arena exhaustion).
 */
extern Bytes enip_cip_encode_path(Arena *a, const char *name);

/*
 * Append a CIP array-index segment (0x28/0x29/0x2A, per enip_cip_encode_path)
 * for `index` to `base_path`, for OPEN_BULK windowed reads (§11.2): the base
 * path has no embedded index, so `path = base[index]` is built by appending
 * one index segment.
 *
 * Returns bytes_null() on a null/empty base_path or arena exhaustion.
 */
extern Bytes enip_cip_encode_path_at(Arena *a, Bytes base_path, uint32_t index);

/*
 * Encode a backplane routing path from a comma-separated list of small
 * unsigned integers, e.g. "1,0" -> {0x01, 0x00}.  Each element becomes one
 * CIP port-segment byte pair (port number, link address); the result is
 * therefore always even-length (word-aligned).  Callers compute
 * path_size_words the same way as for enip_cip_encode_path (result.len / 2).
 *
 * Returns bytes_null() on a null/empty route, a malformed element, an
 * element out of range (0-255), or arena exhaustion.
 */
extern Bytes enip_cip_encode_route(Arena *a, const char *route);

/*
 * Encode a CIP ReadTag (0x4C) request: service(1) + path_size_words(1) +
 * path(N) + element_count(2).  `path` must already be the even-length
 * encoded IOI from enip_cip_encode_path().
 *
 * Returns bytes_null() on a null/empty/odd-length path or arena exhaustion.
 */
extern Bytes enip_cip_read(Arena *a, Bytes path, uint16_t count);

/*
 * Encode a CIP WriteTag (0x4D) request: service(1) + path_size_words(1) +
 * path(N) + type_header(type_header.len) + element_count(2) + data.
 * `path` must be the even-length encoded IOI; `type_header` is the 2- or
 * 4-byte type code learned at OPEN_PROBE and replayed verbatim (§11.5).
 *
 * Returns bytes_null() on a null/empty/odd-length path, a null/empty
 * type_header or data, or arena exhaustion.
 */
extern Bytes enip_cip_write(Arena *a, Bytes path, Bytes type_header, uint16_t count, Bytes data);

/*
 * Encode a tag/symbol listing request (CIP 0x55, Get_Instance_Attribute_List)
 * on the symbol class (0x6B) starting at `instance_id`.  `prefix` is an
 * optional already-encoded symbolic segment (e.g. a "Program:Foo" scope) and
 * may be bytes_null() for controller-scope listing.  Asks for attributes
 * 0x02 (symbol type), 0x07 (element size), 0x08 (array dims), 0x01 (name).
 * Continuation: reissue with instance_id = (highest returned id) + 1 while the
 * reply status is CIP_STATUS_FRAG.
 */
extern Bytes enip_cip_list_tags(Arena *a, Bytes prefix, uint16_t instance_id);

/*
 * Encode a UDT/template metadata request (CIP 0x03, Get_Attribute_List) on the
 * template class (0x6C) instance `udt_id`.  Asks for attributes 0x04 (field
 * definition size in 32-bit words), 0x05 (instance size in bytes), 0x02 (member
 * count), 0x01 (handle/type).
 */
extern Bytes enip_cip_udt_meta(Arena *a, uint16_t udt_id);

/*
 * Encode a UDT/template field-definition read (CIP 0x4C) on the template class
 * (0x6C) instance `udt_id`, reading `total` bytes starting at byte `offset`.
 * Reissue with offset advanced by the returned payload while the reply status
 * is CIP_STATUS_FRAG.
 */
extern Bytes enip_cip_udt_fields(Arena *a, uint16_t udt_id, uint32_t offset, uint16_t total);

/*
 * Split a CIP reply into its header fields and trailing data.
 *
 * Wire format: reply_service(1) reserved(1) general_status(1) ext_size(1)
 * + ext_size 16-bit LE extended-status words + data.
 *
 * Returns false if `in` is shorter than CIP_READ_REPLY_OVERHEAD or shorter
 * than the declared extended-status block.
 */
extern bool enip_cip_parse_reply(Bytes in, cip_reply_t *out);

/*
 * Build a CIP Multiple Service Packet (0x0A) request from N pre-built
 * sub-requests.  sub_reqs[0..count-1] must each be a complete CIP service
 * request (output of enip_cip_read / enip_cip_write).
 *
 * The offset table entries are relative to the start of the offset array
 * per the CIP spec.  Caller must have already verified that the total fits
 * within the negotiated CIP payload budget (ENIP_MS_REQ_FIXED + 2*count +
 * sum(sub_reqs[i].len) <= max_cip - CIP_CONNECTED_ITEM_OVERHEAD).
 *
 * Returns bytes_null() on zero count or arena exhaustion.
 */
extern Bytes enip_cip_multi_service(Arena *a, Bytes *sub_reqs, uint16_t count);

/*
 * Parse the `data` slice from enip_cip_parse_reply when the outer service is
 * CIP_MULTI_SVC | 0x80.  Fills sub_replies[0..*count_out-1] as zero-copy
 * slices of `data`; each slice is a complete CIP sub-reply to be parsed with
 * enip_cip_parse_reply.
 *
 * Returns false on a malformed packet or if the response count exceeds
 * max_count.
 */
extern bool enip_cip_parse_multi_service_reply(Bytes data, uint16_t *count_out,
                                               Bytes *sub_replies, uint16_t max_count);
