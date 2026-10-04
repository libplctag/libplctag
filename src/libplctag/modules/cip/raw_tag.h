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

/*
 * The @raw tag body, shared by the CIP families.
 *
 * A raw tag is a CIP request the application built itself.  The library copies it onto
 * the wire verbatim, adds the EIP and CPF framing, and hands the whole CIP response back.
 * Nothing in that is family-specific, so nothing here is.
 *
 * Each family still owns its vtable: the abort entry point and the attribute table are
 * its own, and so is the debug module ID callers filter on.
 */

#include <libplctag/lib/tag.h>
#include <libplctag/modules/cip/tag.h>
#include <utils/debug.h>


/*
 * A raw tag's byte order.
 *
 * Plain CIP: little-endian throughout, and no string definition at all.  The payload is
 * whatever the application put there, so the library has no business claiming to know how
 * a string inside it is shaped.  An application that does know says so with the str_*
 * attributes, which lib.c applies over this.
 */
extern tag_byte_order_t cip_raw_tag_byte_order;


/* the two vtable entry points; the family supplies the rest of its vtable. */
extern int cip_raw_tag_tickler(cip_tag_p tag);
extern int cip_raw_tag_write_start(cip_tag_p tag);

/* set a freshly created tag up as a raw tag of this family. */
extern int cip_raw_tag_setup(cip_tag_p tag, tag_vtable_p vtable, debug_module_t debug_module);
