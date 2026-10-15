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
 * The @identity tag body, shared by the CIP families.
 *
 * One Get_Attributes_All against the Identity object, sent however the connection sends
 * everything else.  Nothing in that is family-specific.  Each family still owns its vtable,
 * for its attribute table and the debug module ID callers filter on.
 */

#include <libplctag/lib/tag.h>
#include <libplctag/modules/cip/tag.h>
#include <utils/debug.h>


extern int cip_identity_tag_read_start(cip_tag_p tag);
extern int cip_identity_tag_tickler(cip_tag_p tag);
extern int cip_identity_tag_encode_read(cip_tag_p tag, uint8_t *dest, int32_t capacity, tag_op_plan_t *plan);
extern int cip_identity_tag_decode_read(cip_tag_p tag, uint8_t *reply, int32_t reply_size);

/* sets the type, byte order and vtable of a tag named @identity. */
extern int cip_identity_tag_setup(cip_tag_p tag, tag_vtable_p vtable, debug_module_t debug_module);
