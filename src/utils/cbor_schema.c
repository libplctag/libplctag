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

#include <string.h>

#include <utils/cbor.h>
#include <utils/cbor_schema.h>

static bool field_present(const cbor_field_t *f, const uint8_t *base) {
    return f->present_offset < 0 || *(const bool *)(base + (size_t)f->present_offset);
}

static uint64_t field_uint(const cbor_field_t *f, const uint8_t *base) {
    switch(f->kind) {
        case CBOR_FIELD_U8: return *(const uint8_t *)(base + f->offset);
        case CBOR_FIELD_U16: return *(const uint16_t *)(base + f->offset);
        case CBOR_FIELD_U32: return *(const uint32_t *)(base + f->offset);
        case CBOR_FIELD_U64: return *(const uint64_t *)(base + f->offset);
        default: return 0; /* unreachable: caller only calls this for UINT kinds */
    }
}

static void field_span(const cbor_field_t *f, const uint8_t *base, const void **ptr_out, size_t *len_out) {
    *ptr_out = *(void *const *)(base + f->offset);
    *len_out = *(const size_t *)(base + f->len_offset);
}

extern size_t cbor_record_size(const cbor_field_t *fields, size_t count, const void *view) {
    const uint8_t *base = (const uint8_t *)view;
    size_t present_count = 0;
    size_t sz = 0;

    for(size_t i = 0; i < count; i++) {
        const cbor_field_t *f = &fields[i];
        if(!field_present(f, base)) { continue; }
        present_count++;

        sz += cbor_size_text(strlen(f->name));

        if(f->kind == CBOR_FIELD_TEXT || f->kind == CBOR_FIELD_BYTES) {
            const void *ptr = NULL;
            size_t len = 0;
            field_span(f, base, &ptr, &len);
            sz += (f->kind == CBOR_FIELD_TEXT) ? cbor_size_text(len) : cbor_size_bytes(len);
        } else {
            sz += cbor_size_uint(field_uint(f, base));
        }
    }

    return cbor_size_map_header(present_count) + sz;
}

extern bool cbor_emit_record(Bytes dest, size_t *pos, const cbor_field_t *fields, size_t count, const void *view) {
    const uint8_t *base = (const uint8_t *)view;
    size_t present_count = 0;

    for(size_t i = 0; i < count; i++) {
        if(field_present(&fields[i], base)) { present_count++; }
    }

    if(!cbor_write_map_header(dest, pos, present_count)) { return false; }

    for(size_t i = 0; i < count; i++) {
        const cbor_field_t *f = &fields[i];
        if(!field_present(f, base)) { continue; }

        if(!cbor_write_text(dest, pos, f->name, strlen(f->name))) { return false; }

        if(f->kind == CBOR_FIELD_TEXT || f->kind == CBOR_FIELD_BYTES) {
            const void *ptr = NULL;
            size_t len = 0;
            field_span(f, base, &ptr, &len);
            bool ok = (f->kind == CBOR_FIELD_TEXT) ? cbor_write_text(dest, pos, (const char *)ptr, len)
                                                    : cbor_write_bytes(dest, pos, (const uint8_t *)ptr, len);
            if(!ok) { return false; }
        } else {
            if(!cbor_write_uint(dest, pos, field_uint(f, base))) { return false; }
        }
    }

    return true;
}

extern size_t cbor_envelope_size(const char *schema_name, uint64_t schema_version, size_t record_count,
                                 size_t records_bytes) {
    size_t sz = cbor_size_map_header(3);
    sz += cbor_size_text(strlen("schema")) + cbor_size_text(strlen(schema_name));
    sz += cbor_size_text(strlen("schema-version")) + cbor_size_uint(schema_version);
    sz += cbor_size_text(strlen("records")) + cbor_size_array_header(record_count);
    return sz + records_bytes;
}

extern bool cbor_emit_envelope(Bytes dest, size_t *pos, const char *schema_name, uint64_t schema_version,
                               size_t record_count) {
    if(!cbor_write_map_header(dest, pos, 3)) { return false; }
    if(!cbor_write_text(dest, pos, "schema", strlen("schema"))) { return false; }
    if(!cbor_write_text(dest, pos, schema_name, strlen(schema_name))) { return false; }
    if(!cbor_write_text(dest, pos, "schema-version", strlen("schema-version"))) { return false; }
    if(!cbor_write_uint(dest, pos, schema_version)) { return false; }
    if(!cbor_write_text(dest, pos, "records", strlen("records"))) { return false; }
    return cbor_write_array_header(dest, pos, record_count);
}

extern size_t cbor_schema_size(const char *schema_name, uint64_t schema_version, const char *const *field_names,
                               size_t field_count) {
    size_t sz = cbor_size_map_header(3);
    sz += cbor_size_text(strlen("schema")) + cbor_size_text(strlen(schema_name));
    sz += cbor_size_text(strlen("schema-version")) + cbor_size_uint(schema_version);
    sz += cbor_size_text(strlen("fields")) + cbor_size_array_header(field_count);
    for(size_t i = 0; i < field_count; i++) { sz += cbor_size_text(strlen(field_names[i])); }
    return sz;
}

extern bool cbor_emit_schema(Bytes dest, size_t *pos, const char *schema_name, uint64_t schema_version,
                             const char *const *field_names, size_t field_count) {
    if(!cbor_write_map_header(dest, pos, 3)) { return false; }
    if(!cbor_write_text(dest, pos, "schema", strlen("schema"))) { return false; }
    if(!cbor_write_text(dest, pos, schema_name, strlen(schema_name))) { return false; }
    if(!cbor_write_text(dest, pos, "schema-version", strlen("schema-version"))) { return false; }
    if(!cbor_write_uint(dest, pos, schema_version)) { return false; }
    if(!cbor_write_text(dest, pos, "fields", strlen("fields"))) { return false; }
    if(!cbor_write_array_header(dest, pos, field_count)) { return false; }
    for(size_t i = 0; i < field_count; i++) {
        if(!cbor_write_text(dest, pos, field_names[i], strlen(field_names[i]))) { return false; }
    }
    return true;
}
