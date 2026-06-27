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
 * PCCC logical-address parsing and PLC-5 / SLC address encoding for the
 * generic ENIP engine. Copied from the AB driver's pccc.c and adapted to the
 * coding guidelines (Bytes buffers, sized integers, bool flags). The AB copy
 * is being retired; this module is self-contained so ENIP does not depend on
 * the AB tag world.
 */

#include <stdbool.h>
#include <stdint.h>
#include <utils/bytes.h>

typedef enum {
    PCCC_FILE_UNKNOWN = 0x00, /* UNKNOWN! */
    PCCC_FILE_ASCII = 0x8e,
    PCCC_FILE_BCD = 0x8f,
    PCCC_FILE_BIT = 0x85,
    PCCC_FILE_BLOCK_TRANSFER = 0x00, /* UNKNOWN! */
    PCCC_FILE_CONTROL = 0x88,
    PCCC_FILE_COUNTER = 0x87,
    PCCC_FILE_FLOAT = 0x8a,
    PCCC_FILE_INPUT = 0x8c,
    PCCC_FILE_INT = 0x89,
    PCCC_FILE_LONG_INT = 0x91,
    PCCC_FILE_MESSAGE = 0x92,
    PCCC_FILE_OUTPUT = 0x8b,
    PCCC_FILE_PID = 0x93,
    PCCC_FILE_SFC = 0x00, /* UNKNOWN! */
    PCCC_FILE_STATUS = 0x84,
    PCCC_FILE_STRING = 0x8d,
    PCCC_FILE_TIMER = 0x86
} pccc_file_t;

typedef struct {
    pccc_file_t file_type;
    int32_t file;
    int32_t element;
    int32_t sub_element;
    int32_t element_size_bytes;
    uint8_t bit;
    bool is_bit;
} pccc_addr_t;

/* Parse a PLC-neutral PCCC logical address (e.g. N7:0, F8:0, B3:0/2) into
 * address. Returns PLCTAG_STATUS_OK or a PLCTAG_ERR_* code. */
extern int32_t enip_pccc_parse_logical_address(const char *file_address, pccc_addr_t *address);

/* Encode address as a PLC-5 level-encoded address into dest. Returns the used
 * prefix of dest, or bytes_null() if it does not fit or on error. */
extern Bytes enip_pccc_encode_plc5_address(pccc_addr_t *address, Bytes dest);

/* Encode address as an SLC/MicroLogix file/type/element/subelement address
 * into dest. Returns the used prefix of dest, or bytes_null() on error. */
extern Bytes enip_pccc_encode_slc_address(pccc_addr_t *address, Bytes dest);
