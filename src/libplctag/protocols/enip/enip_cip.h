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
 * Shared CIP Operations (generic, all manufacturers)
 *
 * STATUS: KEEP AS-IS through Phase 3.  Phase 4 addition: add two new declarations
 *   for the OMRON service builders (enip_cip_read_tag_omron_request and
 *   enip_cip_write_tag_omron_request) used by the OMRON encode_chunk.
 *
 * Standard CIP services identical across manufacturers:
 *   ReadTag  (0x4C): service(1) + path_size_words(1) + path(N) + elem_count(2)
 *   WriteTag (0x4D): service(1) + path_size_words(1) + path(N) + data_type(2) + elem_count(2) + data
 *
 * AB-specific fragmented services (not generic):
 *   ReadTagFragmented  (0x52): adds byte_offset(4) after elem_count
 *   WriteTagFragmented (0x53): adds byte_offset(4) after elem_count, before data
 *
 * CIP response layout (all services):
 *   reply_service(1) + reserved(1) + general_status(1) + ext_status_size(1)
 *   + ext_status_words(2*ext_status_size) + data
 */

#include <stdint.h>
#include <stddef.h>
#include <utils/arena.h>
#include <utils/bytes.h>

/* CIP service codes */
#define CIP_SVC_READ_TAG              ((uint8_t)0x4C)
#define CIP_SVC_WRITE_TAG             ((uint8_t)0x4D)
#define CIP_SVC_READ_TAG_FRAGMENTED   ((uint8_t)0x52) /* AB-specific */
#define CIP_SVC_WRITE_TAG_FRAGMENTED  ((uint8_t)0x53) /* AB-specific */
#define CIP_SVC_GET_INSTANCE_ATTR_LIST ((uint8_t)0x55)

/* CIP general status codes */
#define CIP_STATUS_SUCCESS    ((uint8_t)0x00)
#define CIP_STATUS_PARTIAL    ((uint8_t)0x06) /* more data available */
#define CIP_STATUS_PATH_ERR   ((uint8_t)0x04)

/* Phase 2: used immediately for all tag path encoding.
 * Encode a tag path string into ANSI CIP Extended Symbol format.
 * Supports: simple names, array indices [N], member navigation .field.
 * Writes into buf[buf_size].
 * Returns bytes written, or 0 on error (name too long, buffer too small). */
extern size_t enip_cip_encode_tag_path(const char *tag_name, uint8_t *buf, size_t buf_size);

/* Phase 2: used immediately for single-packet reads (non-fragmented).
 * Build a CIP ReadTag (0x4C) request.
 * encoded_path: pre-encoded ANSI CIP path bytes (from enip_cip_encode_tag_path).
 * element_count: number of elements to read (must be >= 1).
 * Returns the complete CIP request or bytes_null() on error. */
extern Bytes enip_cip_read_tag_request(Arena *arena, const uint8_t *encoded_path,
                                       size_t encoded_path_len, uint16_t element_count);

/* Phase 2: used immediately for single-packet writes (non-fragmented).
 * Build a CIP WriteTag (0x4D) request.
 * data_type: CIP data type code (e.g., 0x00C4 for DINT, 0x00CA for REAL).
 * element_count: number of elements in write_data.
 * Returns the complete CIP request or bytes_null() on error. */
extern Bytes enip_cip_write_tag_request(Arena *arena, const uint8_t *encoded_path,
                                        size_t encoded_path_len, uint16_t data_type,
                                        uint16_t element_count, const uint8_t *write_data,
                                        size_t write_data_len);

/* Phase 4: used by AB encode_chunk for fragmented reads (byte_offset cursor).
 * Build an AB ReadTagFragmented (0x52) request.
 * byte_offset: byte position within the tag data to start reading. */
extern Bytes enip_cip_read_tag_fragmented_request(Arena *arena, const uint8_t *encoded_path,
                                                  size_t encoded_path_len, uint16_t element_count,
                                                  uint32_t byte_offset);

/* Phase 4: used by AB encode_chunk for fragmented writes.
 * Build an AB WriteTagFragmented (0x53) request.
 * byte_offset: byte position within the tag data to start writing. */
extern Bytes enip_cip_write_tag_fragmented_request(Arena *arena, const uint8_t *encoded_path,
                                                   size_t encoded_path_len, uint16_t data_type,
                                                   uint16_t element_count, uint32_t byte_offset,
                                                   const uint8_t *write_data,
                                                   size_t write_data_len);

/* Phase 1: used to parse every CIP response (session, identity, reads, writes).
 * Parse a CIP response header.
 * Extracts general_status and extended_status_size.
 * Sets *data_out to the payload that follows the status bytes.
 * Returns the same Bytes as *data_out, or bytes_null() on parse failure. */
extern Bytes enip_cip_parse_response(Bytes response, uint8_t *general_status,
                                     uint8_t *extended_status_size, Bytes *data_out);

/* Phase 4: ADD here — enip_cip_read_tag_omron_request(0x4C + 0x80 data segment). */
/* Phase 4: ADD here — enip_cip_write_tag_omron_request(0x4D + 0x80 data segment). */
