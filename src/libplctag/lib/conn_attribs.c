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

/* Shared connection attribute accessors.  See conn_attribs.h for the NULL contract. */

#include <libplctag/api/libplctag.h>
#include <libplctag/lib/conn_attribs.h>
#include <libplctag/lib/conn_timeouts.h>
#include <inttypes.h>
#include <stdint.h>
#include <utils/atomic_utils.h>
#include <utils/debug.h>


int32_t conn_get_status(atomic_int32_t *status, int32_t *result) {
    /* no connection means the tag is not connected, which is a state and not an error. */
    *result = (status ? atomic_get_int32(status) : (int32_t)PLCTAG_CONN_STATUS_DOWN);

    return PLCTAG_STATUS_OK;
}


int32_t conn_get_inactivity_timeout_ms(atomic_int32_t *timeout_ms, int32_t *result) {
    /* no connection means the one that will be created uses the default. */
    *result = (timeout_ms ? atomic_get_int32(timeout_ms) : (int32_t)CONN_INACTIVITY_TIMEOUT_MAX_MS);

    return PLCTAG_STATUS_OK;
}


int32_t conn_set_inactivity_timeout_ms(atomic_int32_t *timeout_ms, int32_t value, int32_t tag_id,
                                       debug_module_t debug_module) {
    int32_t clamped_value = value;
    int32_t rc = PLCTAG_STATUS_OK;

    if(clamped_value < CONN_INACTIVITY_TIMEOUT_MIN_MS) {
        clamped_value = CONN_INACTIVITY_TIMEOUT_MIN_MS;
        rc = PLCTAG_ERR_OUT_OF_BOUNDS;
        pdebug(debug_module, DEBUG_WARN, tag_id, "connection_inactivity_timeout_ms value %" PRId32 " clamped to minimum %d ms.",
               value, CONN_INACTIVITY_TIMEOUT_MIN_MS);
    } else if(clamped_value > CONN_INACTIVITY_TIMEOUT_MAX_MS) {
        clamped_value = CONN_INACTIVITY_TIMEOUT_MAX_MS;
        rc = PLCTAG_ERR_OUT_OF_BOUNDS;
        pdebug(debug_module, DEBUG_WARN, tag_id, "connection_inactivity_timeout_ms value %" PRId32 " clamped to maximum %d ms.",
               value, CONN_INACTIVITY_TIMEOUT_MAX_MS);
    }

    /* clamp first, then report the missing connection: the caller learns both problems. */
    if(!timeout_ms) {
        pdebug(debug_module, DEBUG_WARN, tag_id, "Cannot set connection_inactivity_timeout_ms: no connection exists.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    atomic_set_int32(timeout_ms, clamped_value);

    return rc;
}
