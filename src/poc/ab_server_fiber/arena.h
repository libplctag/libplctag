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
 * Fixed-size bump allocator.  All allocations are sequential; the only
 * "free" is arena_reset() which resets the cursor to zero.
 *
 * Copied from ~/Projects/data_table and modified:
 *   - arena_init() returns util_err_t instead of panicking on malloc failure.
 *   - arena_alloc() returns NULL on overflow instead of calling exit().
 *   - arena_free() no longer prints stats to stderr.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "err.h"

/*
 * Per-reset-cycle usage statistics.  Attach to an arena with arena_set_stats();
 * pass NULL to disable collection entirely.  arena_reset() samples arena.length
 * just before clearing the cursor, so each sample equals the peak usage for
 * that request cycle (bump allocators never free, so length == peak).
 */
typedef struct {
    size_t reset_count;    /* number of non-empty reset cycles measured */
    size_t use_min;        /* minimum usage at reset time (bytes) */
    size_t use_max;        /* maximum usage at reset time (bytes) */
    size_t use_total;      /* sum of all samples (for average) */
} ArenaStats;

typedef struct {
    uint8_t *buffer;
    size_t length;
    size_t capacity;
    size_t high_water;   /* peak usage across all resets */
    ArenaStats *stats;   /* optional; NULL disables stats gathering */
} Arena;

/* Initialize arena with a fixed size.  Returns UTIL_ERESOURCE on malloc failure. */
extern util_err_t arena_init(Arena *out, size_t size);

/* Attach (or detach with NULL) a stats collector.  Clears the stats struct on attach. */
extern void arena_set_stats(Arena *a, ArenaStats *stats);

/* Allocate size bytes from arena.  Returns NULL if out of space; caller must check. */
extern void *arena_alloc(Arena *a, size_t size);

/* Pointer to next free byte (for single-pass pack-then-commit). */
extern uint8_t *arena_current(Arena *a);

/* Bytes remaining in arena. */
extern size_t arena_remaining(Arena *a);

/* Advance arena cursor by n bytes.  Caller must ensure n <= arena_remaining(). */
extern void arena_commit(Arena *a, size_t n);

/* Reset arena cursor to zero without freeing the backing buffer.
 * If stats are attached, samples arena.length before clearing. */
extern void arena_reset(Arena *a);

/* Save current cursor position. */
extern size_t arena_save(Arena *a);

/* Restore arena cursor to a previously saved position. */
extern void arena_restore(Arena *a, size_t saved);

/* Free arena backing buffer. */
extern void arena_free(Arena *a);

#ifdef __cplusplus
}
#endif
