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

#pragma once

#include <stdint.h>

#include "device_sim.h"

/*
 * endpoint.c — lazy find-or-create-by-(bind_addr,port) registry for
 * role=server tags (SERVER_TAGS.md).
 *
 * device_sim_create/start/stop/destroy already implement everything a
 * listening endpoint needs (identity seeding, dialect registration, listener
 * + discovery threads, clean shutdown) for the explicit-lifecycle
 * device_sim_* API. This registry is a thin, refcounted wrapper around those
 * same functions so that independent plc_tag_create(role=server,...) calls
 * can share one running endpoint keyed by (bind_addr, port): the first call
 * at an endpoint starts it; later calls just add a tag to the same device_t;
 * the last plc_tag_destroy stops it. Mirrors the find_or_create_* idiom
 * already used by protocols/ab/session.c (session_find_or_create) and
 * protocols/mb/modbus.c (find_or_create_plc).
 */

/* Must be called once before any endpoint_find_or_create(), and once at
 * shutdown. Guarded by LIBPLCTAG_FEATURE_SERVER at the call site (lib/init.c). */
extern int32_t endpoint_registry_init(void);
extern void endpoint_registry_teardown(void);

/*
 * Find an existing endpoint at (bind_addr,port) and increment its refcount,
 * or create+start a new one (device_sim_create + device_sim_start) with
 * refcount 1. bind_addr may be NULL (any interface); NULL and "0.0.0.0" are
 * treated as the same key.
 *
 * plc_type is only used when creating a new endpoint; if an endpoint already
 * exists at this (bind_addr,port), its original plc_type wins and this
 * parameter is ignored (a server can only be one PLC type per endpoint).
 *
 * Returns NULL on failure (out of memory or thread/socket creation failure).
 */
extern device_sim_t *endpoint_find_or_create(const char *bind_addr, uint16_t port, plc_type_t plc_type);

/*
 * Decrement the refcount of the endpoint owning sim. At zero, stops the
 * listener/discovery threads (device_sim_stop + join) and destroys the
 * device_sim_t (device_sim_destroy) before returning. Safe to call while
 * other tags still reference the same endpoint (refcount > 0 leaves it
 * running untouched).
 */
extern void endpoint_release(device_sim_t *sim);
