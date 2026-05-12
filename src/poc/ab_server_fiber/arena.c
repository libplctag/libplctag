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
 * Fixed-size bump allocator implementation.
 * Copied from ~/Projects/data_table and modified to return errors instead
 * of calling exit().
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"

/* Portable substitute for max_align_t: a union of all fundamental types whose
 * alignment must be respected.  max_align_t from <stddef.h> is C11 but older
 * MSVC toolchains may not provide it even with /std:c11. */
typedef union {
    char c; short s; int i; long l; long long ll;
    float f; double d; long double ld; void *p;
} arena_max_align_t;

void arena_set_stats(Arena *a, ArenaStats *stats) {
    if(!a) { return; }
    a->stats = stats;
    if(stats) {
        memset(stats, 0, sizeof(*stats));
        stats->use_min = SIZE_MAX;
    }
}


util_err_t arena_init(Arena *out, size_t size) {
    if(!out) { return UTIL_EINVAL; }

    out->buffer = (uint8_t *)malloc(size);
    if(!out->buffer) {
        out->length = 0;
        out->capacity = 0;
        out->high_water = 0;
        return UTIL_ERESOURCE;
    }

    out->length = 0;
    out->capacity = size;
    out->high_water = 0;

    return UTIL_OK;
}


void *arena_alloc(Arena *a, size_t size) {
    if(!a || !a->buffer) { return NULL; }

    /* Align the cursor to the strictest fundamental alignment, matching malloc. */
    size_t align = _Alignof(arena_max_align_t);
    size_t padding = (align - (a->length % align)) % align;

    if(a->length + padding + size > a->capacity) { return NULL; }

    a->length += padding;
    void *ptr = a->buffer + a->length;
    a->length += size;

    if(a->length > a->high_water) { a->high_water = a->length; }

    return ptr;
}


void arena_reset(Arena *a) {
    if(!a) { return; }
    if(a->stats && a->length > 0) {
        ArenaStats *s = a->stats;
        s->reset_count++;
        if(a->length < s->use_min) { s->use_min = a->length; }
        if(a->length > s->use_max) { s->use_max = a->length; }
        s->use_total += a->length;
    }
    a->length = 0;
}


uint8_t *arena_current(Arena *a) {
    if(!a || !a->buffer) { return NULL; }
    return a->buffer + a->length;
}


size_t arena_remaining(Arena *a) {
    if(!a || !a->buffer) { return 0; }
    return a->capacity - a->length;
}


void arena_commit(Arena *a, size_t n) {
    if(!a) { return; }
    a->length += n;
    if(a->length > a->high_water) { a->high_water = a->length; }
}


size_t arena_save(Arena *a) { return a ? a->length : 0; }


void arena_restore(Arena *a, size_t saved) {
    if(a && saved <= a->capacity) { a->length = saved; }
}


void arena_free(Arena *a) {
    if(!a) { return; }

    free(a->buffer);
    a->buffer = NULL;
    a->length = 0;
    a->capacity = 0;
    a->high_water = 0;
}
