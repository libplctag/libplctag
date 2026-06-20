/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever    *
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
#include "device.h"

/*
 * Parse argv into *dev and *debug_level_out.
 *
 * Returns PLCTAG_STATUS_OK on success.
 * Returns 1 if --help was requested (caller should exit(0)).
 * Returns a negative PLCTAG_ERR_* code on bad input.
 *
 * On success, dev->tags is a linked list of heap-allocated tag_def_t
 * structs (including initialized data_mutex). Free with args_free_tags.
 */
extern int32_t args_parse(int argc, char **argv, device_t *dev, int32_t *debug_level_out);

/* Free the tag list produced by args_parse (destroys mutexes, frees memory). */
extern void args_free_tags(tag_def_t *tags);

/* Print usage to stderr. */
extern void args_print_usage(const char *prog);
