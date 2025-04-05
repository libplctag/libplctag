/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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

#include "random_utils.h"
#include <stdint.h>  // For uint32_t
#include <time.h>    // For time


/* FIXME - move this all over into a compatibility/platform check header */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    #include <stdlib.h>

uint64_t random_u64(uint64_t upper_bound) {
    uint64_t random_number = 0;

    arc4random_buf(&random_number, sizeof(random_number));
    random_number %= upper_bound;

    return random_number;
}

#elif defined(__linux__)
#include <stdlib.h>
#include <sys/random.h>


uint64_t random_u64(uint64_t upper_bound) {
    uint64_t random_number = 0;

    if (upper_bound == 0) {
        return 0;
    }

    if (getrandom(&random_number, sizeof(random_number), GRND_NONBLOCK) < (ssize_t)sizeof(random_number)) {
        /* not enough entropy, do it the hard way. */
        srand((unsigned int)((uint64_t)time(NULL) ^ random_number));
        for (size_t i = 0; i < sizeof(random_number); ++i) {
            ((uint8_t*)&random_number)[i] ^= (uint8_t)(rand() % 256);
        }
    }

    random_number %= upper_bound;

    return random_number;
}


#elif defined(_WIN32) || defined(_WIN64)
#include <Windows.h>

#include <wincrypt.h>

#include <bcrypt.h>  /* for BCryptGenRandom() */


uint64_t random_u64(uint64_t upper_bound) {
    uint64_t random_number = 0;

    if(BCryptGenRandom(NULL, (PUCHAR)&random_number, sizeof(random_number), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return RANDOM_U64_ERROR;
    }
    if(upper_bound == 0) {
        return 0;
    }
    random_number %= upper_bound;

    return random_number;
}
    


#else
    #error "Platform does not support good random function!"
#endif

