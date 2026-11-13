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
 * Rockwell tag and UDT listing, as a ControlLogix or Micro800 answers it and as the library's
 * @tags and @udt/N tags ask for it.  A Micro800 does not list its BOOL arrays.  Each writes its reply at the start of out and returns what
 * is left of out.
 *
 *   Get Instance Attribute List (0x55) on the Symbol object (class 0x6B).
 *   Get Attribute List (0x03) and Read Template (0x4C) on the Template object (class 0x6C).
 */
#define CIP_SRV_GET_ATTRIBUTE_LIST ((uint8_t)0x03)
#define CIP_SRV_LIST_TAGS ((uint8_t)0x55)
#define CIP_CLASS_TEMPLATE ((uint16_t)0x6c)

extern byte_slice_t logix_list_tags(cip_request_t *req, byte_slice_t out, plc_s *plc);
extern byte_slice_t logix_template_get_attribute_list(cip_request_t *req, byte_slice_t out, plc_s *plc);
extern byte_slice_t logix_template_read(cip_request_t *req, byte_slice_t out, plc_s *plc);
