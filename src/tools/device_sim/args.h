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

#include <stddef.h>
#include <stdint.h>

/*
 * Parsed CLI options.  Tag specs are borrowed pointers into argv (the string
 * following each "--tag="), not copied; caller owns argv's lifetime for as
 * long as sim_args_t is in use.
 */
typedef struct {
    const char *plc_type;   /* raw --plc= value, or NULL for the default */
    const char *model;      /* raw --model= value, or NULL */
    const char *bind_addr;  /* raw --bind= value, or NULL for all interfaces */
    uint16_t    port;
    int32_t     delay_ms;
    const char *tag_specs[64];
    int         num_tags;
} sim_args_t;

/*
 * Parse argv into a sim_args_t.
 *
 * Returns PLCTAG_STATUS_OK on success.
 * Returns 1 if --help was requested (caller should exit(0)).
 * Returns a negative PLCTAG_ERR_* code on bad input.
 */
extern int32_t args_parse(int argc, char **argv, sim_args_t *args_out, int32_t *debug_level_out);

/* Print usage to stderr. */
extern void args_print_usage(const char *prog);

/*
 * Build a plc_tag_create() attribute string for one --tag= spec (CIP:
 * "Name:TYPE[dims]", or PCCC: "B<n>[count]") plus the shared endpoint
 * options.  Returns PLCTAG_STATUS_OK or PLCTAG_ERR_BAD_PARAM.
 */
extern int32_t args_build_tag_attr_str(const sim_args_t *args, const char *spec, char *out, size_t out_cap);
