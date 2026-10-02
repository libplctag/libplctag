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


#include <utils/backoff.h>
#include <utils/random_utils.h>

#include <stdint.h>


void backoff_init(backoff_t *backoff, int64_t initial_ms, int64_t max_ms) {
    if(initial_ms < 1) { initial_ms = 1; }
    if(max_ms < initial_ms) { max_ms = initial_ms; }

    backoff->initial_ms = initial_ms;
    backoff->max_ms = max_ms;
    backoff->base_ms = initial_ms;
    backoff->attempts = 0;
}


void backoff_reset(backoff_t *backoff) {
    backoff->base_ms = backoff->initial_ms;
    backoff->attempts = 0;
}


int64_t backoff_wait_ms(backoff_t *backoff) {
    int64_t half = backoff->base_ms / 2;
    int64_t result = 0;

    /*
     * Equal jitter, drawn on every call.  Folding the randomness into base_ms instead
     * would freeze one sample for as long as the delay stays saturated, and a fleet of
     * clients knocked off by the same device reboot would retry at a fixed offset from
     * each other until the outage ended.
     */
    result = half + (int64_t)random_u64((uint64_t)half);

    if(result < 1) { result = 1; }

    /*
     * Grow by doubling rather than by shifting an attempt counter.  base_ms is never
     * above max_ms when we get here, so the double cannot overflow, and once it
     * saturates it stays there -- there is no exponent to run away.  attempts is only
     * ever reported, so its eventual unsigned wrap changes nothing.
     */
    if(backoff->base_ms < backoff->max_ms) {
        backoff->base_ms *= 2;
        if(backoff->base_ms > backoff->max_ms) { backoff->base_ms = backoff->max_ms; }
    }

    backoff->attempts++;

    return result;
}
