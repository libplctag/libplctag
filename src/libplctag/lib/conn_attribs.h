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
 * The connection attributes every protocol exposes through its tag vtable.
 *
 * Each module resolves the connection object from its own tag type and hands the
 * resulting atomics here; the clamping, defaulting and logging are identical across
 * CIP and Modbus and live in one place.  A NULL pointer means the tag has no connection
 * yet, which is a state rather than an error for the getters.
 */

#include <stdint.h>
#include <utils/atomic_utils.h>
#include <utils/debug.h>


/* PLCTAG_CONN_STATUS_DOWN when there is no connection. */
extern int32_t conn_get_status(atomic_int32_t *status, int32_t *result);

/* the library default when there is no connection: that is what the next one will use. */
extern int32_t conn_get_inactivity_timeout_ms(atomic_int32_t *timeout_ms, int32_t *result);

/*
 * Clamps to [CONN_INACTIVITY_TIMEOUT_MIN_MS, CONN_INACTIVITY_TIMEOUT_MAX_MS], returning
 * PLCTAG_ERR_OUT_OF_BOUNDS if it had to.  Returns PLCTAG_ERR_NOT_FOUND, without storing
 * anything, when there is no connection to set it on.
 */
extern int32_t conn_set_inactivity_timeout_ms(atomic_int32_t *timeout_ms, int32_t value, int32_t tag_id,
                                              debug_module_t debug_module);
