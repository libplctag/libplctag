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
#include <util/attr.h>
#include <util/debug.h>

// #define PLCTAG_CANARY (0xACA7CAFE)
// #define PLCTAG_DATA_LITTLE_ENDIAN   (0)
// #define PLCTAG_DATA_BIG_ENDIAN      (1)


typedef struct plc_tag_t *plc_tag_p;


typedef int (*tag_vtable_func)(plc_tag_p tag);

/* we'll need to set these per protocol type. */
struct tag_vtable_t {
    tag_vtable_func abort;
    tag_vtable_func read;
    tag_vtable_func status;
    tag_vtable_func tickler;
    tag_vtable_func write;

    /* attribute accessors. */
    int (*get_int_attrib)(plc_tag_p tag, const char *attrib_name, int default_value);
    int (*set_int_attrib)(plc_tag_p tag, const char *attrib_name, int new_value);
};

typedef struct tag_vtable_t *tag_vtable_p;


/* byte ordering */

struct tag_byte_order_s {
    unsigned int int16_order_0:1;
    unsigned int int16_order_1:1;

    unsigned int int32_order_0:2;
    unsigned int int32_order_1:2;
    unsigned int int32_order_2:2;
    unsigned int int32_order_3:2;

    unsigned int float32_order_0:2;
    unsigned int float32_order_1:2;
    unsigned int float32_order_2:2;
    unsigned int float32_order_3:2;

    unsigned int int64_order_0:3;
    unsigned int int64_order_1:3;
    unsigned int int64_order_2:3;
    unsigned int int64_order_3:3;
    unsigned int int64_order_4:3;
    unsigned int int64_order_5:3;
    unsigned int int64_order_6:3;
    unsigned int int64_order_7:3;

    unsigned int float64_order_0:3;
    unsigned int float64_order_1:3;
    unsigned int float64_order_2:3;
    unsigned int float64_order_3:3;
    unsigned int float64_order_4:3;
    unsigned int float64_order_5:3;
    unsigned int float64_order_6:3;
    unsigned int float64_order_7:3;
};

typedef struct tag_byte_order_s tag_byte_order_t;



/*
 * The base definition of the tag structure.  This is used
 * by the protocol-specific implementations.
 *
 * The base type only has a vtable for operations.
 */

#define TAG_BASE_STRUCT tag_vtable_p vtable; \
                        int status; \
                        int size; \
                        uint8_t is_bit:1; \
                        uint8_t bit; \
                        int read_complete; \
                        int write_complete; \
                        int32_t tag_id; \
                        mutex_p ext_mutex; \
                        mutex_p api_mutex; \
                        tag_byte_order_t byte_order; \
                        void (*callback)(int32_t tag_id, int event, int status); \
                        uint8_t *data; \
                        int64_t read_cache_expire; \
                        int64_t read_cache_ms

// struct plc_tag_dummy {
//     int tag_id;
// };

struct plc_tag_t {
    TAG_BASE_STRUCT;
};

#define PLC_TAG_P_NULL ((plc_tag_p)0)


/* the following may need to be used where the tag is already mapped or is not yet mapped */
extern int lib_init(void);
extern void lib_teardown(void);
extern int plc_tag_abort_mapped(plc_tag_p tag);
extern int plc_tag_destroy_mapped(plc_tag_p tag);
extern int plc_tag_status_mapped(plc_tag_p tag);


