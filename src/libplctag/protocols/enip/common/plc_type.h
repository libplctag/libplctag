#pragma once
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

#include <stdbool.h>
#include <stddef.h>
#include "platform.h"
#include "utils/bytes.h"

/*
 * PLC family shared by both directions of the ENIP protocol: the client uses
 * it to auto-classify a connection from its CIP Identity reply (vendor id +
 * device type + product-name catalog prefix, see dialects/rockwell and
 * dialects/omron); the server (device_sim/eip_server_tag) uses the same
 * value to select which family's identity, tag-listing behavior, and (for
 * PCCC families) data path to emulate. One enum for both directions so
 * "what the client can detect" and "what the server can emulate" can never
 * drift apart. Mirrors the family list in protocols/ab/defs.h's plc_type_t
 * (copied, not shared/linked -- ab is being folded away, see
 * SERVER_TAGS.md); named enip_plc_type_t, not plc_type_t, only to avoid a
 * generic, easily-collided name.
 *
 * Unlike ab's AB_PLC_NONE (a "user hasn't said yet" sentinel for an
 * attribute the user types in), ENIP_PLC_UNKNOWN here means "we saw a real
 * CIP Identity reply and it did not match any recognized catalog prefix" on
 * the client side; the server side never emulates ENIP_PLC_UNKNOWN (it is
 * not a valid `plc=` selection -- see eip_server_tag.c's parse_plc_type).
 */
typedef enum {
    ENIP_PLC_UNKNOWN = 0,
    ENIP_PLC_PLC5,
    ENIP_PLC_SLC,
    ENIP_PLC_MLGX,
    ENIP_PLC_LGX,
    ENIP_PLC_MICRO800,
    ENIP_PLC_OMRON_NJNX,
} enip_plc_type_t;

/* True if name starts with prefix (case-sensitive, matches Rockwell/OMRON
 * catalog-number conventions). */
static inline bool bytes_has_prefix(Bytes name, const char *prefix) {
    size_t prefix_len = str_length(prefix) < 0 ? 0 : (size_t)str_length(prefix);
    return name.data && name.len >= prefix_len && mem_cmp(name.data, (int)prefix_len, (void *)prefix, (int)prefix_len) == 0;
}
