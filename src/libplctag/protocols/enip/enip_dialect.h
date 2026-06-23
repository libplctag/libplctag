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
 * Per-connection dialect seam (design doc §16a.4).
 *
 * The IO thread, scheduler, lifetime, framing, and the right-sized state
 * machine are common to every CIP device. The manufacturer differences live
 * behind a small vtable selected once per connection from the device's CIP
 * Identity reply during bring-up.  Two concerns actually diverge:
 *
 *   build  - encode the CIP request for a tag's current op (symbolic Read/Write
 *            for Logix/OMRON; Execute-PCCC for PLC-5/SLC/MicroLogix).
 *   apply  - parse one CIP sub-reply, interpret status (vendor-specific), copy
 *            into the tag buffer, and signal whether another round trip is due.
 *
 * The two numbers (requested_cip_size, max_batch_cap) feed existing arithmetic
 * rather than branching on vendor (Micro800 / PCCC set max_batch_cap = 1 to
 * route through the single-in-flight path; §16a.3).
 */

#include <stdbool.h>
#include <stdint.h>
#include <utils/arena.h>
#include <utils/bytes.h>

typedef struct enip_connection_t enip_connection_t;
typedef struct enip_tag_t *enip_tag_p;

typedef struct enip_dialect_t {
    const char *name;

    size_t requested_cip_size; /* number -> ForwardOpen builder; 0 = engine default */
    uint16_t max_batch_cap;    /* number -> min() batch cap; 0 = no cap, 1 = no 0x0A */

    /* Encode the CIP request for t's current op. Caller holds t->api_mutex and
     * has reset the arena. Returns bytes_null() on failure. */
    Bytes (*build)(Arena *a, enip_connection_t *c, enip_tag_p t);

    /* Consume one already-CPF-unwrapped CIP reply: parse it, interpret status
     * (Rockwell 0x06 partial-transfer is meaningful only to its own dialect),
     * copy into t->data, set *more for another round trip. Returns a
     * PLCTAG_STATUS_*/PLCTAG_ERR_* code. Caller holds t->api_mutex. */
    int32_t (*apply)(enip_connection_t *c, enip_tag_p t, Bytes cip_reply, bool *more);
} enip_dialect_t;

/* Logix/Micro800 symbolic dialect (CIP Read/Write Tag 0x4C/0x4D). The default
 * for every connection; also serves OMRON named I/O until its dialect lands. */
extern const enip_dialect_t enip_logix_dialect;

/* PLC-5 / SLC500 / MicroLogix PCCC dialect (Execute PCCC 0x4B on class 0x67). */
extern const enip_dialect_t enip_pccc_dialect;

/* Select the dialect for a connection from its parsed CIP Identity fields.
 * Called once at the end of identity bring-up. Never returns NULL. */
extern const enip_dialect_t *enip_dialect_select(uint16_t vendor_id, uint16_t device_type);
