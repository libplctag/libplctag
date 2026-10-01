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

#ifndef __LIBPLCTAG_AB_PCCC_H__
#define __LIBPLCTAG_AB_PCCC_H__


#include <libplctag/lib/tag.h>
#include <libplctag/modules/cip/tag.h>
#include <platform.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <utils/byteorder.h>

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

// extern int plc5_encode_tag_name(uint8_t *data, int *size, pccc_file_t *file_type, const char *name, int max_tag_name_size);
// extern int slc_encode_tag_name(uint8_t *data, int *size, pccc_file_t *file_type, const char *name, int max_tag_name_size);
extern const char *pccc_decode_error(uint8_t *error_ptr, size_t error_size);
extern uint8_t *pccc_decode_dt_byte(uint8_t *data, int data_size, int *pccc_res_type, int *pccc_res_length);
extern int pccc_encode_dt_byte(uint8_t *data, int buf_size, uint32_t data_type, uint32_t data_size);

/* generic direct ethernet tag functions */
extern int pccc_check_response_header(cip_tag_p tag, bool is_dhp);
extern int pccc_tag_status(cip_tag_p tag);
extern int pccc_tag_tickler(cip_tag_p tag);
extern int pccc_tag_read_start(cip_tag_p tag);
extern int pccc_tag_write_start(cip_tag_p tag);

/*
 * The whole PCCC reply as it arrives once the connection has stripped the EIP and CPF
 * framing: the CIP reply header and PCCC matching info above, then the PCCC command trailer.
 * This is pccc_resp from modules/cip/wire.h less its 40 bytes of framing.
 */
START_PACK typedef struct {
    uint8_t reply_code;
    uint8_t reserved;
    uint8_t general_status;
    uint8_t status_size;

    uint8_t request_id_size;
    uint16_le vendor_id;
    uint32_le vendor_serial_number;

    uint8_t pccc_command;
    uint8_t pccc_status;
    uint16_le pccc_seq_num;
} END_PACK cip_pccc_full_resp;

/* four byte CIP reply header, seven bytes of PCCC matching info, four byte PCCC command. */
_Static_assert(sizeof(cip_pccc_full_resp) == 15, "cip_pccc_full_resp wire size changed");


/*
 * One vtable for PLC/5, SLC and MicroLogix, plain or over a DH+ bridge.  The PLC family is
 * in tag->plc_type and the bridge in tag->session->is_dhp; the functions above read both.
 */
extern struct tag_vtable_t pccc_vtable;

#endif
