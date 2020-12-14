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

struct protocol_s {
    /* request API */
    int (*protocol_request_create)(struct protocol_request_s **request, struct protocol_s *protocol, void *requestor);
    int (*protocol_request_start)(struct protocol_request_s *request,
                                  int (*request_callback)(struct protocol_s *protocol,
                                                          struct protocol_request_s *request,
                                                          void *requestor,
                                                          int64_t current_time,
                                                          slice_t packet));
    int (*protocol_request_abort)(struct protocol_request_s *request);
    int (*protocol_request_destroy)(struct protocol_request_s **request);

    /* protocol layer API up the stack*/
    int (*protocol_can_read)(struct protocol_s *protocol);
    int (*protocol_can_write)(struct protocol_s *protocol);
    int (*protocol_closing)(struct protocol_s *protocol);

    /* protocol layer API down the stack */
    int (*protocol_)

    /* protocol clean up */
    int (*protocol_destroy)(struct protocol_s *protocol);
};
typedef struct protocol_s *protocol_p;

struct protocol_request_s {
    int (*request_callback)(struct protocol_s *protocol,
                            struct protocol_request_s *request,
                            void *requestor,
                            int64_t current_time,
                            int event, slice_t output_buffer);
};
typedef struct protocol_request_s *protocol_request_p;

