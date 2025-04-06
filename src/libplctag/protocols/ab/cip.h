/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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

#ifndef __LIBPLCTAG_AB_CIP_H__
#define __LIBPLCTAG_AB_CIP_H__

#include <libplctag/lib/libplctag.h>
#include <libplctag/protocols/ab/ab_common.h>
#include <libplctag/protocols/ab/defs.h>


// int cip_encode_path(ab_tag_p tag, const char *path);
extern int cip_encode_path(const char *path, int *needs_connection, plc_type_t plc_type, uint8_t *tmp_conn_path,
                           int *tmp_conn_path_size, int *is_dhp, uint16_t *dhp_dest);

//~ char *cip_decode_status(int status);
extern int cip_encode_tag_name(ab_tag_p tag, const char *name);

/* look up the type size in bytes based on the first byte */
extern int cip_lookup_encoded_type_size(uint8_t type_byte, int *type_size);

/* look up the element size in bytes based on the first byte */
extern int cip_lookup_data_element_size(uint8_t type_byte, int *element_size);

#endif
