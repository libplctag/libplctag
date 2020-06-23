/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
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

#include <lib/libplctag.h>
#include <platform.h>
#include <lib/tag.h>
#include <mb/mb_common.h>
#include <mb/tag.h>
#include <util/attr.h>



#define MODBUS_DEFAULT_PORT (502)
#define PLC_BUF_SIZE (550)


struct mb_plc_t {
    struct mb_plc_t *next;

    /* state variable for state machine. */
    int state;

    /* mutex to control access. */
    mutex_p mutex;

    /* need a thread per PLC to prevent hangs on set up. */
    int terminate;
    thread_p thread;

    /* list of tags for this Modbus PLC. */
    struct mb_tag_t *tags;

    /* host and port for this PLC */
    const char *host;
    uint16_t port;

    /* socket and buffers */
    sock_p sock;
    uint32_t flags;
    int input_bytes_read;
    uint8_t in_buf[PLC_BUF_SIZE];
    int output_bytes_written;
    uint8_t out_buf[PLC_BUF_SIZE];
};

typedef struct mb_plc_t *mb_plc_p;

/* Entry points to the PLC module. */
extern mb_plc_p find_or_create_plc(attr attribs);
extern int plc_add_tag(mb_plc_p plc, mb_tag_p tag);
extern int plc_remove_tag(mb_plc_p plc, mb_tag_p tag);

