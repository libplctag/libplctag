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

#pragma once

#include <libplctag/protocols/cip/defs.h>
#include <libplctag/protocols/eip/defs.h>
#include <utils/byteorder.h>
#include <utils/macros.h>


/*
 * How many consecutive zero-payload fragment responses to accept before giving up.
 *
 * A partial-transfer status with no data is legitimate -- packing several requests into one
 * packet can leave the later ones with only a bare CIP header -- but it makes no progress, and
 * a PLC that sends nothing else keeps us asking for the same fragment forever.  This is high
 * enough that ordinary packing starvation resolves itself and low enough that a stuck transfer
 * fails quickly.
 */
#define MAX_FRAGMENT_RETRIES (100)

#define OMRON_EIP_PLC5_PARAM ((uint16_t)0x4302)
#define OMRON_EIP_SLC_PARAM ((uint16_t)0x4302)
#define OMRON_EIP_LGX_PARAM ((uint16_t)0x43F8)


// 0100 0011 1111 1000
// 0100 001 1 1111 1000
// 0100 001 0 0000 0000  0000 0100 0000 0000
// 0x42000400


#define DEFAULT_MAX_REQUESTS (10) /* number of requests and request sizes to allocate by default. */


/* AB Constants*/

/* in milliseconds */

/* AB Commands */

/* AB packet info */

/* specific sub-commands */

/* CIP embedded packet commands */

/* flag set when command is OK */


/* base data type byte values */
/* OBSOLETE - this is now in cip.c in a table. */

/* aggregate data type byte values */


/* transport class */


// #define OMRON_EIP_TRANSPORT 0xA3


/* EIP Item Types */


/* Types of AB protocols */
// #define OMRON_PLC_PLC         (1)
// #define OMRON_PLC_MLGX        (2)
// #define OMRON_PLC_LGX         (3)
// #define OMRON_PLC_MICRO800     (4)
// #define OMRON_PLC_LGX_PCCC    (5)

typedef enum {
    OMRON_PLC_NONE = 0,
    OMRON_PLC_OMRON_NJNX = 7,
    OMRON_PLC_TYPE_LAST,
} plc_type_t;


//
