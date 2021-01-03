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

#include <stdint.h>
#include <lib/libplctag.h>
#include <util/attr.h>


typedef int64_t plc_request_id;

#define INVALID_REQUEST_ID ((plc_request_id)-1)

typedef struct plc_s *plc_p;
typedef struct plc_layer_s *plc_layer_p;
typedef struct plc_request_s *plc_request_p;


struct plc_request_s {
    struct plc_request_s *next;
    void *context;
    plc_request_id req_id;
    int (*build_request)(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id req_num);
    int (*process_response)(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id req_num);
};


extern int plc_set_layer(plc_p plc,
                          int layer_index,
                          void *context,
                          int (*initialize)(void *context),
                          int (*connect)(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end),
                          int (*disconnect)(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end),
                          int (*prepare_for_request)(void *context),
                          int (*build_request)(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_num),
                          int (*prepare_for_response)(void *context),
                          int (*process_response)(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_num),
                          int (*destroy_layer)(void *context)
                        );

extern int plc_get(const char *plc_type, attr attribs, plc_p *plc, int (*constructor)(plc_p plc, attr attribs));
extern int plc_init(plc_p plc, int num_layers);

extern uint32_t plc_get_sequence_id(plc_p plc);

extern int plc_get_idle_timeout(plc_p plc);
extern int plc_set_idle_timeout(plc_p plc, int timeout_ms);

extern int plc_get_buffer_size(plc_p plc);
extern int plc_set_buffer_size(plc_p plc, int buffer_size);

extern int plc_start_request(plc_p plc,
                             plc_request_p request,
                             void *client,
                             int (*build_request_callback)(void *client, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id req_num),
                             int (*process_response_callback)(void *client, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id req_num));

extern int plc_stop_request(plc_p plc, plc_request_p request);

extern int plc_module_init(void);
extern void plc_module_teardown(void);



/* handy definitions. */

#define TRY_GET_BYTE(buffer, capacity, offset, val) if(offset < capacity) { (val) = buffer[offset]; } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++)

#define TRY_GET_U16_LE(buffer, capacity, offset, val) \
        if(offset < capacity) { (val) = (uint16_t)buffer[offset]; } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { (val) |= ((uint16_t)buffer[offset] << 8); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++)

#define TRY_GET_U32_LE(buffer, capacity, offset, val) \
        if(offset < capacity) { (val) = (uint32_t)buffer[offset]; } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { (val) |= ((uint32_t)buffer[offset] << 8); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { (val) |= ((uint32_t)buffer[offset] << 16); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { (val) |= ((uint32_t)buffer[offset] << 24); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++)

#define TRY_GET_U64_LE(buffer, capacity, offset, val) \
        if(offset < capacity) { (val) = (uint64_t)buffer[offset]; } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { (val) |= ((uint64_t)buffer[offset] << 8); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { (val) |= ((uint64_t)buffer[offset] << 16); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { (val) |= ((uint64_t)buffer[offset] << 24); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { (val) |= ((uint64_t)buffer[offset] << 32); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { (val) |= ((uint64_t)buffer[offset] << 40); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { (val) |= ((uint64_t)buffer[offset] << 48); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { (val) |= ((uint64_t)buffer[offset] << 56); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++) \

#define TRY_SET_BYTE(buffer, capacity, offset, val) if(offset < capacity) { buffer[offset] = (val); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++)

#define TRY_SET_U16_LE(buffer, capacity, offset, val) \
        if(offset < capacity) { buffer[offset] = (uint8_t)((val) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { buffer[offset] = (uint8_t)(((val) << 8) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++)

#define TRY_SET_U32_LE(buffer, capacity, offset, val) \
        if(offset < capacity) { buffer[offset] = (uint8_t)((val) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { buffer[offset] = (uint8_t)(((val) << 8) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { buffer[offset] = (uint8_t)(((val) << 16) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { buffer[offset] = (uint8_t)(((val) << 24) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++)

#define TRY_SET_U64_LE(buffer, capacity, offset, val) \
        if(offset < capacity) { buffer[offset] = (uint8_t)((val) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { buffer[offset] = (uint8_t)(((val) << 8) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { buffer[offset] = (uint8_t)(((val) << 16) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { buffer[offset] = (uint8_t)(((val) << 24) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { buffer[offset] = (uint8_t)(((val) << 32) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { buffer[offset] = (uint8_t)(((val) << 40) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { buffer[offset] = (uint8_t)(((val) << 48) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { buffer[offset] = (uint8_t)(((val) << 56) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++)



