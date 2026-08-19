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
 * test_bytes -- covers ENIP-REDESIGN-PLAN.md D6/0.3: bytes_concat() must
 * absorb a null member instead of producing a non-null buffer with
 * uninitialized bytes in it.
 */

#include <assert.h>
#include <stdio.h>
#include <utils/arena.h>
#include <utils/bytes.h>

int main(void) {
    Arena a;
    assert(arena_init(&a, 256) == 0);

    uint8_t data[3] = {1, 2, 3};
    Bytes ok = bytes_from_buf(data, sizeof(data));

    Bytes both_ok = bytes_concat(&a, ok, ok);
    assert(!bytes_is_null(both_ok));
    assert(both_ok.len == 6);

    Bytes with_null = bytes_concat(&a, ok, bytes_null());
    assert(bytes_is_null(with_null));

    Bytes null_first = bytes_concat(&a, bytes_null(), ok);
    assert(bytes_is_null(null_first));

    arena_free(&a);

    printf("test_bytes: OK\n");
    return 0;
}
