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
 * An owned, growable byte buffer: the header and the bytes are one allocation.
 *
 * Contract:
 *
 *   - A NULL buffer is empty: byte_buffer_len() is 0 and byte_buffer_data() is NULL.
 *   - len is the data in use; capacity is what the allocation holds.  len <= capacity.
 *   - byte_buffer_set_len() keeps the capacity when shrinking.  When growing past the
 *     capacity it doubles, adding at most BYTE_BUFFER_MAX_STEP bytes at a time, and never
 *     less than the length asked for.  Bytes between the old and the new length are zero.
 *   - On failure the buffer is unchanged.
 *   - Growing may move the buffer, so never keep a pointer into the data across a resize.
 */

#include <stdint.h>
#include <string.h>

#include <libplctag/api/libplctag.h>
#include <platform.h>


#define BYTE_BUFFER_MAX_STEP (2048)


typedef struct {
    int32_t capacity;
    int32_t len;
    uint8_t data[];
} byte_buffer_t;


static inline int32_t byte_buffer_len(const byte_buffer_t *buf) { return buf ? buf->len : 0; }

static inline uint8_t *byte_buffer_data(byte_buffer_t *buf) { return buf ? buf->data : NULL; }


static inline int32_t byte_buffer_set_len(byte_buffer_t **buf, int32_t len) {
    int32_t old_len = byte_buffer_len(*buf);
    int64_t capacity = (*buf) ? (*buf)->capacity : 0;
    byte_buffer_t *new_buf = *buf;

    if(len < 0) { return PLCTAG_ERR_BAD_PARAM; }

    if(len > capacity) {
        int64_t step = (capacity < BYTE_BUFFER_MAX_STEP) ? capacity : BYTE_BUFFER_MAX_STEP;

        capacity = (capacity + step < len) ? len : capacity + step;

        if(capacity > (int64_t)INT32_MAX - (int64_t)sizeof(byte_buffer_t)) { return PLCTAG_ERR_TOO_LARGE; }

        new_buf = (byte_buffer_t *)mem_realloc(*buf, (int32_t)(sizeof(byte_buffer_t) + (size_t)capacity));
        if(!new_buf) { return PLCTAG_ERR_NO_MEM; }

        new_buf->capacity = (int32_t)capacity;
    }

    /* an empty buffer set to length 0 stays NULL. */
    if(!new_buf) { return PLCTAG_STATUS_OK; }

    if(len > old_len) { memset(new_buf->data + old_len, 0, (size_t)(len - old_len)); }

    new_buf->len = len;
    *buf = new_buf;

    return PLCTAG_STATUS_OK;
}


static inline void byte_buffer_free(byte_buffer_t **buf) {
    mem_free(*buf);
    *buf = NULL;
}
