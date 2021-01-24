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

#include <stddef.h>
#include <stdint.h>


typedef uint16_t tag_type_t;

/* CIP data types. */
#define TAG_CIP_TYPE_SINT        ((tag_type_t)0x00C2) /* Signed 8–bit integer value */
#define TAG_CIP_TYPE_INT         ((tag_type_t)0x00C3) /* Signed 16–bit integer value */
#define TAG_CIP_TYPE_DINT        ((tag_type_t)0x00C4) /* Signed 32–bit integer value */
#define TAG_CIP_TYPE_LINT        ((tag_type_t)0x00C5) /* Signed 64–bit integer value */
#define TAG_CIP_TYPE_USINT       ((tag_type_t)0x00C6) /* Unsigned 8–bit integer value */
#define TAG_CIP_TYPE_UINT        ((tag_type_t)0x00C7) /* Unsigned 16–bit integer value */
#define TAG_CIP_TYPE_UDINT       ((tag_type_t)0x00C8) /* Unsigned 32–bit integer value */
#define TAG_CIP_TYPE_ULINT       ((tag_type_t)0x00C9) /* Unsigned 64–bit integer value */
#define TAG_CIP_TYPE_REAL        ((tag_type_t)0x00CA) /* 32–bit floating point value, IEEE format */
#define TAG_CIP_TYPE_LREAL       ((tag_type_t)0x00CB) /* 64–bit floating point value, IEEE format */
#define TAG_CIP_TYPE_STRING      ((tag_type_t)0x00D0) /* 88-byte string, with 82 bytes of data, 4-byte count and 2 bytes of padding */

/* PCCC data types.   FIXME */
#define TAG_PCCC_TYPE_INT         ((uint8_t)0x89) /* Signed 16–bit integer value */
#define TAG_PCCC_TYPE_DINT        ((uint8_t)0x91) /* Signed 32–bit integer value */
#define TAG_PCCC_TYPE_REAL        ((uint8_t)0x8a) /* 32–bit floating point value, IEEE format */
#define TAG_PCCC_TYPE_STRING      ((uint8_t)0x8d) /* 82-byte string with 2-byte count word. */

struct tag_def_s {
    struct tag_def_s *next_tag;
    char *name;
    tag_type_t tag_type;
    size_t elem_size;
    size_t elem_count;
    size_t data_file_num;
    size_t num_dimensions;
    size_t dimensions[3];
    uint8_t *data;
};

typedef struct tag_def_s tag_def_s;

typedef enum {
    PLC_CONTROL_LOGIX,
    PLC_MICRO800,
    PLC_OMRON,
    PLC_PLC5,
    PLC_SLC,
    PLC_MICROLOGIX
} plc_type_t;

/* Define the context that is passed around. */
typedef struct {
    plc_type_t plc_type;
    uint8_t path[20];
    uint8_t path_len;

    /* connection info. */
    uint32_t session_handle;
    uint64_t sender_context;
    uint32_t server_connection_id;
    uint16_t server_connection_seq;
    uint32_t server_to_client_rpi;
    uint32_t client_connection_id;
    uint16_t client_connection_seq;
    uint16_t client_connection_serial_number;
    uint16_t client_vendor_id;
    uint32_t client_serial_number;
    uint32_t client_to_server_rpi;

    uint32_t client_to_server_max_packet;
    uint32_t server_to_client_max_packet;

    /* PCCC info */
    uint16_t pccc_seq_id;

    /* debugging. */
    int reject_fo_count;

    /* list of tags served by this "PLC" */
    struct tag_def_s *tags;
} plc_s;

