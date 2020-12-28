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
#include <util/attr.h>
#include <util/slice.h>

typedef struct pccc_plc_s *pccc_plc_p;

typedef struct pccc_plc_request_s *pccc_plc_request_p;

struct pccc_plc_request_s {
    struct pccc_plc_request_s *next;
    void *tag;
    pccc_plc_p plc;
    slice_t (*build_request_callback)(slice_t output_buffer, pccc_plc_p plc, void *tag);
    int (*handle_response_callback)(slice_t input_buffer, pccc_plc_p plc, void *tag);
};

#define PCCC_CMD_OK ((uint8_t)(0x40))
#define PCCC_TYPED_CMD ((uint8_t)(0x0F))

extern pccc_plc_p pccc_plc_get(attr attribs);
extern int pccc_plc_request_init(pccc_plc_p pccc_plc, pccc_plc_request_p req);
extern int pccc_plc_request_start(pccc_plc_p plc, pccc_plc_request_p req, void *tag,
                                  slice_t (*build_request_callback)(slice_t output_buffer, pccc_plc_p plc, void *tag),
                                  int (*handle_response_callback)(slice_t input_buffer, pccc_plc_p plc, void *tag));
extern int pccc_plc_request_abort(pccc_plc_p plc, pccc_plc_request_p req);
extern uint16_t pccc_plc_get_tsn(pccc_plc_p plc);


typedef enum { PCCC_FILE_UNKNOWN        = 0x00, /* UNKNOWN! */
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

extern int pccc_plc_parse_logical_address(const char *name, pccc_file_t *file_type, int *file_num, int *elem_num, int *sub_elem_num);
extern const char *pccc_plc_decode_error(slice_t err);

extern int pccc_plc_init(void);
extern void pccc_plc_teardown(void);
