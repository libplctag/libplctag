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
 * Adapted from src/poc/ab_server_fiber/arena.h.
 * Changes: util_err_t → int32_t; removed err.h dependency.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Per-reset-cycle usage statistics.  Attach with arena_set_stats();
 * pass NULL to disable.  arena_reset() samples arena.length before clearing.
 */
typedef struct {
    size_t reset_count;
    size_t use_min;
    size_t use_max;
    size_t use_total;
} ArenaStats;

typedef struct {
    uint8_t   *buffer;
    size_t     length;
    size_t     capacity;
    size_t     high_water;
    ArenaStats *stats;
} Arena;

/* Returns PLCTAG_STATUS_OK or PLCTAG_ERR_NO_MEM. */
extern int32_t arena_init(Arena *out, size_t size);

extern void arena_set_stats(Arena *a, ArenaStats *stats);

/* Returns NULL on out-of-space; caller must check. */
extern void *arena_alloc(Arena *a, size_t size);

extern uint8_t *arena_current(Arena *a);
extern size_t   arena_remaining(Arena *a);
extern void     arena_commit(Arena *a, size_t n);
extern void     arena_reset(Arena *a);
extern size_t   arena_save(Arena *a);
extern void     arena_restore(Arena *a, size_t saved);
extern void     arena_free(Arena *a);
