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

#include <stdint.h>

/*
 * CIP tag-access service codes, reply status codes and data type codes.
 *
 * These are the wire values every CIP family uses, Rockwell and OMRON alike.  A family
 * that does not implement a service simply never sends it -- OMRON NJ/NX, for instance,
 * has no fragmented read or write, so it never sends 0x52 or 0x53 and never sees 0x06.
 */



/* tag access services. */
#define CIP_SVC_READ ((uint8_t)0x4C)
#define CIP_SVC_READ_FRAG ((uint8_t)0x52)
#define CIP_SVC_WRITE ((uint8_t)0x4D)
#define CIP_SVC_WRITE_FRAG ((uint8_t)0x53)
#define CIP_SVC_RMW ((uint8_t)0x4E)
#define CIP_SVC_MULTI ((uint8_t)0x0A)
#define CIP_SVC_GET_ATTR_LIST ((uint8_t)0x03)

/*
 * Listing a PLC's tags or UDTs is not a CIP service.  Each vendor invented its own --
 * Rockwell reads its symbol and template classes, OMRON uses a service of its own --
 * so those codes belong to the vendor's module, not here.
 */

/* OR'd into the service code in a reply. */
#define CIP_SVC_REPLY ((uint8_t)0x80)

/* reply status codes. */
#define CIP_STATUS_OK ((uint8_t)0x00)
#define CIP_STATUS_FRAG ((uint8_t)0x06) /* more data follows; ask again at the next offset */
#define CIP_STATUS_UNSUPPORTED_SERVICE ((uint8_t)0x08)
#define CIP_STATUS_PARTIAL_ERROR ((uint8_t)0x1E)

/* the two variable-length string type codes, which have no fixed element stride. */
#define CIP_DATA_STRING ((uint8_t)0xD0)       /* 2-byte count, then that many characters */
#define CIP_DATA_SHORT_STRING ((uint8_t)0xDA) /* 1-byte count, then that many characters */

/*
 * The largest tag payload we will grow a buffer to.
 *
 * A PLC can keep answering a fragmented read with "more follows" forever.  This is the
 * point at which we stop believing it.
 */
#define CIP_MAX_TAG_DATA_SIZE (8 * 1024 * 1024)

/*
 * How many fragment replies carrying no payload we tolerate in a row.
 *
 * Zero bytes with a partial status is legitimate once -- a packed reply can leave a later
 * request nothing but a header -- but forever means the transfer is not advancing.
 */
/*
 * The largest atomic CIP data type, LINT and LREAL.  A transfer may be split inside an
 * element but never inside one of these, so this bounds the split granularity.
 */
#define CIP_MAX_ATOMIC_SIZE (8)

#define CIP_MAX_FRAGMENT_RETRIES (100)
