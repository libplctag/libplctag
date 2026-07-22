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

#include "platform.h"
#include "device.h"
#include "device_types.h"

const cip_type_entry_t CIP_TYPES[] = {
    {"BOOL",   TAG_CIP_TYPE_BOOL,   1},
    {"SINT",   TAG_CIP_TYPE_SINT,   1},
    {"INT",    TAG_CIP_TYPE_INT,    2},
    {"DINT",   TAG_CIP_TYPE_DINT,   4},
    {"LINT",   TAG_CIP_TYPE_LINT,   8},
    {"USINT",  TAG_CIP_TYPE_USINT,  1},
    {"UINT",   TAG_CIP_TYPE_UINT,   2},
    {"UDINT",  TAG_CIP_TYPE_UDINT,  4},
    {"ULINT",  TAG_CIP_TYPE_ULINT,  8},
    {"REAL",   TAG_CIP_TYPE_REAL,   4},
    {"LREAL",  TAG_CIP_TYPE_LREAL,  8},
    {"BYTE",   TAG_CIP_TYPE_BYTE,   1},
    {"WORD",   TAG_CIP_TYPE_WORD,   2},
    {"DWORD",  TAG_CIP_TYPE_DWORD,  4},
    {"LWORD",  TAG_CIP_TYPE_LWORD,  8},
    {"STRING", TAG_CIP_TYPE_STRING, TAG_CIP_STRING_SIZE},
    {NULL,     0,                   0},
};

/* pccc_type= letter, matching Allen-Bradley PLC-5/SLC file-type letters. */
const cip_type_entry_t PCCC_TYPES[] = {
    {"B",  TAG_PCCC_TYPE_BIT,    2},
    {"N",  TAG_PCCC_TYPE_INT,    2},
    {"L",  TAG_PCCC_TYPE_DINT,   4},
    {"F",  TAG_PCCC_TYPE_REAL,   4},
    {"R",  TAG_PCCC_TYPE_REAL,   4},
    {"ST", TAG_PCCC_TYPE_STRING, TAG_PCCC_STRING_SIZE},
    {NULL, 0,                    0},
};

extern bool lookup_cip_type(const char *name, tag_type_t *type_out, size_t *elem_size_out) {
    for(const cip_type_entry_t *e = CIP_TYPES; e->name; e++) {
        if(str_cmp_i(name, e->name) == 0) {
            *type_out = e->type;
            *elem_size_out = e->elem_size;
            return true;
        }
    }
    return false;
}

extern bool lookup_pccc_type(const char *letter, tag_type_t *type_out, size_t *elem_size_out) {
    for(const cip_type_entry_t *e = PCCC_TYPES; e->name; e++) {
        if(str_cmp_i(letter, e->name) == 0) {
            *type_out = e->type;
            *elem_size_out = e->elem_size;
            return true;
        }
    }
    return false;
}

/* type -> element size (device.h's declaration; previously an independent
 * hand-written switch in device_sim.c spelling the same sizes a second
 * time). Derived from the same CIP_TYPES/PCCC_TYPES tables above -- PCCC_TYPES'
 * two REAL entries ("F"/"R") share one type/size, so the linear scan can
 * never disagree with itself. Unknown types return 0, matching the switch's
 * prior default. */
extern size_t device_elem_size_for_type(tag_type_t t) {
    for(const cip_type_entry_t *e = CIP_TYPES; e->name; e++) {
        if(e->type == t) { return e->elem_size; }
    }
    for(const cip_type_entry_t *e = PCCC_TYPES; e->name; e++) {
        if(e->type == t) { return e->elem_size; }
    }
    return 0;
}
