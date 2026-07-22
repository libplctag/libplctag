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

/*
 * device_types.h — CIP/PCCC atomic type <-> element size, one source of
 * truth (ENIP-UPDATES-PLAN.md item 3.h). Previously three independent
 * copies: eip_server_tag.c's CIP_TYPES[]/PCCC_TYPES[] (name -> type+size,
 * for elem_type=/pccc_type= attribute parsing), device_sim.c's
 * device_elem_size_for_type (a hand-written type -> size switch spelling
 * the same sizes a third time), and item 2's udt= member parser (reused
 * lookup_cip_type). Now: CIP_TYPES/PCCC_TYPES live here; both
 * eip_server_tag.c's attribute parsing and device_elem_size_for_type
 * (device.h's declaration, defined here) read the same tables.
 */

#include <stdbool.h>
#include <stddef.h>
#include "device_sim.h" /* tag_type_t, TAG_CIP_TYPE_*, TAG_PCCC_TYPE_*, TAG_*_STRING_SIZE */

typedef struct {
    const char *name;
    tag_type_t type;
    size_t elem_size;
} cip_type_entry_t;

/* elem_type= attribute names (CIP atomic types + STRING). NULL-name-terminated. */
extern const cip_type_entry_t CIP_TYPES[];

/* pccc_type= attribute letters (Allen-Bradley PLC-5/SLC file-type letters).
 * NULL-name-terminated. */
extern const cip_type_entry_t PCCC_TYPES[];

extern bool lookup_cip_type(const char *name, tag_type_t *type_out, size_t *elem_size_out);
extern bool lookup_pccc_type(const char *letter, tag_type_t *type_out, size_t *elem_size_out);
