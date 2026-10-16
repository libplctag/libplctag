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
 * The composition root for CIP tag creation: the one place that knows both the Rockwell and
 * the OMRON modules, so neither has to know the other.  It lives in modules/cip because what
 * it dispatches is CIP and nothing else, but it is the one file here that sits *above* the
 * families rather than below them -- the rest of modules/cip names no family and must keep it
 * that way.
 *
 * It picks a family (cip/family.h) from the tag string or the Identity object, finds or makes
 * the connection, and sets the tag up with the family that connection holds.
 */

#include <libplctag/lib/tag.h>
#include <libplctag/modules/cip/family.h>
#include <stdbool.h>
#include <stdint.h>
#include <utils/attr.h>


/*
 * Whether a tag of this family uses a CIP connection, from the family and, for the one family
 * that allows it, the tag string.
 *
 * Both the connection and the tag need this answer, and the connection needs it first, so it
 * is resolved here instead of inside the family's setup.
 */
extern bool cip_family_use_connected_msg(const cip_family_t *family, attr attribs);

/*
 * The family for a device, from its Identity vendor ID and product name.  A device that is not
 * recognised gets generic CIP.  A chassis module that is not a CPU is refused with
 * PLCTAG_ERR_BAD_PARAM: the path stops short of the CPU, or there is no path at all.
 */
extern int cip_family_from_identity(uint16_t vendor_id, const char *product_name, bool has_path,
                                    const cip_family_t **family_out);

/*
 * The family the plc (or legacy cpu) attribute names, or NULL when it names none or one this
 * library does not speak.  The family is the direct one; a DH+ path picks its DH+ form when
 * the connection is created.
 */
extern const cip_family_t *cip_family_from_attributes(attr attribs);

/*
 * The constructor the library dispatches protocol=ab-eip to.
 *
 * It settles the family and the connection, then sets the tag up with the family the
 * connection holds.  Registered in lib/init.c's tag_type_map.
 */
extern int cip_tag_create(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                          void *userdata, plc_tag_p src_tag, plc_tag_p *tag_out);
