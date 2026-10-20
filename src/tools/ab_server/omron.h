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

#include <stdint.h>

#include "cip.h"
#include "plc.h"
#include <utils/byte_slice.h>

/*
 * The Omron NJ/NX services that are not plain Logix-style tag access: the Tag Name Server's
 * instance list (0x5F on class 0x6A), Get Attributes All on a variable or on the Variable Type
 * Object (class 0x6C), and reads and writes carrying a Simple Data Segment (0x80).  Each writes
 * its reply at the start of out and returns what is left of out.
 */
#define OMRON_SIMPLE_DATA_SEGMENT ((uint8_t)0x80)

extern byte_slice_t omron_get_instance_list(cip_request_t *req, byte_slice_t out, plc_s *plc);
extern byte_slice_t omron_variable_get_attributes_all(cip_request_t *req, byte_slice_t out, plc_s *plc);
extern byte_slice_t omron_variable_type_get_attributes_all(cip_request_t *req, byte_slice_t out, plc_s *plc);

/* a read or write of tag; segment is the Simple Data Segment at the end of the path. */
extern byte_slice_t omron_segment_access(cip_request_t *req, tag_def_s *tag, byte_slice_t segment, byte_slice_t out);
