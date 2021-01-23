/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
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

#include <stdint.h>
#include <util/atomic_int.h>

#define DF1_CMD_OK ((uint8_t)(0x40))
#define DF1_TYPED_CMD ((uint8_t)(0x0F))

// #define DF1_PLC5_READ_MAX_PAYLOAD (244)
// #define DF1_PLC5_WRITE_MAX_PAYLOAD (240)



typedef enum { DF1_FILE_UNKNOWN        = 0x00, /* UNKNOWN! */
               DF1_FILE_ASCII          = 0x8e,
               DF1_FILE_BCD            = 0x8f,
               DF1_FILE_BIT            = 0x85,
               DF1_FILE_BLOCK_TRANSFER = 0x00, /* UNKNOWN! */
               DF1_FILE_CONTROL        = 0x88,
               DF1_FILE_COUNTER        = 0x87,
               DF1_FILE_FLOAT          = 0x8a,
               DF1_FILE_INPUT          = 0x8c,
               DF1_FILE_INT            = 0x89,
               DF1_FILE_LONG_INT       = 0x91,
               DF1_FILE_MESSAGE        = 0x92,
               DF1_FILE_OUTPUT         = 0x8b,
               DF1_FILE_PID            = 0x93,
               DF1_FILE_SFC            = 0x00, /* UNKNOWN! */
               DF1_FILE_STATUS         = 0x84,
               DF1_FILE_STRING         = 0x8d,
               DF1_FILE_TIMER          = 0x86,

               /* fake types for use in things like strings. */
               DF1_FILE_BYTE           = 0x1000
             } df1_file_t;

extern int df1_parse_logical_address(const char *name, df1_file_t *file_type, int *file_num, int *elem_num, int *subelem_num, bool *is_bit, uint8_t *bit_num);
extern int df1_element_size(df1_file_t file_type);
extern const char *df1_decode_error(uint8_t status, uint16_t extended_status);

