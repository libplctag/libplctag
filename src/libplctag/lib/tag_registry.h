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

/*
 * The global tag-handle registry: the map from the int32_t handle the application
 * holds to the plc_tag_p behind it.
 */

/* Look up a tag by id, returning a NEW reference, or NULL if there is none. */
extern plc_tag_p lookup_tag(int32_t tag_id);

/* Assign the tag a fresh id and publish it in the registry. */
extern int add_tag_lookup(plc_tag_p tag);

/* Remove a tag from the registry.  The caller still holds its own reference. */
extern void remove_tag_lookup(plc_tag_p tag);

/* Remove a tag by id and return it, or NULL if no such tag is registered. */
extern plc_tag_p remove_tag_lookup_by_id(tag_registry_p registry, int32_t tag_id);

/* Tag ids are 28 bits, so a handle can never be mistaken for a pointer. */
#define TAG_ID_MASK (0xFFFFFFF)

/*
 * Lifecycle, registered as a row in lib_components[] (lib/init.c).
 *
 * init() builds the registry but deliberately does NOT publish it: no tag may
 * find it until every device module has started, or a module that fails to
 * start would leave a live tickler behind.  initialize_modules() calls
 * publish() once they have all succeeded, or discard_pending() if one did not.
 */
extern int tag_registry_init(void);
extern void tag_registry_teardown(void);
extern void tag_registry_publish(void);
extern void tag_registry_discard_pending(void);

/* Take a reference to the running registry, or NULL when it is not running. */
extern tag_registry_p tag_registry_acquire(void);

/*
 * Atomically close the gate: exactly one caller ever sees the registry non-NULL
 * and clears it, so repeated or concurrent plc_tag_shutdown() calls are safe.
 * Returns the reference it took, or NULL if the registry was already closed.
 */
extern tag_registry_p tag_registry_close(void);

/* Hand the closed registry to teardown, which drops the last reference. */
extern void tag_registry_stage_shutdown(tag_registry_p registry);

/* Destroy every tag still registered.  Only valid after tag_registry_close(). */
extern void tag_registry_destroy_all_tags(tag_registry_p registry);

/* Wake the tickler thread and wait for it to exit. */
extern void tag_registry_stop_tickler(tag_registry_p registry);
