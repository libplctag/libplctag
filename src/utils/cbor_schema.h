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
 * Descriptor-driven CBOR map emitter (ENIP-UPDATES-PLAN.md item 3.c),
 * protocol-agnostic (item 3.5) -- built on utils/cbor.h's primitives. Three
 * read-only ENIP tag kinds (identity, PCCC file listing, UDP discovery) used
 * to each hand-write six functions (record_cbor_size/write,
 * envelope_cbor_size/write, schema_cbor_size/write) with every field name
 * spelled 3x. Here, each site normalizes its record into a small local
 * "view" struct -- uint fields at their natural width, text/bytes fields as
 * pointer+length pairs, an optional bool "present" flag for a field that
 * isn't always emitted -- and describes it with one cbor_field_t[] table;
 * cbor_record_size/cbor_emit_record then walk the table instead of a
 * hand-written pair. cbor_envelope_size/emit and cbor_schema_size/emit cover
 * the {"schema","schema-version","records":[...]} and
 * {"schema","schema-version","fields":[...]} wrapper shapes shared
 * verbatim by all three sites.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <utils/bytes.h>

typedef enum {
    CBOR_FIELD_U8,
    CBOR_FIELD_U16,
    CBOR_FIELD_U32,
    CBOR_FIELD_U64,
    CBOR_FIELD_TEXT,  /* view holds `const char *ptr` at offset, `size_t len` at len_offset */
    CBOR_FIELD_BYTES, /* view holds `const uint8_t *ptr` at offset, `size_t len` at len_offset */
} cbor_field_kind_t;

typedef struct {
    const char *name;
    cbor_field_kind_t kind;
    size_t offset;        /* offset in the view struct of the value (TEXT/BYTES: of the pointer) */
    size_t len_offset;     /* TEXT/BYTES only: offset of the matching size_t length field */
    ptrdiff_t present_offset; /* offset of a `bool` "emit this field" flag in the view; -1 = always present */
} cbor_field_t;

/*
 * Size/write one CBOR map for `view`, walking `fields`. A field whose
 * present_offset resolves to false is skipped entirely -- both from the
 * map's declared pair count and its bytes -- matching what hand-written code
 * that conditionally omits a key would produce.
 */
extern size_t cbor_record_size(const cbor_field_t *fields, size_t count, const void *view);
extern bool cbor_emit_record(Bytes dest, size_t *pos, const cbor_field_t *fields, size_t count, const void *view);

/*
 * Shared outer envelope: {"schema","schema-version","records":[...]}. Only
 * the map header + "records" array header are written here (definite-length
 * CBOR needs no closing token for either); the caller emits `record_count`
 * records immediately after, e.g. via cbor_emit_record per record.
 * records_bytes is the pre-summed size of those records (from
 * cbor_record_size), needed by cbor_envelope_size but not by the write side.
 */
extern size_t cbor_envelope_size(const char *schema_name, uint64_t schema_version, size_t record_count,
                                 size_t records_bytes);
extern bool cbor_emit_envelope(Bytes dest, size_t *pos, const char *schema_name, uint64_t schema_version,
                               size_t record_count);

/* Shared field-name reflection: {"schema","schema-version","fields":[...]}. */
extern size_t cbor_schema_size(const char *schema_name, uint64_t schema_version, const char *const *field_names,
                               size_t field_count);
extern bool cbor_emit_schema(Bytes dest, size_t *pos, const char *schema_name, uint64_t schema_version,
                             const char *const *field_names, size_t field_count);
