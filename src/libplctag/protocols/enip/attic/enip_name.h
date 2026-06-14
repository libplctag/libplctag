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
 * Tag Name Utilities
 *
 * STATUS: KEEP AS-IS for all phases.  No changes needed.
 *   enip_name_extract_root and enip_name_encode_path are used from Phase 2.
 *   enip_name_encode_route is wired in Phase 3 (currently never called).
 *
 * enip_name_extract_root -- pull the first path segment (before '[' or '.')
 *   Used to look up instance IDs in the Phase-1 root symbol cache.
 *
 * enip_name_encode_path -- encode a full tag path string into ANSI CIP bytes
 *   Delegates to enip_cip_encode_tag_path(); wrapper for call-site convenience.
 *
 * enip_name_encode_route -- encode a backplane/port route string into CIP
 *   port segment bytes.
 *
 *   Route string format:  "A,0" | "B,1" | "1,192.168.1.10"
 *     - Port letter: A (port 18) or B (port 19)
 *     - Port number: 1..15 as a decimal integer
 *     - Link address: decimal slot number, or dotted-decimal IP for Ethernet ports
 *
 *   Output is an even number of bytes (padded with 0x00 if needed) ready to
 *   append to a CIP path.  path_size_words = output_len / 2.
 */

#include <stddef.h>
#include <stdint.h>

/* Phase 2: call during tag creation to get the root name for symbol cache lookup.
 * Copy the root name (first segment before '[' or '.') from tag_path into buf.
 * buf is null-terminated on success.
 * Returns the number of characters written (not counting the null terminator),
 * or 0 if tag_path is empty or the root segment exceeds buf_size-1. */
extern size_t enip_name_extract_root(const char *tag_path, char *buf, size_t buf_size);

/* Phase 2: call during tag creation to pre-encode the tag path for I/O requests.
 * Encode tag_path into ANSI CIP Extended Symbol format in buf[buf_size].
 * Returns bytes written, or 0 on error.  Delegates to enip_cip_encode_tag_path(). */
extern size_t enip_name_encode_path(const char *tag_path, uint8_t *buf, size_t buf_size);

/* Phase 3: call from enip_connection_create with the "path" attribute (e.g. "1,4").
 * Store result in conn->conn_path / conn->conn_path_size (size in 16-bit words).
 * Encode a route string into CIP port segment bytes in buf[buf_size].
 * Returns bytes written (always even, padded if necessary), or 0 on error. */
extern size_t enip_name_encode_route(const char *route, uint8_t *buf, size_t buf_size);
