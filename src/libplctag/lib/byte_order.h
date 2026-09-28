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

#include <libplctag/lib/tag.h>
#include <utils/attr.h>

/*
 * Upper bound on the string size attributes (str_max_capacity, str_total_length,
 * str_pad_bytes).
 *
 * These come from the application's attribute string and used to accept anything up to
 * INT_MAX.  They are stored as unsigned int and then summed to validate str_total_length,
 * so two large values wrapped that sum and let a nonsensical string definition through.
 * Real string formats are tiny -- a Logix STRING is 82 characters plus a 4-byte count --
 * so this cap is far above anything legitimate while keeping the sum from overflowing.
 */
#define MAX_STR_SIZE_PARAM (65536)

/*
 * Apply the byte-order and string-format attributes from the attribute string to the
 * tag.  Allocates tag->byte_order when the tag needs one of its own.
 */
extern int set_tag_byte_order(plc_tag_p tag, attr attribs);
