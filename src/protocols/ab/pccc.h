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

#ifndef __LIBPLCTAG_AB_PCCC_H__
#define __LIBPLCTAG_AB_PCCC_H__



#include <lib/libplctag.h>
#include <lib/tag.h>
#include <platform.h>

typedef enum {
               PCCC_FILE_UNKNOWN        = 0x00, /* UNKNOWN! */
               PCCC_FILE_ASCII          = 0x8e,
               PCCC_FILE_BCD            = 0x8f,
               PCCC_FILE_BIT            = 0x85,
               PCCC_FILE_BLOCK_TRANSFER = 0x00, /* UNKNOWN! */
               PCCC_FILE_CONTROL        = 0x88,
               PCCC_FILE_COUNTER        = 0x87,
               PCCC_FILE_FLOAT          = 0x8a,
               PCCC_FILE_INPUT          = 0x8c,
               PCCC_FILE_INT            = 0x89,
               PCCC_FILE_LONG_INT       = 0x91,
               PCCC_FILE_MESSAGE        = 0x92,
               PCCC_FILE_OUTPUT         = 0x8b,
               PCCC_FILE_PID            = 0x93,
               PCCC_FILE_SFC            = 0x00, /* UNKNOWN! */
               PCCC_FILE_STATUS         = 0x84,
               PCCC_FILE_STRING         = 0x8d,
               PCCC_FILE_TIMER          = 0x86
             } pccc_file_t;

typedef struct {
    pccc_file_t file_type;
    int file;
    int element;
    int sub_element;
    uint8_t is_bit;
    uint8_t bit;
    int element_size_bytes;
} pccc_addr_t;

extern int parse_pccc_logical_address(const char *file_address, pccc_addr_t *address);
extern int plc5_encode_address(uint8_t *data, int *size, int buf_size, pccc_addr_t *address);
extern int slc_encode_address(uint8_t *data, int *size, int buf_size, pccc_addr_t *address);

//extern int plc5_encode_tag_name(uint8_t *data, int *size, pccc_file_t *file_type, const char *name, int max_tag_name_size);
//extern int slc_encode_tag_name(uint8_t *data, int *size, pccc_file_t *file_type, const char *name, int max_tag_name_size);
extern uint8_t pccc_calculate_bcc(uint8_t *data,int size);
extern uint16_t pccc_calculate_crc16(uint8_t *data, int size);
extern const char *pccc_decode_error(uint8_t *error_ptr);
extern uint8_t *pccc_decode_dt_byte(uint8_t *data,int data_size, int *pccc_res_type, int *pccc_res_length);
extern int pccc_encode_dt_byte(uint8_t *data,int buf_size, uint32_t data_type, uint32_t data_size);



#endif
