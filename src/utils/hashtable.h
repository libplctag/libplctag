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

/* Open addressed hash table keyed by non-zero, well mixed int64_t.  See hashtable.c. */

#include <stdint.h>


typedef struct hashtable_t *hashtable_p;


extern hashtable_p hashtable_create(int32_t initial_capacity);
extern void *hashtable_get(hashtable_p table, int64_t key);
extern int32_t hashtable_put(hashtable_p table, int64_t key, void *data);
extern void *hashtable_remove(hashtable_p table, int64_t key);
extern int32_t hashtable_destroy(hashtable_p table);
/* Entry walk: indexes 0..hashtable_capacity()-1, NULL for empty entries.  Caller holds the table's lock. */
extern int32_t hashtable_capacity(hashtable_p table);
extern void *hashtable_get_index(hashtable_p table, int32_t index);
