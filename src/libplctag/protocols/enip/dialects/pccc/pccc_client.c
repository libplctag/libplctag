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
 * pccc_client.c — PCCC client dialect: PLC-5 / SLC500 / MicroLogix
 * (Execute-PCCC, CIP service 0x4B). The CIP request rides the same
 * connected CPF+EIP wrap as the symbolic dialect; build/apply just speak
 * PCCC. Reuses client/enip_pccc_addr.c's pure address encoders. Wire
 * constants shared with the server half in dialects/pccc/pccc_defs.h (3.d).
 * Moved out of enip_session.c.
 */

#include <libplctag/protocols/enip/client/enip_cip.h>
#include <libplctag/protocols/enip/client/enip_connection_internal.h>
#include <libplctag/protocols/enip/client/enip_dialect.h>
#include <libplctag/protocols/enip/client/enip_pccc_addr.h>
#include "pccc_defs.h"

/* In a parsed reply.data: requestor id (7) + PCCC cmd(1)+sts(1)+tns(2) = 11. */
#define PCCC_REPLY_HDR ((size_t)11)
#define PCCC_REPLY_STS_OFF ((size_t)8)

/* Write the CIP/PCCC header + requestor id shared by every Execute-PCCC
 * request (13 bytes), then the PCCC command fields up to and including FNC.
 * Returns the unfilled remainder of dest (bytes_null() if dest was too
 * small), so callers thread it as their write cursor for the rest of the
 * request. */
static Bytes pccc_write_header(Bytes dest, uint16_t tns, uint8_t fnc) {
    return bytes_pack_into(dest, BYTES_LE,
                           (uint8_t)PCCC_EXECUTE_SVC, (uint8_t)0x02 /* path size in 16-bit words */,
                           (uint8_t)0x20, (uint8_t)0x67, (uint8_t)0x24, (uint8_t)0x01 /* PCCC object 0x67 inst 1 */,
                           (uint8_t)0x07 /* requestor id size = vendor_id(2) + serial(4) + this byte */,
                           (uint16_t)PCCC_VENDOR_ID, (uint32_t)PCCC_VENDOR_SN,
                           (uint8_t)PCCC_TYPED_CMD, (uint8_t)0x00, tns, fnc);
}

/* PLC-5 masked bit write (Execute-PCCC function 0x26, "Protected Typed Logical
 * Read/Write with mask"): AND-mask/OR-mask pair, one byte per element byte.
 * The remote never learns the current value of the word -- only the target
 * bit's byte gets a non-0xFF/0x00 mask entry -- so unrelated bits are never
 * clobbered. Mirrors ab/pccc.c:plc5_tag_write_bit_start; server side is
 * dialects/pccc/pccc.c:handle_plc5_rmw. */
static Bytes enip_pccc_build_plc5_bit_write(enip_connection_t *c, enip_tag_p t, Bytes dest) {
    uint8_t addr_buf[32];
    pccc_addr_t addr = t->pccc_addr;
    Bytes encoded = enip_pccc_encode_plc5_address(&addr, bytes_from_buf(addr_buf, sizeof(addr_buf)));
    if(bytes_is_null(encoded) || encoded.len == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to encode PCCC logical address!");
        return bytes_null();
    }

    Bytes rest = pccc_write_header(dest, (uint16_t)(c->conn_seq + 1), PCCC_PLC5_RMW_FNC);
    rest = bytes_pack_into(rest, BYTES_LE, encoded);
    if(bytes_is_null(rest)) { return bytes_null(); }

    size_t byte_idx = (size_t)(t->bit / 8);
    uint8_t bit_mask = (uint8_t)(1u << (t->bit % 8));
    bool bit_set = (t->data[byte_idx] & bit_mask) != 0;

    for(uint32_t i = 0; i < t->elem_size && !bytes_is_null(rest); i++) {
        uint8_t and_mask = ((size_t)i == byte_idx) ? (bit_set ? (uint8_t)0xFF : (uint8_t)~bit_mask) : (uint8_t)0xFF;
        rest = bytes_pack_into(rest, BYTES_LE, and_mask);
    }
    for(uint32_t i = 0; i < t->elem_size && !bytes_is_null(rest); i++) {
        uint8_t or_mask = ((size_t)i == byte_idx) ? (bit_set ? bit_mask : (uint8_t)0x00) : (uint8_t)0x00;
        rest = bytes_pack_into(rest, BYTES_LE, or_mask);
    }
    if(bytes_is_null(rest)) { return bytes_null(); }

    return bytes_from_buf(dest.data, dest.len - rest.len);
}

/* SLC/MicroLogix masked bit write (Execute-PCCC function 0xAB, "SLC Range
 * Write with mask"): a single 16-bit mask/set pair -- the mask is transmitted
 * as 2 bytes regardless of element size, so this only applies to 2-byte (B/N)
 * data files. A 32-bit L-file bit is not maskable this way (matches AB: real
 * hardware and run_enip_tests.sh's MicroLogix L-bit-write test both expect
 * failure). Mirrors ab/pccc.c:slc_tag_write_bit_start; server side is
 * dialects/pccc/pccc.c:handle_slc_rmw. */
static Bytes enip_pccc_build_slc_bit_write(enip_connection_t *c, enip_tag_p t, Bytes dest) {
    if(t->elem_size != 2 || t->size != 2) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id,
               "SLC/MicroLogix masked bit write requires a 2-byte element (mask is 16 bits); got %u bytes.",
               (unsigned)t->elem_size);
        return bytes_null();
    }

    uint8_t addr_buf[32];
    pccc_addr_t addr = t->pccc_addr;
    Bytes encoded = enip_pccc_encode_slc_address(&addr, bytes_from_buf(addr_buf, sizeof(addr_buf)));
    if(bytes_is_null(encoded) || encoded.len == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to encode PCCC logical address!");
        return bytes_null();
    }

    Bytes rest = pccc_write_header(dest, (uint16_t)(c->conn_seq + 1), PCCC_SLC_RMW_FNC);
    rest = bytes_pack_into(rest, BYTES_LE, (uint8_t)t->size /* transfer size in bytes, fixed at 2 */, encoded);
    if(bytes_is_null(rest)) { return bytes_null(); }

    uint8_t mask[2] = {0, 0};
    mask[t->bit / 8] = (uint8_t)(1u << (t->bit % 8));

    /* set bytes: only the masked bit is honored remotely, so t->data's other
     * bits (whatever they happen to hold locally) are harmless. */
    rest = bytes_pack_into(rest, BYTES_LE, bytes_from_buf(mask, sizeof(mask)), bytes_from_buf(t->data, 2));
    if(bytes_is_null(rest)) { return bytes_null(); }

    return bytes_from_buf(dest.data, dest.len - rest.len);
}

static Bytes enip_pccc_build(enip_connection_t *c, enip_tag_p t, Bytes dest) {
    if(t->op != ENIP_OP_READ && t->op != ENIP_OP_WRITE) { return bytes_null(); }

    /* A single-bit write must go out as a masked RMW, never a plain word
     * write: t->data only ever holds the bit this tag cares about (it is
     * never populated by a real read of the sibling bits), so overwriting the
     * whole word would clobber them on real hardware. Bit reads need no
     * special handling -- they fall through to the plain word read below and
     * the generic plc_tag_get_bit() extracts the bit locally. */
    if(t->is_bit && t->op == ENIP_OP_WRITE) {
        return t->pccc_plc5 ? enip_pccc_build_plc5_bit_write(c, t, dest) : enip_pccc_build_slc_bit_write(c, t, dest);
    }

    bool is_write = (t->op == ENIP_OP_WRITE);

    /* Chunk to <=PCCC_MAX_TRANSFER_BYTES per round trip: PCCC has no
     * CIP_STATUS_FRAG equivalent, so a large array tag is walked one element
     * chunk at a time via the element cursor t->read_off (reset to 0 for a
     * fresh op in enip_tag_read/write), each chunk addressed by advancing the
     * PCCC logical address's element field -- see apply() below for the other
     * half of the loop. */
    uint32_t remaining_elems = t->elem_count - t->read_off;
    uint32_t max_elems = (uint32_t)(PCCC_MAX_TRANSFER_BYTES / (size_t)t->elem_size);
    if(max_elems == 0) { max_elems = 1; }
    uint32_t chunk_elems = (remaining_elems < max_elems) ? remaining_elems : max_elems;
    size_t chunk_bytes = (size_t)chunk_elems * (size_t)t->elem_size;
    size_t data_len = is_write ? chunk_bytes : 0;

    /* Encode the logical address with the family encoder (copy: it may adjust), advanced to this chunk's starting element. */
    uint8_t addr_buf[32];
    pccc_addr_t addr = t->pccc_addr;
    addr.element += (int32_t)t->read_off;
    Bytes encoded = t->pccc_plc5 ? enip_pccc_encode_plc5_address(&addr, bytes_from_buf(addr_buf, sizeof(addr_buf)))
                                 : enip_pccc_encode_slc_address(&addr, bytes_from_buf(addr_buf, sizeof(addr_buf)));
    if(bytes_is_null(encoded) || encoded.len == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to encode PCCC logical address!");
        return bytes_null();
    }

    uint16_t tns = (uint16_t)(c->conn_seq + 1);
    uint8_t fnc = t->pccc_plc5 ? (is_write ? PCCC_PLC5_WRITE_FNC : PCCC_PLC5_READ_FNC)
                               : (is_write ? PCCC_SLC_WRITE_FNC : PCCC_SLC_READ_FNC);
    Bytes rest = pccc_write_header(dest, tns, fnc);

    if(t->pccc_plc5) {
        uint16_t words = (uint16_t)(chunk_bytes / 2u); /* transfer size in words, this chunk only */
        rest = bytes_pack_into(rest, BYTES_LE, (uint16_t)0x0000 /* offset=0: each chunk uses its own address instead */,
                               words, encoded);
        if(!is_write && !bytes_is_null(rest)) {
            rest = bytes_pack_into(rest, BYTES_LE, (uint8_t)chunk_bytes); /* PLC-5 read appends this chunk's byte size */
        }
    } else {
        rest = bytes_pack_into(rest, BYTES_LE, (uint8_t)chunk_bytes /* transfer size in bytes, this chunk only (<=240) */,
                               encoded);
    }
    if(bytes_is_null(rest)) { return bytes_null(); }

    if(is_write) {
        rest = bytes_pack_into(rest, BYTES_LE, bytes_from_buf(t->data + (size_t)t->read_off * (size_t)t->elem_size, data_len));
        if(bytes_is_null(rest)) { return bytes_null(); }
    }

    return bytes_from_buf(dest.data, dest.len - rest.len);
}

static int32_t enip_pccc_apply(enip_connection_t *c, enip_tag_p t, Bytes cip_reply, bool *more) {
    (void)c;
    *more = false;

    cip_reply_t reply;
    if(!enip_cip_parse_reply(cip_reply, &reply)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to parse PCCC CIP reply!");
        return PLCTAG_ERR_BAD_REPLY;
    }

    if(reply.status != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "PCCC CIP error 0x%02X (ext 0x%04X).", reply.status, reply.ext_status);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    if(reply.data.len < PCCC_REPLY_HDR) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "PCCC reply too short (%zu bytes)!", reply.data.len);
        return PLCTAG_ERR_BAD_REPLY;
    }

    /* PCCC-level STS. 0xF0 carries an extended code in the next byte; treating
     * any non-zero as a remote error is enough for the data path. */
    uint8_t pccc_sts = reply.data.data[PCCC_REPLY_STS_OFF];
    if(pccc_sts != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "PCCC status error 0x%02X.", pccc_sts);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* Same chunk-size formula as build() above, recomputed from the cursor
     * build() used for this round trip (unchanged since then -- one round
     * trip in flight at a time). */
    uint32_t remaining_elems = t->elem_count - t->read_off;
    uint32_t max_elems = (uint32_t)(PCCC_MAX_TRANSFER_BYTES / (size_t)t->elem_size);
    if(max_elems == 0) { max_elems = 1; }
    uint32_t chunk_elems = (remaining_elems < max_elems) ? remaining_elems : max_elems;

    if(t->op == ENIP_OP_WRITE) {
        t->read_off += chunk_elems;
        if(t->read_off < t->elem_count) { *more = true; }
        return PLCTAG_STATUS_OK;
    }

    size_t avail = reply.data.len - PCCC_REPLY_HDR;
    size_t chunk_bytes = (size_t)chunk_elems * (size_t)t->elem_size;
    size_t n = (avail < chunk_bytes) ? avail : chunk_bytes;
    bytes_pack_into(bytes_from_buf(t->data + (size_t)t->read_off * (size_t)t->elem_size, n), BYTES_LE,
                   bytes_from_buf(reply.data.data + PCCC_REPLY_HDR, n));
    t->read_off += (uint32_t)(n / (size_t)t->elem_size);
    if(t->read_off < t->elem_count) { *more = true; }
    return PLCTAG_STATUS_OK;
}

/* PCCC "@tags" listing (File 0 system directory read). Per a vendor protocol
 * technical report (not independently verified against real PLC-5/SLC/
 * MicroLogix hardware in this tree): PCCC has no symbol-object equivalent to
 * Logix's class 0x6B, but every PCCC family exposes a fixed-width directory
 * record per data file in "File 0", readable with the ordinary word-range
 * read used for real data -- PLC-5's 2-address Word Range Read (FNC 0x01),
 * or SLC/MicroLogix's 3-address Typed Logical Read (FNC 0xA2) addressing
 * File 0 as an Integer file (0x89, the type code File 0 reports as). This
 * treats MicroLogix identically to SLC (6-byte records); the report also
 * describes an extended 8-byte record for specific newer MicroLogix variants
 * (e.g. 1400-series) that this does not distinguish or support -- a known
 * limitation, not a silent misread, since enip_plc_type_t has no MicroLogix
 * sub-family (see plc_type.h).
 *
 * Termination is inferred, not signaled: PCCC has no partial-transfer status
 * (unlike Logix's CIP_STATUS_FRAG), so apply_listing below infers "more"
 * purely from whether the reply came back exactly full. Both platforms'
 * record sizes (4 bytes/PLC-5, 6 bytes/SLC+MicroLogix) divide evenly into a
 * fixed 240-byte page, so one page size works for both with no per-platform
 * rounding, and it fits under the single-byte transfer-size field both
 * platforms' read wire format shares (max 255) -- see build below.
 *
 * t->list_next_id (reset to 0 by enip_listing_tag_read) is the next 16-bit
 * WORD offset into File 0 -- shared with Logix's own @tags use of the same
 * LISTING-kind tag (enip_dialect_t's listing seam, enip_dialect.h); the
 * accumulated raw byte length in t->data is t->size itself (0.1: no separate
 * cursor field). The raw
 * buffer is bare concatenated native records, no per-record framing (matches
 * ENIP-METADATA-AND-DISCOVERY-DESIGN.md's raw-is-canonical model); parsed
 * access is via plc_tag_get_formatted_data(tag, PLCTAG_FORMAT_CBOR, ...)
 * (enip_tag.c), which knows the record layout from the connection's
 * identity-classified PLC family. */

static Bytes enip_pccc_build_listing(Arena *a, enip_tag_p t) {
    if(t->op != ENIP_OP_LIST) { return bytes_null(); }

    enip_connection_t *c = t->conn;
    bool is_plc5 = (c->plc_type == ENIP_PLC_PLC5);

    /* File 0 has no name (it is the system directory itself); the address
     * encoders below need only file/file_type/element/sub_element, not a
     * parsed pccc_addr_t from a tag name -- built directly, not from
     * t->pccc_addr (LISTING-kind tags don't have that union member). */
    pccc_addr_t addr = {0};
    addr.file = 0;
    addr.file_type = PCCC_FILE_INT; /* File 0 reports as Integer-typed on SLC/MicroLogix; unused by the PLC-5 2-address encoder */
    addr.element = (int32_t)t->list_next_id;
    addr.sub_element = -1;

    uint8_t addr_buf[32];
    Bytes encoded = is_plc5 ? enip_pccc_encode_plc5_address(&addr, bytes_from_buf(addr_buf, sizeof(addr_buf)))
                            : enip_pccc_encode_slc_address(&addr, bytes_from_buf(addr_buf, sizeof(addr_buf)));
    if(bytes_is_null(encoded) || encoded.len == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to encode PCCC File 0 address!");
        return bytes_null();
    }

    size_t need = 13 /* CIP/PCCC header + requestor id, see pccc_write_header */ + (is_plc5 ? 5u : 1u) + encoded.len;
    Bytes dest = bytes_alloc(a, need);
    if(bytes_is_null(dest)) { return bytes_null(); }

    Bytes rest = pccc_write_header(dest, (uint16_t)(c->conn_seq + 1), is_plc5 ? PCCC_PLC5_READ_FNC : PCCC_SLC_READ_FNC);

    if(is_plc5) {
        rest = bytes_pack_into(rest, BYTES_LE,
                               (uint16_t)0x0000 /* byte offset = 0: whole-word range read, not a fragmented single element */,
                               PCCC_MAX_TRANSFER_WORDS, encoded);
        if(!bytes_is_null(rest)) {
            rest = bytes_pack_into(rest, BYTES_LE,
                                   (uint8_t)PCCC_MAX_TRANSFER_BYTES); /* PLC-5 read appends total byte size (fits: 240<=255) */
        }
    } else {
        rest = bytes_pack_into(rest, BYTES_LE, (uint8_t)PCCC_MAX_TRANSFER_BYTES /* transfer size in bytes (fits: 240<=255) */,
                               encoded);
    }
    if(bytes_is_null(rest)) { return bytes_null(); }

    return bytes_from_buf(dest.data, dest.len - rest.len);
}

static int32_t enip_pccc_apply_listing(enip_tag_p t, uint8_t cip_status, Bytes data, bool *more) {
    (void)cip_status; /* PCCC replies carry no CIP-level partial-transfer status */
    *more = false;

    if(data.len < PCCC_REPLY_HDR) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "PCCC File 0 reply too short (%zu bytes)!", data.len);
        return PLCTAG_ERR_BAD_REPLY;
    }

    uint8_t pccc_sts = data.data[PCCC_REPLY_STS_OFF];
    if(pccc_sts != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "PCCC File 0 read status error 0x%02X.", pccc_sts);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    size_t payload_len = data.len - PCCC_REPLY_HDR;

    if(payload_len > 0) {
        Bytes payload = bytes_from_buf(data.data + PCCC_REPLY_HDR, payload_len);
        if(!tag_data_append((plc_tag_p)t, &t->buf_cap, payload)) { return PLCTAG_ERR_NO_MEM; }
        t->list_next_id += (uint32_t)(payload_len / 2); /* advance the word cursor */
    }

    /* No PCCC partial-transfer signal exists (unlike Logix's CIP_STATUS_FRAG):
     * a full page means "maybe more"; a short page is the last one. */
    if(payload_len >= PCCC_MAX_TRANSFER_BYTES) { *more = true; }

    return PLCTAG_STATUS_OK;
}

const enip_dialect_t enip_pccc_dialect = {
    .name = "pccc",
    .requested_cip_size = 0,
    .max_batch_cap = 1, /* single in-flight: no Multiple Service (0x0A) packing */
    .build = enip_pccc_build,
    .apply = enip_pccc_apply,
    .build_listing = enip_pccc_build_listing,
    .apply_listing = enip_pccc_apply_listing,
};
