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

#include <stdbool.h>
#include <stdint.h>

/* ===== PROTOCOL ERRORS (ODVA Specified - DO NOT CHANGE) ===== */

/* EIP encapsulation status codes (ODVA EIP Specification) */
#define EIP_STATUS_SUCCESS ((uint32_t)0x0000)
#define EIP_STATUS_INVALID_COMMAND ((uint32_t)0x0001)     /* invalid or unsupported command */
#define EIP_STATUS_INSUFFICIENT_MEMORY ((uint32_t)0x0002) /* no memory for the reply */
#define EIP_STATUS_INCORRECT_DATA ((uint32_t)0x0003)      /* malformed data */
#define EIP_STATUS_INVALID_SESSION ((uint32_t)0x0064)     /* invalid session handle */
#define EIP_STATUS_INVALID_LENGTH ((uint32_t)0x0065)      /* invalid length */
#define EIP_STATUS_UNSUPPORTED_VERSION ((uint32_t)0x0069) /* unsupported protocol version */
