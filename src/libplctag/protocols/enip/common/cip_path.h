#pragma once

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
 * cip_path.c — CIP logical-path segment codec. Direction-agnostic: no
 * device_t, no eip_session_t, no I/O. Single source of truth for the
 * "1-byte tag, optional pad, N-byte value" logical segment grammar (class
 * 0x20/0x21/0x22, instance 0x24/0x25/0x26, attribute 0x30/0x31, element
 * index 0x28/0x29/0x2A) that was previously hand-rolled at 5 parse sites
 * and 3 encode sites across common/cip.c, client/enip_cip.c,
 * dialects/rockwell/ab_listing.c, and dialects/omron/omron_listing.c.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "utils/arena.h"
#include "utils/bytes.h"

typedef struct {
    uint32_t class_id;
    uint32_t instance_id;
    uint32_t attr_id;
    bool has_class;
    bool has_instance;
    bool has_attr;
} cip_path_ids_t;

/*
 * Parse a run of class/instance/(optional)attribute logical segments in any
 * combination of 8/16/32-bit widths (class: 0x20/21/22, instance:
 * 0x24/25/26, attribute: 0x30/31 -- no 32-bit attribute segment exists in
 * CIP). Returns true once at least a class and instance segment were seen;
 * *out is zeroed first.
 */
extern bool cip_path_parse(Bytes path, cip_path_ids_t *out);

/*
 * Parse a run of element-index logical segments (0x28/0x29/0x2A, "array
 * index" widths) from the front of `path`. Stops at the first byte that
 * isn't one of these three segment types and returns the unconsumed
 * remainder in *rest_out (bytes_null()'s .len is 0 when fully consumed) --
 * the caller decides whether leftover bytes are an error. Returns false
 * only on a malformed (truncated) segment or exceeding max_idx.
 */
extern bool cip_path_parse_indexes(Bytes path, uint32_t *num_idx_out, uint32_t *indexes, uint32_t max_idx,
                                   Bytes *rest_out);

/*
 * Encode one class + instance + (optional) attribute logical path.
 * attr_id < 0 means "no attribute segment". Class is encoded in the
 * narrowest width that fits (8/16/32-bit); instance is encoded 16-bit if it
 * fits, else 32-bit (never 8-bit -- matches every existing call site's
 * wire bytes, all of which assumed at least 16-bit instance encoding, and
 * is what the pre-existing OMRON >0xFFFF member-id special case already
 * did); attribute (if present) is encoded 8-bit if it fits, else 16-bit.
 * Optionally prefixed by an already-encoded path (e.g. a symbolic segment),
 * which must already be an even number of bytes. Returns bytes_null() on
 * arena exhaustion or a misaligned (odd-length) prefix.
 */
extern Bytes cip_path_encode(Arena *a, Bytes prefix, uint32_t class_id, uint32_t instance_id, int64_t attr_id);

/* Encode one array-index element segment (0x28/0x29/0x2A) into buf[buf_size],
 * narrowest width that fits. Returns bytes written, or 0 if buf_size is too
 * small. Buffer-write (not arena) form: used while building a path
 * incrementally into a caller-owned scratch buffer. */
extern size_t cip_path_encode_index_into(uint32_t index, uint8_t *buf, size_t buf_size);
