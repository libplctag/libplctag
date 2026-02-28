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

#include <platform.h>
#include <stdint.h>

/*
 * Handle-based object management system
 *
 * Features:
 * - O(1) lookup by handle value (array index)
 * - Generation counters to prevent ABA reuse problems
 * - Automatic thread-safe access control via mutex
 * - Reference counting for proper lifetime management
 * - No circular dependencies or self-join deadlocks
 *
 * Handle Format (64-bit):
 *   [48-bit index | 16-bit generation]
 */

typedef uint64_t handle_t;

#define HANDLE_INVALID 0ULL
#define HANDLE_INDEX_BITS 48
#define HANDLE_GEN_BITS 16
#define HANDLE_INDEX_MASK ((1ULL << HANDLE_INDEX_BITS) - 1)
#define HANDLE_GEN_MASK ((1ULL << HANDLE_GEN_BITS) - 1)

/* Extract components from handle */
static inline uint64_t handle_index(handle_t h) { return h >> HANDLE_GEN_BITS; }

static inline uint16_t handle_gen(handle_t h) { return h & HANDLE_GEN_MASK; }

/* Create handle from index and generation */
static inline handle_t handle_make(uint64_t index, uint16_t gen) { return (index << HANDLE_GEN_BITS) | (gen & HANDLE_GEN_MASK); }

/* Destructor function type for cleanup when handle is destroyed */
typedef void (*handle_destructor_f)(void *);

/*
 * Initialize handle system (call once at library startup)
 * Returns: PLCTAG_STATUS_OK on success
 */
int handle_system_init(void);

/*
 * Allocate a new handle and return its value
 *
 * Args:
 *   data_size:  Size of user data to allocate (after metadata header)
 *   destructor: Optional cleanup function called when handle is destroyed (can be NULL)
 *
 * Returns:
 *   Valid handle_t value on success
 *   HANDLE_INVALID on failure (allocation failed)
 *
 * The returned data pointer is not accessible directly; use handle_acquire()
 * to gain access within a critical section.
 */
handle_t handle_alloc(size_t data_size, handle_destructor_f destructor);

/*
 * Acquire a handle for use (lookup + lock + refcount increment)
 *
 * Args:
 *   h:         The handle to acquire
 *   data_out:  Pointer to receive the user data pointer
 *
 * Returns:
 *   PLCTAG_STATUS_OK: Successfully acquired the handle
 *                     *data_out now points to user data
 *                     Caller MUST call handle_release() to unlock
 *
 *   PLCTAG_ERR_NOT_FOUND: Handle is invalid or has been destroyed
 *                         *data_out is set to NULL
 *                         Do NOT call handle_release()
 *
 *   PLCTAG_ERR_OBJECT_FREED: Handle is marked for destruction
 *                            *data_out is set to NULL
 *                            Do NOT call handle_release()
 *
 *   PLCTAG_ERR_NULL_PTR: data_out was NULL
 */
int handle_acquire(handle_t h, void **data_out);

/*
 * Release a handle after use (unlock and decrement refcount)
 *
 * Args:
 *   h: The handle to release (must have been successfully acquired)
 *
 * Note: Only call this if handle_acquire() returned PLCTAG_STATUS_OK
 */
void handle_release(handle_t h);

/*
 * Destroy a handle and free its resources
 *
 * Args:
 *   h: The handle to destroy
 *
 * Returns:
 *   PLCTAG_STATUS_OK: Handle marked for destruction successfully
 *                     Waits for all active references to be released
 *                     Calls destructor (if provided) and frees memory
 *
 *   PLCTAG_ERR_NOT_FOUND: Handle is invalid or already destroyed
 *
 * Note: This blocks until all active handle_acquire() calls have released
 *       the handle via handle_release()
 */
int handle_destroy(handle_t h);

/*
 * Wait for all handles to be destroyed (for module teardown)
 *
 * Blocks until all allocated handles have been destroyed via handle_destroy()
 * or a 5-second timeout occurs.
 *
 * Call this during library shutdown to ensure proper cleanup.
 */
void handle_system_teardown(void);

/*
 * Helper macros for scoped handle access
 */

/*
 * handle_scoped - Automatically acquire and release a handle
 *
 * Usage:
 *   modbus_plc_p plc;
 *   handle_scoped(tag->plc_handle, plc) {
 *       if(plc && !plc->flags.terminate) {
 *           mb_abort((plc_tag_p)tag);
 *       }
 *   }
 *
 * The handle is automatically released when leaving the block.
 * If the handle is invalid, plc will be NULL.
 */
#define handle_scoped(h, ptr_var)                                                                                   \
    for(int _h_acq = (handle_acquire((h), (void **)&(ptr_var))), _h_rel = 0; _h_acq == PLCTAG_STATUS_OK && !_h_rel; \
        _h_rel = 1, handle_release((h)))
