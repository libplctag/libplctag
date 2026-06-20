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
 * Adapted from src/poc/ab_server_fiber/arena.c.
 * Changes: util_err_t → int32_t; malloc/memset/free → mem_alloc/mem_set/mem_free.
 */

#include <stdint.h>

#include <libplctag/lib/libplctag.h>
#include "platform.h"
#include "arena.h"

/* Portable max-align substitute for older MSVC. */
typedef union {
    char c; short s; int i; long l; long long ll;
    float f; double d; long double ld; void *p;
} arena_max_align_t;


extern void arena_set_stats(Arena *a, ArenaStats *stats) {
    if(!a) { return; }
    a->stats = stats;
    if(stats) {
        mem_set(stats, 0, (int)sizeof(*stats));
        stats->use_min = (size_t)-1;
    }
}


extern int32_t arena_init(Arena *out, size_t size) {
    if(!out) { return PLCTAG_ERR_BAD_PARAM; }

    out->buffer = (uint8_t *)mem_alloc((int)size);
    if(!out->buffer) {
        out->length   = 0;
        out->capacity = 0;
        out->high_water = 0;
        return PLCTAG_ERR_NO_MEM;
    }

    out->length     = 0;
    out->capacity   = size;
    out->high_water = 0;
    out->stats      = NULL;

    return PLCTAG_STATUS_OK;
}


extern void *arena_alloc(Arena *a, size_t size) {
    if(!a || !a->buffer) { return NULL; }

    size_t align   = _Alignof(arena_max_align_t);
    size_t padding = (align - (a->length % align)) % align;

    if(a->length + padding + size > a->capacity) { return NULL; }

    a->length += padding;
    void *ptr  = a->buffer + a->length;
    a->length += size;

    if(a->length > a->high_water) { a->high_water = a->length; }

    return ptr;
}


extern uint8_t *arena_current(Arena *a) {
    if(!a || !a->buffer) { return NULL; }
    return a->buffer + a->length;
}


extern size_t arena_remaining(Arena *a) {
    if(!a || !a->buffer) { return 0; }
    return a->capacity - a->length;
}


extern void arena_commit(Arena *a, size_t n) {
    if(!a) { return; }
    a->length += n;
    if(a->length > a->high_water) { a->high_water = a->length; }
}


extern void arena_reset(Arena *a) {
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


extern size_t arena_save(Arena *a) { return a ? a->length : 0; }


extern void arena_restore(Arena *a, size_t saved) {
    if(a && saved <= a->capacity) { a->length = saved; }
}


extern void arena_free(Arena *a) {
    if(!a) { return; }
    mem_free(a->buffer);
    a->buffer     = NULL;
    a->length     = 0;
    a->capacity   = 0;
    a->high_water = 0;
}
