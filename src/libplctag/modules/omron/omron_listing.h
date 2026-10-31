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

#include <libplctag/modules/cip/tag.h>

/*
 * OMRON NJ/NX tag and UDT listing.  Both fill the buffers in the Logix layouts, so one decoder
 * reads either family:
 *
 * @tags   one entry per variable, user variables then system ones: instance (4), symbol type
 *         (2), element size (2), three dimensions (4 each), name length (2), then the name.
 *         The symbol type is the CIP type code, or 0x8000 plus the Variable Type Object
 *         instance for a structure, with the dimension count in bits 13-14.
 *
 * @udt/N  Variable Type Object instance N: UDT id (2), member description size in 32-bit
 *         words (4), size in bytes (4), member count (2), CRC (2); then per member its array
 *         count (2), type (2) and byte offset (4); then the UDT name and each member name, each
 *         zero terminated.
 */
extern int omron_setup_listing_tag(cip_tag_p tag);
extern int omron_setup_udt_tag(cip_tag_p tag, const char *name);
