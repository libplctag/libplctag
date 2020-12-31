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

#include <util/attr.h>
#include <util/mutex.h>
#include <util/slice.h>

typedef struct protocol_s *protocol_p;
struct protocol_s {
    struct protocol_s *next;

    mutex_p mutex;
    protocol_request_p request_list;

    const char *protocol_key;

    /* called just before adding the new request to the queue. */
    int (*handle_new_request_callback)(protocol_p protocol, protocol_request_p request);

    /* called just after a request is removed from the queue. */
    int (*handle_cleanup_request_callback)(protocol_p protocol, protocol_request_p request);
};

typedef struct protocol_request_s *protocol_request_p;
struct protocol_request_s {
    struct protocol_request *next;
    void *client;
    int (*build_request_callback)(protocol_p protocol, void *client, slice_t output_buffer, slice_t *used_buffer);
    int (*process_response_callback)(protocol_p protocol, void *client, slice_t input_buffer, slice_t *used_buffer);
};

extern int protocol_get(const char *protocol_key, attr attribs, protocol_p *protocol, int (*constructor)(const char *protocol_key, attr attribs, protocol_p *protocol));
extern int protocol_init(protocol_p protocol,
                         const char *protocol_key,
                         int (*handle_new_request_callback)(protocol_p protocol, protocol_request_p request),
                         int (*handle_cleanup_request_callback)(protocol_p protocol, protocol_request_p request));
extern int protocol_cleanup(protocol_p protocol);

/* request handling from the client. */
extern int protocol_request_init(protocol_p protocol, protocol_request_p req);
extern int protocol_start_request(protocol_p protocol,
                                  protocol_request_p request,
                                  void *client,
                                  int (*build_request_callback)(protocol_p protocol, void *client, slice_t output_buffer, slice_t *used_buffer),
                                  int (*process_response_callback)(protocol_p protocol, void *client, slice_t input_buffer, slice_t *used_buffer));
extern int protocol_stop_request(protocol_p plc, protocol_request_p req);



/* request handling from the protocol layer itself. */
extern int protocol_build_request(protocol_p protocol,
                                  slice_t output_buffer,
                                  slice_t *used_slice,
                                  int (*build_request_result_callback)(protocol_p protocol, protocol_request_p request, int status, slice_t used_slice, slice_t *new_output_slice));
extern int protocol_process_response(protocol_p protocol,
                                     slice_t input_slice,
                                     slice_t *used_slice,
                                     int (*process_response_result_callback)(protocol_p protocol, protocol_request_p request, int status, slice_t used_slice, slice_t *new_input_slice));


/* global module set up and teardown. */
extern int protocol_module_init(void);
extern void protocol_module_teardown(void);
