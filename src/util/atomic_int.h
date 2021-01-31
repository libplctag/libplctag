/***************************************************************************
 *   Copyright (C) 2021 by Kyle Hayes                                      *
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


#include <stdint.h>
#include <limits.h>
#include <lib/libplctag.h>
#include <util/bool.h>
#include <util/debug.h>
#include <util/platform.h>
// #include <util/lock.h>

#define ATOMIC_INT_STATIC_INIT(n) {n}

typedef struct { int32_t val; } atomic_int32;
typedef struct { int16_t val; } atomic_int16;
typedef struct { int8_t val; } atomic_int8;

typedef struct { uint32_t val; } atomic_uint32;
typedef struct { uint16_t val; } atomic_uint16;
typedef struct { uint8_t val; } atomic_uint8;


#define ATOMIC_BOOL_STATIC_INIT(b) {b}
typedef struct { bool val; } atomic_bool;

#ifdef IS_WINDOWS


#else

/* Only GCC and Clang! */

/*
 * NOTE: People are still using the older versions of GCC for many
 * embedded platforms.  Keep using the old __sync_XXX() functions for now.
 */

/* int32 */

static inline int32_t atomic_int32_load(atomic_int32 *atomic_val)
{
    return  __sync_fetch_and_or((int32_t *)atomic_val, 0);
}

static inline int32_t atomic_int32_store(atomic_int32 *atomic_val, int32_t new_val)
{
    int32_t old_val = 0;
    int32_t result_val = new_val;

    /* lame.  We need to loop until CAS works. */
    do {
        old_val = __sync_val_compare_and_swap((int32_t *)atomic_val, old_val, new_val);

        if(old_val != new_val) {
            result_val = old_val;
        }
    } while(old_val != new_val);

    return result_val;
}

static inline int32_t atomic_int32_add(atomic_int32 *atomic_val, int32_t addend)
{
    return __sync_fetch_and_add((int32_t *)atomic_val, addend);
}

static inline int32_t atomic_int32_sub(atomic_int32 *atomic_val, int32_t subend)
{
    return __sync_fetch_and_sub((int32_t *)atomic_val, subend);
}


/* int16 */

static inline int16_t atomic_int16_load(atomic_int16 *atomic_val)
{
    return  __sync_fetch_and_or((int16_t *)atomic_val, 0);
}

static inline int16_t atomic_int16_store(atomic_int16 *atomic_val, int16_t new_val)
{
    int16_t old_val = 0;
    int16_t result_val = new_val;

    /* lame.  We need to loop until CAS works. */
    do {
        old_val = __sync_val_compare_and_swap((int16_t *)atomic_val, old_val, new_val);

        if(old_val != new_val) {
            result_val = old_val;
        }
    } while(old_val != new_val);

    return result_val;
}

static inline int16_t atomic_int16_add(atomic_int16 *atomic_val, int16_t addend)
{
    return __sync_fetch_and_add((int16_t *)atomic_val, addend);
}

static inline int16_t atomic_int16_sub(atomic_int16 *atomic_val, int16_t subend)
{
    return __sync_fetch_and_sub((int16_t *)atomic_val, subend);
}



/* int8 */

static inline int8_t atomic_int8_load(atomic_int8 *atomic_val)
{
    return  __sync_fetch_and_or((int8_t *)atomic_val, 0);
}

static inline int8_t atomic_int8_store(atomic_int8 *atomic_val, int8_t new_val)
{
    int8_t old_val = 0;
    int8_t result_val = new_val;

    /* lame.  We need to loop until CAS works. */
    do {
        old_val = __sync_val_compare_and_swap((int8_t *)atomic_val, old_val, new_val);

        if(old_val != new_val) {
            result_val = old_val;
        }
    } while(old_val != new_val);

    return result_val;
}

static inline int8_t atomic_int8_add(atomic_int8 *atomic_val, int8_t addend)
{
    return __sync_fetch_and_add((int8_t *)atomic_val, addend);
}

static inline int8_t atomic_int8_sub(atomic_int8 *atomic_val, int8_t subend)
{
    return __sync_fetch_and_sub((int8_t *)atomic_val, subend);
}



/* uint32 */

static inline uint32_t atomic_uint32_load(atomic_uint32 *atomic_val)
{
    return  __sync_fetch_and_or((uint32_t *)atomic_val, 0);
}

static inline uint32_t atomic_uint32_store(atomic_uint32 *atomic_val, uint32_t new_val)
{
    uint32_t old_val = 0;
    uint32_t result_val = new_val;

    /* lame.  We need to loop until CAS works. */
    do {
        old_val = __sync_val_compare_and_swap((uint32_t *)atomic_val, old_val, new_val);

        if(old_val != new_val) {
            result_val = old_val;
        }
    } while(old_val != new_val);

    return result_val;
}

static inline uint32_t atomic_uint32_add(atomic_uint32 *atomic_val, uint32_t addend)
{
    return __sync_fetch_and_add((uint32_t *)atomic_val, addend);
}

static inline uint32_t atomic_uint32_sub(atomic_uint32 *atomic_val, uint32_t subend)
{
    return __sync_fetch_and_sub((uint32_t *)atomic_val, subend);
}


/* uint16 */

static inline uint16_t atomic_uint16_load(atomic_uint16 *atomic_val)
{
    return  __sync_fetch_and_or((uint16_t *)atomic_val, 0);
}

static inline uint16_t atomic_uint16_store(atomic_uint16 *atomic_val, uint16_t new_val)
{
    uint16_t old_val = 0;
    uint16_t result_val = new_val;

    /* lame.  We need to loop until CAS works. */
    do {
        old_val = __sync_val_compare_and_swap((uint16_t *)atomic_val, old_val, new_val);

        if(old_val != new_val) {
            result_val = old_val;
        }
    } while(old_val != new_val);

    return result_val;
}

static inline uint16_t atomic_uint16_add(atomic_uint16 *atomic_val, uint16_t addend)
{
    return __sync_fetch_and_add((uint16_t *)atomic_val, addend);
}

static inline uint16_t atomic_uint16_sub(atomic_uint16 *atomic_val, uint16_t subend)
{
    return __sync_fetch_and_sub((uint16_t *)atomic_val, subend);
}



/* uint8 */

static inline uint8_t atomic_uint8_load(atomic_uint8 *atomic_val)
{
    return  __sync_fetch_and_or((uint8_t *)atomic_val, 0);
}

static inline uint8_t atomic_uint8_store(atomic_uint8 *atomic_val, uint8_t new_val)
{
    uint8_t old_val = 0;
    uint8_t result_val = new_val;

    /* lame.  We need to loop until CAS works. */
    do {
        old_val = __sync_val_compare_and_swap((uint8_t *)atomic_val, old_val, new_val);

        if(old_val != new_val) {
            result_val = old_val;
        }
    } while(old_val != new_val);

    return result_val;
}

static inline uint8_t atomic_uint8_add(atomic_uint8 *atomic_val, uint8_t addend)
{
    return __sync_fetch_and_add((uint8_t *)atomic_val, addend);
}

static inline uint8_t atomic_uint8_sub(atomic_uint8 *atomic_val, uint8_t subend)
{
    return __sync_fetch_and_sub((uint8_t *)atomic_val, subend);
}





/* bool */

static inline bool atomic_bool_load(atomic_bool *atomic_val)
{
    return  __sync_fetch_and_or((bool *)atomic_val, 0);
}

static inline bool atomic_bool_store(atomic_bool *atomic_val, bool new_val)
{
    bool old_val = 0;
    bool result_val = new_val;

    do {
        old_val = __sync_val_compare_and_swap((bool *)atomic_val, old_val, new_val);

        if(old_val != new_val) {
            result_val = old_val;
        }
    } while(old_val != new_val);

    return result_val;
}


#endif


// typedef struct { lock_t lock; volatile int val; } atomic_int;

// #define ATOMIC_INT_STATIC_INIT(n) {0, n}

// static inline int atomic_int_get(atomic_int *a)
// {
//     int val = 0;

//     pdebug(DEBUG_SPEW, "Starting.");

//     if(!a) {
//         return INT_MAX;
//     }

//     spin_block(&a->lock) {
//         val = a->val;
//     }

//     pdebug(DEBUG_SPEW, "Done.");

//     return val;
// }



// static inline int atomic_int_set(atomic_int *a, int new_val)
// {
//     int old_val = 0;

//     pdebug(DEBUG_SPEW, "Starting.");

//     if(!a) {
//         return INT_MAX;
//     }

//     spin_block(&a->lock) {
//         old_val = a->val;
//         a->val = new_val;
//     }

//     pdebug(DEBUG_SPEW, "Done.");

//     return old_val;
// }



// static inline int atomic_int_add(atomic_int *a, int other)
// {
//     int old_val = 0;

//     pdebug(DEBUG_SPEW, "Starting.");

//     spin_block(&a->lock) {
//         old_val = a->val;

//         a->val += other;
//     }

//     pdebug(DEBUG_SPEW, "Done.");

//     return old_val;
// }



// static inline int atomic_int_add_mask(atomic_int *a, int addend, unsigned int mask)
// {
//     int old_val = 0;

//     pdebug(DEBUG_SPEW, "Starting.");

//     spin_block(&a->lock) {
//         old_val = a->val;
//         a->val += addend;
//         a->val &= (int)(unsigned int)mask;
//     }

//     pdebug(DEBUG_SPEW, "Done.");

//     return old_val;
// }


// static inline int atomic_int_and(atomic_int *a, int other)
// {
//     int old_val = 0;

//     pdebug(DEBUG_SPEW, "Starting.");

//     spin_block(&a->lock) {
//         old_val = a->val;
//         a->val &= other;
//     }

//     pdebug(DEBUG_SPEW, "Done.");

//     return old_val;
// }



// static inline int atomic_int_or(atomic_int *a, int other)
// {
//     int old_val = 0;

//     pdebug(DEBUG_SPEW, "Starting.");

//     spin_block(&a->lock) {
//         old_val = a->val;
//         a->val |= other;
//     }

//     pdebug(DEBUG_SPEW, "Done.");

//     return old_val;
// }



// static inline int atomic_int_xor(atomic_int *a, int other)
// {
//     int old_val = 0;

//     pdebug(DEBUG_SPEW, "Starting.");

//     spin_block(&a->lock) {
//         old_val = a->val;
//         a->val ^= other;
//     }

//     pdebug(DEBUG_SPEW, "Done.");

//     return old_val;
// }


// /* atomic bit manipulation */

// static inline int atomic_int_get_bit(atomic_int *a, int bit)
// {
//     int val = 0;

//     pdebug(DEBUG_SPEW, "Starting.");

//     if(bit < 0 || (bit >= (((int)(unsigned int)sizeof(int)*CHAR_BIT)))) {
//         pdebug(DEBUG_WARN, "Bit must be between 0 and %d!", ((int)(unsigned int)(sizeof(int)*CHAR_BIT)) - 1);
//         return PLCTAG_ERR_OUT_OF_BOUNDS;
//     }

//     spin_block(&a->lock) {
//         val = (a->val & (1 << bit)) ? 1 : 0;
//     }

//     pdebug(DEBUG_SPEW, "Done.");

//     return val;
// }

// static inline int atomic_int_set_bit(atomic_int *a, int bit, int new_val)
// {
//     int old_val = 0;

//     pdebug(DEBUG_SPEW, "Starting.");

//     if(bit < 0 || (bit >= (((int)(unsigned int)sizeof(int)*CHAR_BIT)))) {
//         pdebug(DEBUG_WARN, "Bit must be between 0 and %d!", ((int)(unsigned int)(sizeof(int)*CHAR_BIT)) - 1);
//         return PLCTAG_ERR_OUT_OF_BOUNDS;
//     }

//     spin_block(&a->lock) {
//         /* save old value */
//         old_val = (a->val & ((int)1 << bit)) ? 1 : 0;

//         if(new_val) {
//             a->val |= ((int)1 << bit);
//         } else {
//             a->val &= (~((int)1 << bit));
//         }
//     }

//     pdebug(DEBUG_SPEW, "Done.");

//     return old_val;
// }



// /* convenience for single bools */

// typedef atomic_int atomic_bool;

// #ifndef bool
// #define bool int
// #endif

// #define ATOMIC_BOOL_STATIC_INIT(n) {0, n}


// static inline bool atomic_bool_get(atomic_bool *ab)
// {
//     int val = 0;

//     pdebug(DEBUG_SPEW, "Starting.");

//     spin_block(&ab->lock) {
//         val = (ab->val ? 1 : 0);
//     }

//     pdebug(DEBUG_SPEW, "Done.");

//     return val;
// }


// static inline bool atomic_bool_set(atomic_bool *ab, bool new_val)
// {
//     int old_val = 0;

//     pdebug(DEBUG_SPEW, "Starting.");

//     spin_block(&ab->lock) {
//         old_val = (ab->val ? 1 : 0);

//         ab->val = (new_val ? 1 : 0);
//     }

//     pdebug(DEBUG_SPEW, "Done.");

//     return old_val;
// }


