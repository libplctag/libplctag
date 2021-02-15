/***************************************************************************
 *   Copyright (C) 2021 by Kyle Hayes                                      *
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

#include <stdbool.h>
#include <stdint.h>
#include <lib/tag.h>
#include <ab2/ab.h>
#include <util/atomic_int.h>
#include <util/attr.h>
#include <util/plc.h>

#define CIP_CMD_GET_ATTRIBS      ((uint8_t)0x03)
#define CIP_CMD_MULTI            ((uint8_t)0x0A)
#define CIP_CMD_READ             ((uint8_t)0x4C)
#define CIP_CMD_READ_TEMPLATE    ((uint8_t)0x4C)
#define CIP_CMD_WRITE            ((uint8_t)0x4D)
#define CIP_CMD_RMW              ((uint8_t)0x4E)
#define CIP_CMD_READ_FRAG        ((uint8_t)0x52)
#define CIP_CMD_WRITE_FRAG       ((uint8_t)0x53)
#define CIP_CMD_LIST_TAGS        ((uint8_t)0x55)

/* flag set when command is OK */
#define CIP_CMD_OK                   ((uint8_t)0x80)

#define CIP_STATUS_OK                ((uint8_t)0x00)
#define CIP_STATUS_FRAG              ((uint8_t)0x06)

#define CIP_ERR_UNSUPPORTED_SERVICE  ((uint8_t)0x08)
#define CIP_ERR_PARTIAL_ERROR        ((uint8_t)0x1e)

#define CIP_MAX_PAYLOAD (60000)
#define CIP_STD_EX_PAYLOAD (4002)
#define CIP_STD_PAYLOAD (508)
#define CIP_MIN_PAYLOAD (92)


typedef struct {
    struct plc_tag_t base_tag;

    int elem_size;
    int elem_count;

    /* data type info */
    uint8_t tag_type_info[4];
    uint8_t tag_type_info_length;
    bool is_raw_tag;

    /* tag encoded name */
    int encoded_name_length;
    uint8_t *encoded_name;

    /* plc and request info */
    plc_p plc;
    struct plc_request_s request;

    /* count of bytes sent or received. */
    uint32_t trans_offset;
} cip_tag_t;
typedef cip_tag_t *cip_tag_p;


extern plc_tag_p cip_tag_create(ab2_plc_type_t plc_type, attr attribs);
extern int cip_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value);
extern int cip_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value);

/* generic CIP functions */
extern int ab2_cip_encode_tag_name(const char *name, uint8_t **encoded_tag_name, int *encoded_name_length, bool *is_bit, uint8_t *bit_num);
extern int ab2_cip_encode_path(uint8_t *data, int data_capacity, int *data_offset, const char *path, bool *is_dhp, uint8_t *target_dhp_port, uint8_t *target_dhp_id);

/* generic CIP error translation */
extern const char *cip_decode_error_short(uint8_t err_status, uint16_t extended_err_status);
extern const char *cip_decode_error_long(uint8_t err_status, uint16_t extended_err_status);
extern int cip_decode_error_code(uint8_t err_status, uint16_t extended_err_status);

