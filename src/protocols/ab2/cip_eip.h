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
#include <util/attr.h>
#include <util/protocol.h>
#include <util/slice.h>

typedef struct cip_eip_s *cip_eip_p;


#define CIP_CMD_OK ((uint8_t)(0x80))

extern protocol_p cip_eip_get(attr attribs);

extern int cip_eip_get_dhp_dest(protocol_p plc);

//extern int cip_encode_path(const char *path, bool needs_connection, bool *has_dhp, uint8_t *dhp_dest, slice_t buffer);
extern int cip_encode_tag_name(const char *name, slice_t buffer);

extern const char *cip_decode_error_short(uint8_t *data);
extern const char *cip_decode_error_long(uint8_t *data);
extern int cip_decode_error_code(uint8_t *data);
