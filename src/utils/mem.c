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
 * Memory allocation and block operations.  See utils/mem.h.
 *
 * The two platform shims carried the same code here, differing only in a cast
 * spelling and a few comments, so there is nothing to select between and no
 * #ifdef in this file.
 *
 * The (size_t)(uint32_t) casts are for -Wconversion, which is on in every
 * build.  Each one sits behind a check that has already proven the size is
 * positive.
 */

#include <stdlib.h>
#include <string.h>

#include <libplctag/lib/libplctag.h>
#include <utils/debug.h>
#include <utils/mem.h>


/*
 * mem_alloc
 *
 * This is a wrapper around the platform's memory allocation routine.
 * It will zero out memory before returning it.
 *
 * It will return NULL on failure.
 */
extern void *mem_alloc(int32_t size) {
    if(size <= 0) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Allocation size must be greater than zero bytes!");
        return NULL;
    }

    return calloc((size_t)(uint32_t)size, 1);
}


/*
 * mem_realloc
 *
 * This is a wrapper around the platform's memory re-allocation routine.
 *
 * It will return NULL on failure.
 */
extern void *mem_realloc(void *orig, int32_t size) {
    if(size <= 0) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "New allocation size must be greater than zero bytes!");
        return NULL;
    }

    return realloc(orig, (size_t)(uint32_t)size);
}


/*
 * mem_free
 *
 * Free the allocated memory passed in.  If the passed pointer is
 * null, do nothing.
 */
extern void mem_free(const void *mem) {
    if(mem) { free((void *)mem); }
}


/*
 * mem_set
 *
 * set memory to the passed argument.
 */
extern void mem_set(void *dest, int32_t c, int32_t size) {
    if(!dest) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Destination pointer is NULL!");
        return;
    }

    if(size <= 0) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Size to set must be a positive number!");
        return;
    }

    // NOLINTNEXTLINE
    memset(dest, c, (size_t)(uint32_t)size);
}


/*
 * mem_copy
 *
 * copy memory from one pointer to another for the passed number of bytes.
 */
extern void mem_copy(void *dest, void *src, int32_t size) {
    if(!dest) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Destination pointer is NULL!");
        return;
    }

    if(!src) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Source pointer is NULL!");
        return;
    }

    if(size < 0) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Size to copy must be a positive number!");
        return;
    }

    if(size == 0) {
        /* nothing to do. */
        return;
    }

    // NOLINTNEXTLINE
    memcpy(dest, src, (size_t)(uint32_t)size);
}


/*
 * mem_move
 *
 * move memory from one pointer to another for the passed number of bytes.
 */
extern void mem_move(void *dest, void *src, int32_t size) {
    if(!dest) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Destination pointer is NULL!");
        return;
    }

    if(!src) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Source pointer is NULL!");
        return;
    }

    if(size < 0) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Size to move must be a positive number!");
        return;
    }

    if(size == 0) {
        /* nothing to do. */
        return;
    }

    // NOLINTNEXTLINE
    memmove(dest, src, (size_t)(uint32_t)size);
}


extern int32_t mem_cmp(void *src1, int32_t src1_size, void *src2, int32_t src2_size) {
    if(!src1 || src1_size <= 0) {
        if(!src2 || src2_size <= 0) {
            /* both are NULL or zero length, but that is "equal" for our purposes. */
            return 0;
        } else {
            /* first one is "less" than second. */
            return -1;
        }
    } else {
        if(!src2 || src2_size <= 0) {
            /* first is "greater" than second */
            return 1;
        } else {
            /* both pointers are non-NULL and the lengths are positive. */

            /* short circuit the comparison if the blocks are different lengths */
            if(src1_size != src2_size) { return (src1_size - src2_size); }

            return memcmp(src1, src2, (size_t)(uint32_t)src1_size);
        }
    }
}
