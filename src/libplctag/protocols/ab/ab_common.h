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

#include <libplctag/lib/libplctag.h>
#include <stdbool.h>
#include <libplctag/lib/tag.h>
#include <libplctag/protocols/ab/defs.h>
#include <utils/vector.h>

typedef struct ab_tag_t *ab_tag_p;
#define AB_TAG_NULL ((ab_tag_p)NULL)

typedef struct cip_conn_t *cip_conn_p;
#define CIP_CONN_NULL ((cip_conn_p)NULL)

typedef struct ab_request_t *ab_request_p;
#define AB_REQUEST_NULL ((ab_request_p)NULL)

extern int ab_tag_abort_request_only(ab_tag_p tag);
extern int ab_tag_abort_request(ab_tag_p tag);
extern int ab_tag_abort(ab_tag_p tag);
extern int ab_tag_status(ab_tag_p tag);


/* Runtime attributes shared by every AB tag type. */
extern const attr_def_t ab_attribs[];

// extern int ab_get_bit(plc_tag_p tag, int offset_bit);
// extern int ab_set_bit(plc_tag_p tag, int offset_bit, int val);

// extern uint64_t ab_get_uint64(plc_tag_p tag, int offset);
// extern int ab_set_uint64(plc_tag_p tag, int offset, uint64_t val);

// extern int64_t ab_get_int64(plc_tag_p tag, int offset);
// extern int ab_set_int64(plc_tag_p tag, int offset, int64_t val);


// extern uint32_t ab_get_uint32(plc_tag_p tag, int offset);
// extern int ab_set_uint32(plc_tag_p tag, int offset, uint32_t val);

// extern int32_t ab_get_int32(plc_tag_p tag, int offset);
// extern int ab_set_int32(plc_tag_p tag, int offset, int32_t val);


// extern uint16_t ab_get_uint16(plc_tag_p tag, int offset);
// extern int ab_set_uint16(plc_tag_p tag, int offset, uint16_t val);

// extern int16_t ab_get_int16(plc_tag_p tag, int offset);
// extern int ab_set_int16(plc_tag_p tag, int offset, int16_t val);


// extern uint8_t ab_get_uint8(plc_tag_p tag, int offset);
// extern int ab_set_uint8(plc_tag_p tag, int offset, uint8_t val);

// extern int8_t ab_get_int8(plc_tag_p tag, int offset);
// extern int ab_set_int8(plc_tag_p tag, int offset, int8_t val);


// extern double ab_get_float64(plc_tag_p tag, int offset);
// extern int ab_set_float64(plc_tag_p tag, int offset, double val);

// extern float ab_get_float32(plc_tag_p tag, int offset);
// extern int ab_set_float32(plc_tag_p tag, int offset, float val);


// int ab_tag_destroy(ab_tag_p p_tag);
/*
 * One entry per PLC type: what its session needs and the tag constructor that finishes a tag
 * once the session exists.  plc= or cpu= picks the entry by one of its aliases.
 */
typedef struct cip_plc_type_t {
    plc_type_t id;
    const char *name;
    const char *const *aliases; /* NULL terminated */

    /* the session */
    int32_t max_msg_size;
    int32_t fo_conn_size;    /* the plain Forward Open size, and the unconnected one */
    int32_t fo_ex_conn_size; /* the largest Large Forward Open size; 0 for a PLC without it */

    /* tag defaults: 0 never connected, 1 always, -1 the use_connected_msg attribute, default on. */
    int8_t connected;
    bool allow_packing; /* the allow_packing attribute, default on; false means never. */
    bool needs_path;
    bool fragmented; /* has Read and Write Tag Fragmented (0x52, 0x53). */

    /* installs this type's connection state machine once the type is known. */
    void (*setup_conn)(cip_conn_p conn);

    /* the PLC-specific half of creating a tag.  from_src is set by plc_tag_create_from_tag(). */
    int (*tag_create)(ab_tag_p tag, attr attribs, bool from_src);
} cip_plc_type_t;

extern const cip_plc_type_t *ab_plc_type_find(attr attribs);
extern const cip_plc_type_t *ab_plc_type_get(plc_type_t id);
extern plc_type_t get_plc_type(attr attribs);

/* The PLC type an Identity reply names, or an error when the device is not a CPU we can use. */
extern int ab_plc_type_from_identity(uint16_t vendor_id, const char *product_name, bool has_path, plc_type_t *plc_type);
extern int check_tag_name(ab_tag_p tag, const char *name);
extern int check_mutex(int debug);
extern vector_p find_read_group_tags(ab_tag_p tag);

// THREAD_FUNC(request_handler_func);

/* helpers for checking request status. */
extern int check_request_status(ab_tag_p tag);
// extern int check_write_request_status(ab_tag_p tag);

#define rc_is_error(rc) (rc < PLCTAG_STATUS_OK)
