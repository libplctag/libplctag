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
 * Per-connection dialect seam (design doc §16a.4).
 *
 * The IO thread, scheduler, lifetime, framing, and the right-sized state
 * machine are common to every CIP device. The manufacturer differences live
 * behind a small vtable selected once per connection from the device's CIP
 * Identity reply during bring-up.  Two concerns actually diverge:
 *
 *   build  - encode the CIP request for a tag's current op (symbolic Read/Write
 *            for Logix/OMRON; Execute-PCCC for PLC-5/SLC/MicroLogix).
 *   apply  - parse one CIP sub-reply, interpret status (vendor-specific), copy
 *            into the tag buffer, and signal whether another round trip is due.
 *
 * The two numbers (requested_cip_size, max_batch_cap) feed existing arithmetic
 * rather than branching on vendor (Micro800 / PCCC set max_batch_cap = 1 to
 * route through the single-in-flight path; §16a.3).
 */

#include <stdbool.h>
#include <stdint.h>
#include <utils/arena.h>
#include <utils/bytes.h>
#include <libplctag/protocols/enip/common/plc_type.h>

typedef struct enip_connection_t enip_connection_t;
typedef struct enip_tag_t *enip_tag_p;

typedef struct enip_dialect_t {
    const char *name;

    size_t requested_cip_size; /* number -> ForwardOpen builder; 0 = engine default */
    uint16_t max_batch_cap;    /* number -> min() batch cap; 0 = no cap, 1 = no 0x0A */

    /* Encode the CIP request for t's current op into `dest`, a caller-owned
     * region sized to the remaining packet budget. Returns the used prefix of
     * dest, or bytes_null() if the encoded op does not fit dest.len (or on
     * error). The caller owns all buffering and 0x0A Multiple Service packing:
     * it advances its cursor by the returned length and packs the next
     * sub-request, or stops. Caller holds t->api_mutex. c->arena is available
     * as scratch (the caller resets it before the pack). */
    Bytes (*build)(enip_connection_t *c, enip_tag_p t, Bytes dest);

    /* Consume one already-CPF-unwrapped CIP reply: parse it, interpret status
     * (Rockwell 0x06 partial-transfer is meaningful only to its own dialect),
     * copy into t->data, set *more for another round trip. Returns a
     * PLCTAG_STATUS or PLCTAG_ERR code. Caller holds t->api_mutex. */
    int32_t (*apply)(enip_connection_t *c, enip_tag_p t, Bytes cip_reply, bool *more);

    /* @tags/@udt enumeration (design doc §16a listing subsystem). NULL if this
     * dialect does not support listing -- build_tag_request rejects
     * ENIP_OP_LIST/UDT_META/UDT_FIELDS with PLCTAG_ERR_UNSUPPORTED when
     * build_listing is NULL, so a dialect that sets one of this pair must set
     * both. Rockwell and OMRON enumerate via different CIP classes/services
     * (ROCKWELL-SPECIFIC-DESIGN.md vs OMRON-SPECIFIC-DESIGN.md §5) with no
     * shared wire shape, so unlike `build`/`apply` these are never shared
     * verbatim between dialects.
     *
     * build_listing encodes t's current listing op into a fresh a-backed
     * allocation (unconnected-sized; wrapped in the same connected CPF as data
     * ops by the caller). apply_listing parses one already-CPF-unwrapped reply
     * (the CIP status/data past the reply header), accumulates into t->data,
     * and sets *more when another request is needed. Caller holds
     * t->api_mutex for both. */
    Bytes (*build_listing)(Arena *a, enip_tag_p t);
    int32_t (*apply_listing)(enip_tag_p t, uint8_t cip_status, Bytes data, bool *more);
} enip_dialect_t;

/* Logix/Micro800 symbolic dialect (CIP Read/Write Tag 0x4C/0x4D). The default
 * for every connection. */
extern const enip_dialect_t enip_logix_dialect;

/* OMRON NJ/NX symbolic dialect. Shares enip_logix_dialect's build/apply
 * verbatim (OMRON-SPECIFIC-DESIGN.md §1: same path encoding, same Read/Write
 * Tag services, same CIP Common Format reply framing) -- the only difference
 * is requested_cip_size (§2.2). Byte-fragment mode for oversized single
 * elements (§3) and tag/UDT enumeration (§5) are out of MVP scope. */
extern const enip_dialect_t enip_omron_dialect;

/* PLC-5 / SLC500 / MicroLogix PCCC dialect (Execute-PCCC, CIP service 0x4B).
 * Unlike the symbolic dialect this is selected per-tag (by ENIP_TAG_KIND_PCCC,
 * decided from the tag name at create) rather than per-connection from Identity:
 * PCCC addressing and the probe-less bring-up are known at tag create, and the
 * PLC-5-vs-SLC encoding split is not reliably derivable from CIP Identity. */
extern const enip_dialect_t enip_pccc_dialect;

/* Select the dialect for a connection from its classified PLC family (see
 * common/plc_classify.h). Called once at the end of identity bring-up, after
 * enip_classify_plc(). Never returns NULL. */
extern const enip_dialect_t *enip_dialect_select(enip_plc_type_t plc_type);
