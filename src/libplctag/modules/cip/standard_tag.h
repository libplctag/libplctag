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
 * The read/write engine for ordinary (symbolic, named) CIP tags.
 *
 * One implementation serves every CIP family.  The request shapes are the same CIP tag
 * services everywhere; what differs is which of them a family implements, and that is a
 * branch on the tag's plc_type, not a separate engine.
 *
 * Each family exports its own vtable because the abort entry point still differs.
 */

#include <libplctag/lib/tag.h>

/*
 * The three vtable entry points.  Each family builds its own vtable around these, because
 * the abort entry point and the attribute table are still the family's own.
 */
extern int cip_standard_tag_read_start(plc_tag_p tag_arg);
extern int cip_standard_tag_write_start(plc_tag_p tag_arg);
extern int cip_standard_tag_tickler(plc_tag_p tag_arg);
