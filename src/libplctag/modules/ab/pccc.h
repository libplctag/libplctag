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

extern const char *pccc_decode_error(uint8_t *error_ptr, size_t error_size);

/* the library error for the same PCCC STS/EXT STS bytes pccc_decode_error() describes. */
extern int pccc_status_to_rc(uint8_t *error_ptr, size_t error_size);
extern uint8_t *pccc_decode_dt_byte(uint8_t *data, int data_size, int *pccc_res_type, int *pccc_res_length);

/*
 * The front of a PCCC request carried by the Execute PCCC service, and the PCCC command
 * blocks that follow it.
 *
 * These live here rather than in pccc.c because PCCC-mapped Logix uses the same request
 * prefix and the same command block, with its own function codes and its own payload.
 */
START_PACK typedef struct {
    /* PCCC Command Req Routing */
    uint8_t service_code;           /* ALWAYS 0x4B, Execute PCCC */
    uint8_t req_path_size;          /* ALWAYS 0x02, in 16-bit words */
    uint8_t req_path[4];            /* ALWAYS 0x20,0x67,0x24,0x01 for PCCC */
    uint8_t request_id_size;        /* ALWAYS 7 */
    uint16_le vendor_id;            /* Our CIP Vendor ID */
    uint32_le vendor_serial_number; /* Our CIP Vendor Serial Number */
} END_PACK cip_pccc_req;

START_PACK typedef struct {
    /* PCCC Command */
    uint8_t pccc_command;         /* CMD read, write etc. */
    uint8_t pccc_status;          /* STS 0x00 in request */
    uint16_le pccc_seq_num;       /* TNS transaction/sequence id */
    uint8_t pccc_function;        /* FNC sub-function of command */
    uint16_le pccc_offset;        /* offset of requested in total request */
    uint16_le pccc_transfer_size; /* total number of words requested */
} END_PACK plc5_pccc_read_cmd_req;

START_PACK typedef struct {
    /* PCCC Command */
    uint8_t pccc_command;         /* CMD read, write etc. */
    uint8_t pccc_status;          /* STS 0x00 in request */
    uint16_le pccc_seq_num;       /* TNS transaction/sequence id */
    uint8_t pccc_function;        /* FNC sub-function of command */
    uint16_le pccc_offset;        /* offset of requested in total request */
    uint16_le pccc_transfer_size; /* total number of words to be written */
} END_PACK plc5_pccc_write_cmd_req;


/*
 * What sits in front of a PCCC command: the Execute PCCC service and our requester ID, or a
 * DH+ routing header.  A PCCC family's setup picks one, so no codec asks which it is.
 */
typedef struct pccc_framing_t {
    int32_t prefix_size;    /* request bytes before the PCCC command */
    int32_t reply_overhead; /* reply bytes up to and including the PCCC command response */
    void (*write_prefix)(cip_tag_p tag, uint8_t *dest);

    /* the framing's own reply checks: service, length, and who the reply is from. */
    int (*check_route)(cip_tag_p tag, uint8_t *reply, int32_t reply_size);
} pccc_framing_t;

extern const pccc_framing_t pccc_cip_framing;
extern const pccc_framing_t pccc_dhp_framing;

/* the framing's checks, then that the reply carries the TNS of the request we sent. */
extern int pccc_check_response_header(cip_tag_p tag, uint8_t *reply, int32_t reply_size);

/* the codecs' sent: draw the TNS and write it into the request going on the wire. */
extern void pccc_sent(cip_tag_p tag, uint8_t *request);

/*
 * Check that a request needing overhead + payload bytes fits what the session will carry.
 * Compares before subtracting, so a connection too small for the overhead cannot wrap.
 */
extern int pccc_check_payload_space(cip_tag_p tag, size_t overhead, size_t payload);


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


/* PLC/5, and SLC 500 with MicroLogix, each plain or over a DH+ bridge. */
extern struct tag_vtable_t plc5_vtable;
extern const cip_tag_codec_t plc5_codec;
extern struct tag_vtable_t slc_vtable;
extern const cip_tag_codec_t slc_codec;



/*
 * PCCC command and function codes.
 *
 * PCCC is Rockwell's own protocol, carried inside CIP by the Execute PCCC service.  These
 * are its codes, not CIP's.
 */
#define AB_EIP_CMD_PCCC_EXECUTE ((uint8_t)0x4B) /* the CIP service that carries a PCCC message */

#define AB_EIP_PCCC_TYPED_CMD ((uint8_t)0x0F)
#define AB_EIP_PLC5_RANGE_READ_FUNC ((uint8_t)0x01)
#define AB_EIP_PLC5_RANGE_WRITE_FUNC ((uint8_t)0x00)
#define AB_EIP_PLC5_RMW_FUNC ((uint8_t)0x26)
#define AB_EIP_PCCCLGX_TYPED_READ_FUNC ((uint8_t)0x68)
#define AB_EIP_PCCCLGX_TYPED_WRITE_FUNC ((uint8_t)0x67)
#define AB_EIP_SLC_RANGE_READ_FUNC ((uint8_t)0xA2)
#define AB_EIP_SLC_RANGE_WRITE_FUNC ((uint8_t)0xAA)
#define AB_EIP_SLC_RANGE_WRITE_MASK_FUNC ((uint8_t)0xAB)

/* PCCC data file types, as they appear in a typed read or write. */
#define AB_PCCC_DATA_ARRAY 9
