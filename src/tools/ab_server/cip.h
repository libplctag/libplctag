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

#include "plc.h"
#include <utils/byte_slice.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Handle one CIP request, routed by the class its path addresses and then by service.  unwrapped
 * is true for a request straight in SendRRData, the only place the Connection Manager services are
 * accepted.  Writes the reply at the start of out and returns what is left of out.
 */
extern byte_slice_t cip_dispatch_request(byte_slice_t in, byte_slice_t out, plc_s *plc, bool unwrapped);

/*
 * The Identity object's attributes as Get Attributes All returns them -- vendor, device type,
 * product code, revision, status, serial number and product name -- at the start of output.
 * ListIdentity carries the same bytes.  Returns what is left of out.
 */
extern byte_slice_t cip_encode_identity(byte_slice_t out, plc_s *plc);

/*
 * The symbolic segment at the start of path: sets *tag to the tag it names, or NULL, and returns
 * the rest of path after the segment and its pad.
 */
extern byte_slice_t cip_find_symbol_tag(byte_slice_t path, plc_s *plc, tag_def_s **tag);

/*
 * One request as the class handlers see it.  Requests are routed the way the CIP object model
 * does it: by the class the path addresses, then by service code within that class.
 */
typedef struct {
    uint8_t service;       /* without the reply bit */
    uint16_t class_id;     /* from the logical class segment, or the Symbol class for a symbolic path */
    uint32_t instance;     /* 0 when the path has no instance segment */
    byte_slice_t path;     /* what follows class/instance, or the whole symbolic path */
    byte_slice_t payload;
    bool unwrapped;        /* arrived straight in SendRRData, not connected and not in an Unconnected Send */
} cip_request_t;


/* tag commands */
#define CIP_SRV_GET_ATTRIBUTES_ALL ((uint8_t)0x01)
#define CIP_SRV_MULTI ((uint8_t)0x0a)
#define CIP_SRV_PCCC_EXECUTE ((uint8_t)0x4b)
#define CIP_SRV_READ_NAMED_TAG ((uint8_t)0x4c)
#define CIP_SRV_WRITE_NAMED_TAG ((uint8_t)0x4d)
/* the same code on different classes: routing by class keeps them apart. */
#define CIP_SRV_FORWARD_CLOSE ((uint8_t)0x4e)
#define CIP_SRV_RMW ((uint8_t)0x4e)
#define CIP_SRV_READ_NAMED_TAG_FRAG ((uint8_t)0x52)
#define CIP_SRV_UNCONNECTED_SEND ((uint8_t)0x52)
#define CIP_SRV_WRITE_NAMED_TAG_FRAG ((uint8_t)0x53)
#define CIP_SRV_FORWARD_OPEN ((uint8_t)0x54)
#define CIP_SRV_FORWARD_OPEN_EX ((uint8_t)0x5b)
#define CIP_SRV_OMRON_GET_INSTANCE_LIST ((uint8_t)0x5f)


#define CIP_DONE ((uint8_t)0x80)

#define CIP_SYMBOLIC_SEGMENT_MARKER ((uint8_t)0x91)

/* logical segments: an 8-bit class, then an 8- or 16-bit instance (the 16-bit one has a pad byte). */
#define CIP_SEG_CLASS_8 ((uint8_t)0x20)
#define CIP_SEG_CLASS_16 ((uint8_t)0x21)
#define CIP_SEG_INSTANCE_8 ((uint8_t)0x24)
#define CIP_SEG_INSTANCE_16 ((uint8_t)0x25)

/* CIP Errors */

#define CIP_OK ((uint8_t)0x00)
/* the Identity object, class 1 instance 1, is what an @identity tag asks for. */
#define CIP_CLASS_IDENTITY ((uint8_t)0x01)
#define CIP_IDENTITY_INSTANCE ((uint8_t)0x01)

/* the other classes served; Omron adds its two. */
#define CIP_CLASS_MESSAGE_ROUTER ((uint16_t)0x02)
#define CIP_CLASS_CONNECTION_MANAGER ((uint16_t)0x06)
#define CIP_CLASS_PCCC ((uint16_t)0x67)
#define CIP_CLASS_OMRON_TAG_NAME_SERVER ((uint16_t)0x6a)
#define CIP_CLASS_SYMBOL ((uint16_t)0x6b)
#define CIP_CLASS_OMRON_VARIABLE_TYPE ((uint16_t)0x6c)

#define CIP_ERR_EXT_ERR ((uint8_t)0x01)
#define CIP_ERR_INVALID_PARAM ((uint8_t)0x03)
#define CIP_ERR_PATH_SEGMENT ((uint8_t)0x04)
#define CIP_ERR_PATH_DEST_UNKNOWN ((uint8_t)0x05)
#define CIP_ERR_FRAG ((uint8_t)0x06)
#define CIP_ERR_UNSUPPORTED ((uint8_t)0x08)
#define CIP_ERR_REPLY_TOO_LARGE ((uint8_t)0x11)
#define CIP_ERR_INSUFFICIENT_DATA ((uint8_t)0x13)
#define CIP_ERR_TOO_MUCH_DATA ((uint8_t)0x15)
#define CIP_ERR_OBJECT_DOES_NOT_EXIST ((uint8_t)0x16)
#define CIP_ERR_EMBEDDED_SERVICE ((uint8_t)0x1e)

#define CIP_ERR_EX_DUPLICATE_CONN ((uint16_t)0x0100)
#define CIP_ERR_EX_LINK_OFFLINE ((uint16_t)0x0800)
#define CIP_ERR_EX_OUT_OF_CONNECTIONS ((uint16_t)0x0113)
#define CIP_ERR_EX_INVALID_CONN_SIZE ((uint16_t)0x0109)
#define CIP_ERR_EX_UC_SEND_PARAM ((uint16_t)0x0205)

#define CIP_RESPONSE_HEADER_SIZE ((size_t)4)

/* a CIP reply header: the service with the reply bit, the status and optional extended status. */
/*
 * The status for a request whose read chain failed: 0x15 if bytes were left over (TOO_LARGE
 * from byte_slice_end), 0x13 if it ran out.  A reply that does not fit is the caller's call.
 */
extern uint8_t cip_input_status(byte_slice_t failed);

extern byte_slice_t cip_write_reply_header(byte_slice_t out, uint8_t service, uint8_t status, bool extend, uint16_t extended_status);
