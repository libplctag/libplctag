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
 * Adapted from src/poc/ab_server_fiber/bytes.c.
 * Changes: memcpy → mem_copy; memset → mem_set.
 * Dead-code blocks (#if 0 ... #endif) are preserved but not shown here
 * since they were already excluded from the original's compilation.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>   /* strlen */

#include "platform.h"
#include "bytes.h"


Bytes bytes_alloc(Arena *a, size_t len) {
    void *mem = arena_alloc(a, len);
    if(!mem) { return (Bytes){NULL, 0}; }
    return (Bytes){(uint8_t *)mem, len};
}


void bytes_zero(Bytes b) {
    if(b.data) { mem_set(b.data, 0, (int)b.len); }
}


Bytes bytes_concat_impl(Arena *a, int count, ...) {
    va_list args;
    size_t total_len = 0;

    va_start(args, count);
    for(int i = 0; i < count; i++) {
        Bytes b = va_arg(args, Bytes);
        total_len += b.len;
    }
    va_end(args);

    uint8_t *mem = (uint8_t *)arena_alloc(a, total_len);
    if(!mem) { return (Bytes){NULL, 0}; }

    size_t offset = 0;
    va_start(args, count);
    for(int i = 0; i < count; i++) {
        Bytes b = va_arg(args, Bytes);
        if(b.data) { mem_copy(mem + offset, b.data, (int)b.len); }
        offset += b.len;
    }
    va_end(args);

    return (Bytes){mem, total_len};
}


/* ============================================================================
 * Type-safe pack implementation
 * ============================================================================ */

static size_t write_typed_args(uint8_t *dst, size_t cap, int endian, va_list args);

Bytes bytes_pack_impl(Arena *a, int endian, ...) {
    uint8_t *dst = arena_current(a);
    size_t   cap = arena_remaining(a);

    va_list args;
    va_start(args, endian);
    size_t written = write_typed_args(dst, cap, endian, args);
    va_end(args);

    if(written == SIZE_MAX) { return (Bytes){NULL, 0}; }
    arena_commit(a, written);
    return (Bytes){dst, written};
}


Bytes bytes_pack_into_impl(Bytes buf, int endian, ...) {
    if(!buf.data) { return (Bytes){NULL, 0}; }

    va_list args;
    va_start(args, endian);
    size_t written = write_typed_args(buf.data, buf.len, endian, args);
    va_end(args);

    if(written == SIZE_MAX) { return (Bytes){NULL, 0}; }
    return bytes_slice(buf, written, buf.len - written);
}


/* ============================================================================
 * Slicing
 * ============================================================================ */

Bytes bytes_from_buf(const uint8_t *buf, size_t len) { return (Bytes){(uint8_t *)buf, len}; }


Bytes bytes_slice(Bytes b, size_t offset, size_t len) {
    if(!b.data || offset + len > b.len) { return (Bytes){NULL, 0}; }
    return (Bytes){b.data + offset, len};
}


Bytes bytes_pad_even(Arena *a, Bytes b) {
    if(b.len % 2 == 0) { return b; }
    Bytes pad = bytes_alloc(a, b.len + 1);
    if(bytes_is_null(pad)) { return (Bytes){NULL, 0}; }
    mem_copy(pad.data, b.data, (int)b.len);
    pad.data[b.len] = 0x00;
    return pad;
}


/* ============================================================================
 * Type-safe unpack implementation
 * ============================================================================ */

static size_t read_typed_args(const uint8_t *src, size_t len, int endian, va_list args);

Bytes bytes_unpack_impl(Bytes data, int endian, ...) {
    if(!data.data) { return (Bytes){NULL, 0}; }

    va_list args;
    va_start(args, endian);
    size_t consumed = read_typed_args(data.data, data.len, endian, args);
    va_end(args);

    if(consumed == SIZE_MAX) { return (Bytes){NULL, 0}; }
    return bytes_slice(data, consumed, data.len - consumed);
}


/* ============================================================================
 * Host ↔ wire byte-order helpers — implemented as bit-shift to avoid
 * alignment issues; no memcpy needed here.
 * ============================================================================ */

static inline uint16_t u16h_to_le(uint16_t v) {
    uint16_t r; uint8_t *b = (uint8_t *)&r;
    b[0]=(uint8_t)v; b[1]=(uint8_t)(v>>8); return r;
}
static inline uint16_t u16h_to_be(uint16_t v) {
    uint16_t r; uint8_t *b = (uint8_t *)&r;
    b[0]=(uint8_t)(v>>8); b[1]=(uint8_t)v; return r;
}
static inline uint32_t u32h_to_le(uint32_t v) {
    uint32_t r; uint8_t *b = (uint8_t *)&r;
    b[0]=(uint8_t)v; b[1]=(uint8_t)(v>>8); b[2]=(uint8_t)(v>>16); b[3]=(uint8_t)(v>>24); return r;
}
static inline uint32_t u32h_to_be(uint32_t v) {
    uint32_t r; uint8_t *b = (uint8_t *)&r;
    b[0]=(uint8_t)(v>>24); b[1]=(uint8_t)(v>>16); b[2]=(uint8_t)(v>>8); b[3]=(uint8_t)v; return r;
}
static inline uint64_t u64h_to_le(uint64_t v) {
    uint64_t r; uint8_t *b = (uint8_t *)&r;
    b[0]=(uint8_t)v;       b[1]=(uint8_t)(v>>8);  b[2]=(uint8_t)(v>>16); b[3]=(uint8_t)(v>>24);
    b[4]=(uint8_t)(v>>32); b[5]=(uint8_t)(v>>40); b[6]=(uint8_t)(v>>48); b[7]=(uint8_t)(v>>56); return r;
}
static inline uint64_t u64h_to_be(uint64_t v) {
    uint64_t r; uint8_t *b = (uint8_t *)&r;
    b[0]=(uint8_t)(v>>56); b[1]=(uint8_t)(v>>48); b[2]=(uint8_t)(v>>40); b[3]=(uint8_t)(v>>32);
    b[4]=(uint8_t)(v>>24); b[5]=(uint8_t)(v>>16); b[6]=(uint8_t)(v>>8);  b[7]=(uint8_t)v; return r;
}
static inline uint16_t u16le_to_host(uint16_t v) {
    const uint8_t *b = (const uint8_t *)&v;
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1]<<8));
}
static inline uint16_t u16be_to_host(uint16_t v) {
    const uint8_t *b = (const uint8_t *)&v;
    return (uint16_t)(((uint16_t)b[0]<<8) | (uint16_t)b[1]);
}
static inline uint32_t u32le_to_host(uint32_t v) {
    const uint8_t *b = (const uint8_t *)&v;
    return (uint32_t)b[0] | ((uint32_t)b[1]<<8) | ((uint32_t)b[2]<<16) | ((uint32_t)b[3]<<24);
}
static inline uint32_t u32be_to_host(uint32_t v) {
    const uint8_t *b = (const uint8_t *)&v;
    return ((uint32_t)b[0]<<24) | ((uint32_t)b[1]<<16) | ((uint32_t)b[2]<<8) | (uint32_t)b[3];
}
static inline uint64_t u64le_to_host(uint64_t v) {
    const uint8_t *b = (const uint8_t *)&v;
    return  (uint64_t)b[0]        | ((uint64_t)b[1]<<8)  | ((uint64_t)b[2]<<16) | ((uint64_t)b[3]<<24)
          | ((uint64_t)b[4]<<32)  | ((uint64_t)b[5]<<40) | ((uint64_t)b[6]<<48) | ((uint64_t)b[7]<<56);
}
static inline uint64_t u64be_to_host(uint64_t v) {
    const uint8_t *b = (const uint8_t *)&v;
    return ((uint64_t)b[0]<<56) | ((uint64_t)b[1]<<48) | ((uint64_t)b[2]<<40) | ((uint64_t)b[3]<<32)
          | ((uint64_t)b[4]<<24) | ((uint64_t)b[5]<<16) | ((uint64_t)b[6]<<8)  | (uint64_t)b[7];
}

static size_t pack_type_elem_size(BytesPackType t) {
    switch(t) {
        case BYTES_TYPE_U8:  case BYTES_TYPE_I8:  return 1;
        case BYTES_TYPE_U16: case BYTES_TYPE_I16: return 2;
        case BYTES_TYPE_U32: case BYTES_TYPE_I32: case BYTES_TYPE_F32: return 4;
        case BYTES_TYPE_U64: case BYTES_TYPE_I64: case BYTES_TYPE_F64: return 8;
        default: return 0;
    }
}


static size_t write_typed_args(uint8_t *dst, size_t cap, int endian, va_list args) {
    size_t off = 0;

    for(;;) {
        int tag = va_arg(args, int);
        if(tag == (int)BYTES_TYPE_END) { break; }

        switch((BytesPackType)tag) {
            case BYTES_TYPE_U8: {
                if(off + 1 > cap) { return SIZE_MAX; }
                dst[off++] = (uint8_t)va_arg(args, unsigned int);
                break;
            }
            case BYTES_TYPE_I8: {
                if(off + 1 > cap) { return SIZE_MAX; }
                dst[off++] = (uint8_t)(int8_t)va_arg(args, int);
                break;
            }
            case BYTES_TYPE_U16:
            case BYTES_TYPE_I16: {
                if(off + 2 > cap) { return SIZE_MAX; }
                uint16_t v = (uint16_t)va_arg(args, unsigned int);
                if(endian == (int)BYTES_LE) { dst[off]=(uint8_t)v; dst[off+1]=(uint8_t)(v>>8); }
                else                        { dst[off]=(uint8_t)(v>>8); dst[off+1]=(uint8_t)v; }
                off += 2;
                break;
            }
            case BYTES_TYPE_U32:
            case BYTES_TYPE_I32: {
                if(off + 4 > cap) { return SIZE_MAX; }
                uint32_t v = va_arg(args, uint32_t);
                if(endian == (int)BYTES_LE) {
                    dst[off]=(uint8_t)v; dst[off+1]=(uint8_t)(v>>8);
                    dst[off+2]=(uint8_t)(v>>16); dst[off+3]=(uint8_t)(v>>24);
                } else {
                    dst[off]=(uint8_t)(v>>24); dst[off+1]=(uint8_t)(v>>16);
                    dst[off+2]=(uint8_t)(v>>8); dst[off+3]=(uint8_t)v;
                }
                off += 4;
                break;
            }
            case BYTES_TYPE_U64:
            case BYTES_TYPE_I64: {
                if(off + 8 > cap) { return SIZE_MAX; }
                uint64_t v = va_arg(args, uint64_t);
                if(endian == (int)BYTES_LE) {
                    for(size_t i = 0; i < 8; i++) { dst[off+i] = (uint8_t)(v >> (i*8)); }
                } else {
                    for(size_t i = 0; i < 8; i++) { dst[off+i] = (uint8_t)(v >> ((7-i)*8)); }
                }
                off += 8;
                break;
            }
            case BYTES_TYPE_F32: {
                if(off + 4 > cap) { return SIZE_MAX; }
                double   d = va_arg(args, double);
                float    f = (float)d;
                uint32_t bits;
                mem_copy(&bits, &f, 4);
                if(endian == (int)BYTES_LE) {
                    dst[off]=(uint8_t)bits; dst[off+1]=(uint8_t)(bits>>8);
                    dst[off+2]=(uint8_t)(bits>>16); dst[off+3]=(uint8_t)(bits>>24);
                } else {
                    dst[off]=(uint8_t)(bits>>24); dst[off+1]=(uint8_t)(bits>>16);
                    dst[off+2]=(uint8_t)(bits>>8); dst[off+3]=(uint8_t)bits;
                }
                off += 4;
                break;
            }
            case BYTES_TYPE_F64: {
                if(off + 8 > cap) { return SIZE_MAX; }
                double   d = va_arg(args, double);
                uint64_t bits;
                mem_copy(&bits, &d, 8);
                if(endian == (int)BYTES_LE) {
                    for(size_t i = 0; i < 8; i++) { dst[off+i] = (uint8_t)(bits >> (i*8)); }
                } else {
                    for(size_t i = 0; i < 8; i++) { dst[off+i] = (uint8_t)(bits >> ((7-i)*8)); }
                }
                off += 8;
                break;
            }
            case BYTES_TYPE_BYTES: {
                Bytes b = va_arg(args, Bytes);
                if(b.data && b.len > 0) {
                    if(off + b.len > cap) { return SIZE_MAX; }
                    mem_copy(dst + off, b.data, (int)b.len);
                    off += b.len;
                }
                break;
            }
            case BYTES_TYPE_SKIP: {
                BytesSkip *s = va_arg(args, BytesSkip *);
                if(off + s->count > cap) { return SIZE_MAX; }
                mem_set(dst + off, 0, (int)s->count);
                off += s->count;
                break;
            }
            case BYTES_TYPE_ARRAY: {
                BytesArray *ba = va_arg(args, BytesArray *);
                if(!ba->data || ba->count == 0) { break; }
                size_t esz = pack_type_elem_size(ba->elem_type);
                if(esz == 0 || off + ba->count * esz > cap) { return SIZE_MAX; }
                int le = (endian == (int)BYTES_LE);
                for(size_t i = 0; i < ba->count; i++) {
                    const uint8_t *elem = (const uint8_t *)ba->data + i * esz;
                    switch(ba->elem_type) {
                        case BYTES_TYPE_U8: case BYTES_TYPE_I8:
                            dst[off] = *elem; break;
                        case BYTES_TYPE_U16: case BYTES_TYPE_I16: {
                            uint16_t t; mem_copy(&t, (void *)elem, 2);
                            uint16_t w = le ? u16h_to_le(t) : u16h_to_be(t);
                            mem_copy(dst + off, &w, 2); break;
                        }
                        case BYTES_TYPE_U32: case BYTES_TYPE_I32: case BYTES_TYPE_F32: {
                            uint32_t t; mem_copy(&t, (void *)elem, 4);
                            uint32_t w = le ? u32h_to_le(t) : u32h_to_be(t);
                            mem_copy(dst + off, &w, 4); break;
                        }
                        case BYTES_TYPE_U64: case BYTES_TYPE_I64: case BYTES_TYPE_F64: {
                            uint64_t t; mem_copy(&t, (void *)elem, 8);
                            uint64_t w = le ? u64h_to_le(t) : u64h_to_be(t);
                            mem_copy(dst + off, &w, 8); break;
                        }
                        default: break;
                    }
                    off += esz;
                }
                break;
            }
            default: break;
        }
    }
    return off;
}


static size_t read_typed_args(const uint8_t *src, size_t len, int endian, va_list args) {
    size_t off = 0;

    for(;;) {
        int   tag = va_arg(args, int);
        if(tag == (int)BYTES_TYPE_END) { break; }
        void *ptr = va_arg(args, void *);

        switch((BytesPackType)tag) {
            case BYTES_TYPE_U8: {
                if(off + 1 > len) { return SIZE_MAX; }
                *(uint8_t *)ptr = src[off++]; break;
            }
            case BYTES_TYPE_I8: {
                if(off + 1 > len) { return SIZE_MAX; }
                *(int8_t *)ptr = (int8_t)src[off++]; break;
            }
            case BYTES_TYPE_U16: {
                if(off + 2 > len) { return SIZE_MAX; }
                *(uint16_t *)ptr = (endian == (int)BYTES_LE)
                    ? (uint16_t)((uint16_t)src[off] | ((uint16_t)src[off+1]<<8))
                    : (uint16_t)(((uint16_t)src[off]<<8) | (uint16_t)src[off+1]);
                off += 2; break;
            }
            case BYTES_TYPE_I16: {
                if(off + 2 > len) { return SIZE_MAX; }
                uint16_t v = (endian == (int)BYTES_LE)
                    ? (uint16_t)((uint16_t)src[off] | ((uint16_t)src[off+1]<<8))
                    : (uint16_t)(((uint16_t)src[off]<<8) | (uint16_t)src[off+1]);
                *(int16_t *)ptr = (int16_t)v; off += 2; break;
            }
            case BYTES_TYPE_U32: {
                if(off + 4 > len) { return SIZE_MAX; }
                *(uint32_t *)ptr = (endian == (int)BYTES_LE)
                    ? ((uint32_t)src[off] | ((uint32_t)src[off+1]<<8) | ((uint32_t)src[off+2]<<16) | ((uint32_t)src[off+3]<<24))
                    : (((uint32_t)src[off]<<24) | ((uint32_t)src[off+1]<<16) | ((uint32_t)src[off+2]<<8) | (uint32_t)src[off+3]);
                off += 4; break;
            }
            case BYTES_TYPE_I32: {
                if(off + 4 > len) { return SIZE_MAX; }
                uint32_t v = (endian == (int)BYTES_LE)
                    ? ((uint32_t)src[off] | ((uint32_t)src[off+1]<<8) | ((uint32_t)src[off+2]<<16) | ((uint32_t)src[off+3]<<24))
                    : (((uint32_t)src[off]<<24) | ((uint32_t)src[off+1]<<16) | ((uint32_t)src[off+2]<<8) | (uint32_t)src[off+3]);
                *(int32_t *)ptr = (int32_t)v; off += 4; break;
            }
            case BYTES_TYPE_U64: {
                if(off + 8 > len) { return SIZE_MAX; }
                uint64_t v = 0;
                if(endian == (int)BYTES_LE) {
                    for(size_t i = 0; i < 8; i++) { v |= ((uint64_t)src[off+i] << (i*8)); }
                } else {
                    for(size_t i = 0; i < 8; i++) { v |= ((uint64_t)src[off+i] << ((7-i)*8)); }
                }
                *(uint64_t *)ptr = v; off += 8; break;
            }
            case BYTES_TYPE_I64: {
                if(off + 8 > len) { return SIZE_MAX; }
                uint64_t v = 0;
                if(endian == (int)BYTES_LE) {
                    for(size_t i = 0; i < 8; i++) { v |= ((uint64_t)src[off+i] << (i*8)); }
                } else {
                    for(size_t i = 0; i < 8; i++) { v |= ((uint64_t)src[off+i] << ((7-i)*8)); }
                }
                *(int64_t *)ptr = (int64_t)v; off += 8; break;
            }
            case BYTES_TYPE_F32: {
                if(off + 4 > len) { return SIZE_MAX; }
                uint32_t bits = (endian == (int)BYTES_LE)
                    ? ((uint32_t)src[off] | ((uint32_t)src[off+1]<<8) | ((uint32_t)src[off+2]<<16) | ((uint32_t)src[off+3]<<24))
                    : (((uint32_t)src[off]<<24) | ((uint32_t)src[off+1]<<16) | ((uint32_t)src[off+2]<<8) | (uint32_t)src[off+3]);
                mem_copy(ptr, &bits, 4); off += 4; break;
            }
            case BYTES_TYPE_F64: {
                if(off + 8 > len) { return SIZE_MAX; }
                uint64_t bits = 0;
                if(endian == (int)BYTES_LE) {
                    for(size_t i = 0; i < 8; i++) { bits |= ((uint64_t)src[off+i] << (i*8)); }
                } else {
                    for(size_t i = 0; i < 8; i++) { bits |= ((uint64_t)src[off+i] << ((7-i)*8)); }
                }
                mem_copy(ptr, &bits, 8); off += 8; break;
            }
            case BYTES_TYPE_BYTES: {
                Bytes *bp = (Bytes *)ptr;
                if(off + bp->len > len) { return SIZE_MAX; }
                bp->data = (uint8_t *)src + off;
                off += bp->len; break;
            }
            case BYTES_TYPE_SKIP: {
                BytesSkip *s = (BytesSkip *)ptr;
                if(off + s->count > len) { return SIZE_MAX; }
                off += s->count; break;
            }
            case BYTES_TYPE_ARRAY: {
                BytesArray *bap = (BytesArray *)ptr;
                if(!bap->data || bap->count == 0) { break; }
                size_t esz = pack_type_elem_size(bap->elem_type);
                if(esz == 0 || off + bap->count * esz > len) { return SIZE_MAX; }
                int le = (endian == (int)BYTES_LE);
                for(size_t i = 0; i < bap->count; i++) {
                    const uint8_t *src_elem = src + off;
                    uint8_t *dst_elem = (uint8_t *)bap->data + i * esz;
                    switch(bap->elem_type) {
                        case BYTES_TYPE_U8: case BYTES_TYPE_I8:
                            *dst_elem = *src_elem; break;
                        case BYTES_TYPE_U16: case BYTES_TYPE_I16: {
                            uint16_t t; mem_copy(&t, (void *)src_elem, 2);
                            uint16_t h = le ? u16le_to_host(t) : u16be_to_host(t);
                            mem_copy(dst_elem, &h, 2); break;
                        }
                        case BYTES_TYPE_U32: case BYTES_TYPE_I32: case BYTES_TYPE_F32: {
                            uint32_t t; mem_copy(&t, (void *)src_elem, 4);
                            uint32_t h = le ? u32le_to_host(t) : u32be_to_host(t);
                            mem_copy(dst_elem, &h, 4); break;
                        }
                        case BYTES_TYPE_U64: case BYTES_TYPE_I64: case BYTES_TYPE_F64: {
                            uint64_t t; mem_copy(&t, (void *)src_elem, 8);
                            uint64_t h = le ? u64le_to_host(t) : u64be_to_host(t);
                            mem_copy(dst_elem, &h, 8); break;
                        }
                        default: break;
                    }
                    off += esz;
                }
                break;
            }
            default: break;
        }
    }
    return off;
}


void bytes_hexdump(Bytes b, const char *label) {
    fprintf(stderr, "\n%s (%zu bytes):\n", label ? label : "", b.len);
    for(size_t i = 0; i < b.len; i += 16) {
        fprintf(stderr, "  ");
        size_t chunk = (b.len - i < 16) ? (b.len - i) : 16;
        for(size_t j = 0; j < chunk; j++) {
            fprintf(stderr, "%02X", b.data[i + j]);
            if(j < chunk - 1) { fprintf(stderr, " "); }
        }
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}
