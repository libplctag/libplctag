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

#pragma once

#include "plc.h"
#include "slice.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern slice_s cip_dispatch_request(slice_s input, slice_s output, plc_s *context);
extern slice_s cip_dispatch_unconnected_request(slice_s input, slice_s output, plc_s *context);

/*
 * The Identity object's attributes as Get Attributes All returns them -- vendor, device type,
 * product code, revision, status, serial number and product name -- at the start of output.
 * ListIdentity carries the same bytes.  Returns the size written, 0 if it does not fit.
 */
extern size_t cip_encode_identity(slice_s output, plc_s *plc);

/* a CIP response header for cip_cmd with status cip_err, plus two bytes of extended status when extend is set. */
extern slice_s make_cip_log_error(slice_s output, uint8_t cip_cmd, uint8_t cip_err, bool extend, uint16_t extended_error);

/*
 * The tag named by the symbolic segment at the start of path, or NULL.  *end_offset is the
 * first byte after the segment and its pad.
 */
extern tag_def_s *cip_find_symbol_tag(slice_s path, plc_s *plc, size_t *end_offset);

/* true when path is exactly a logical class segment for class_id and an 8- or 16-bit instance. */
extern bool cip_path_is_class(slice_s path, uint8_t class_id, uint32_t *instance);


/* tag commands */
#define CIP_SRV_GET_ATTRIBUTES_ALL ((uint8_t)0x01)
#define CIP_SRV_MULTI ((uint8_t)0x0a)
#define CIP_SRV_PCCC_EXECUTE ((uint8_t)0x4b)
#define CIP_SRV_READ_NAMED_TAG ((uint8_t)0x4c)
#define CIP_SRV_WRITE_NAMED_TAG ((uint8_t)0x4d)
#define CIP_SRV_FORWARD_CLOSE ((uint8_t)0x4e)
/*
 * The same code is Read-Modify-Write when the path addresses a tag rather than the
 * Connection Manager object.  The two never collide here: a Forward Close arrives at the
 * unconnected dispatcher, an RMW at the connected one, wrapped in an Unconnected_Send or a
 * connected CPF item.
 */
#define CIP_SRV_RMW ((uint8_t)0x4e)
#define CIP_SRV_READ_NAMED_TAG_FRAG ((uint8_t)0x52)
#define CIP_SRV_UNCONNECTED_SEND ((uint8_t)0x52)
#define CIP_SRV_WRITE_NAMED_TAG_FRAG ((uint8_t)0x53)
#define CIP_SRV_FORWARD_OPEN ((uint8_t)0x54)
#define CIP_SRV_INSTANCES_ATTRIBS ((uint8_t)0x55)
#define CIP_SRV_FORWARD_OPEN_EX ((uint8_t)0x5b)


#define CIP_DONE ((uint8_t)0x80)

#define CIP_SYMBOLIC_SEGMENT_MARKER ((uint8_t)0x91)

/* logical segments: an 8-bit class, then an 8- or 16-bit instance (the 16-bit one has a pad byte). */
#define CIP_SEG_CLASS_8 ((uint8_t)0x20)
#define CIP_SEG_INSTANCE_8 ((uint8_t)0x24)
#define CIP_SEG_INSTANCE_16 ((uint8_t)0x25)

/* CIP Errors */

#define CIP_OK ((uint8_t)0x00)
/* the Identity object, class 1 instance 1, is what an @identity tag asks for. */
#define CIP_CLASS_IDENTITY ((uint8_t)0x01)
#define CIP_IDENTITY_INSTANCE ((uint8_t)0x01)

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
#define CIP_ERR_EXTENDED ((uint8_t)0xff)

#define CIP_ERR_EX_DUPLICATE_CONN ((uint16_t)0x0100)
#define CIP_ERR_EX_LINK_OFFLINE ((uint16_t)0x0800)
#define CIP_ERR_EX_CONN_SIZE ((uint16_t)0x0109)
#define CIP_ERR_EX_INVALID_CONN_SIZE ((uint16_t)0x0109)
#define CIP_ERR_EX_TOO_LONG ((uint16_t)0x2105)
#define CIP_ERR_EX_UC_SEND_PARAM ((uint16_t)0x0205)

#define CIP_RESPONSE_HEADER_SIZE ((size_t)4)
#define CIP_RESPONSE_HEADER_EXT_ERR_SIZE ((size_t)6)
