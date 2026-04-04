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
 * Modbus TCP protocol processing using Bytes/Arena instead of buf_t.
 *
 * The MBAP header is handled by the caller (modbus_server3.c).
 * This layer only deals with the PDU: function code + body.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "arena.h"
#include "bytes.h"
#include "register_storage.h"

/* ============================================================================
 * MBAP header constants
 * ============================================================================ */

#define MBAP_HEADER_SIZE 7

/* ============================================================================
 * Function codes
 * ============================================================================ */

#define MODBUS_FC_READ_COILS                ((uint8_t)0x01)
#define MODBUS_FC_READ_DISCRETE_INPUTS      ((uint8_t)0x02)
#define MODBUS_FC_READ_HOLDING_REGISTERS    ((uint8_t)0x03)
#define MODBUS_FC_READ_INPUT_REGISTERS      ((uint8_t)0x04)
#define MODBUS_FC_WRITE_SINGLE_COIL         ((uint8_t)0x05)
#define MODBUS_FC_WRITE_SINGLE_REGISTER     ((uint8_t)0x06)
#define MODBUS_FC_WRITE_MULTIPLE_COILS      ((uint8_t)0x0F)
#define MODBUS_FC_WRITE_MULTIPLE_REGISTERS  ((uint8_t)0x10)

/* ============================================================================
 * Limits
 * ============================================================================ */

#define MODBUS_MAX_PDU_SIZE             260
#define MODBUS_MAX_READ_RESPONSE_BYTES  250
#define MODBUS_MAX_READ_COILS           2000
#define MODBUS_MAX_READ_DISCRETE_INPUTS 2000
#define MODBUS_MAX_READ_REGISTERS       125
#define MODBUS_MAX_WRITE_COILS          1968
#define MODBUS_MAX_WRITE_REGISTERS      123

/* ============================================================================
 * Protocol processing
 * ============================================================================ */

/*
 * Process a Modbus PDU request and return the response PDU.
 *
 *   a        — arena for all response allocations (should be reset each cycle)
 *   fc       — function code byte
 *   body     — request body (bytes after the FC byte)
 *   storage  — register/coil storage
 *
 * Returns the complete response PDU (FC byte + data) as a Bytes value.
 * On exception, returns an error PDU (FC|0x80, exception_code).
 * Returns {NULL, 0} only on arena exhaustion.
 */
extern Bytes modbus_process_request_bytes(Arena *a, uint8_t fc, Bytes body,
                                          register_storage_t *storage);

#ifdef __cplusplus
}
#endif

