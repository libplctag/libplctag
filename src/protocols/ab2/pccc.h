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
#include <util/slice.h>

typedef struct pccc_plc_s *pccc_plc_p;

typedef struct pccc_plc_request_s *pccc_plc_request_p;

struct pccc_plc_request_s {
    struct pccc_plc_request_s *next;
    void *tag;
    pccc_plc_p plc;
    int (*build_request_callback)(slice_t output_buffer, pccc_plc_p plc, void *tag);
    int (*handle_response_callback)(slice_t input_buffer, pccc_plc_p plc, void *tag);
};

extern pccc_plc_p pccc_plc_get(attr attribs);
extern int pccc_plc_request_init(pccc_plc_p pccc_plc, pccc_plc_request_p req);
extern int pccc_plc_request_start(pccc_plc_p plc, pccc_plc_request_p req, void *tag,
                                  int (*build_request_callback)(slice_t output_buffer, pccc_plc_p plc, void *tag),
                                  int (*handle_response_callback)(slice_t input_buffer, pccc_plc_p plc, void *tag));
extern int pccc_plc_request_abort(pccc_plc_p plc, pccc_plc_request_p req);

extern int pccc_plc_init(void);
extern int pccc_plc_teardown(void);
