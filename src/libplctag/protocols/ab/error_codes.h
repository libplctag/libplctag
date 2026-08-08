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

#pragma once

#include <stddef.h>
#include <stdint.h>

extern const char *decode_cip_error_short(uint8_t *data, size_t data_size);
extern const char *decode_cip_error_long(uint8_t *data, size_t data_size);
extern int decode_cip_error_code(uint8_t *data, size_t data_size);

/*
 * Bytes remaining in a received buffer starting at data, given the buffer's end pointer.
 * data and buf_end must point into (or one past) the same buffer.  Returns 0 if data is at
 * or past buf_end -- e.g. a truncated response that did not even reach this field -- rather
 * than letting a negative pointer difference wrap around to a huge size_t.
 */
static inline size_t cip_error_data_size(uint8_t *data, uint8_t *buf_end) {
    return (data < buf_end) ? (size_t)(buf_end - data) : 0;
}
