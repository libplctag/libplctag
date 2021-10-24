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
    /* set if we allocated this specifically for the tag. */
    unsigned int is_allocated:1;

    /* string type and ordering. */
    unsigned int str_is_defined:1;
    unsigned int str_is_counted:1;
    unsigned int str_is_fixed_length:1;
    unsigned int str_is_zero_terminated:1;
    unsigned int str_is_byte_swapped:1;

    unsigned int str_count_word_bytes;
    unsigned int str_max_capacity;
    unsigned int str_total_length;
    unsigned int str_pad_bytes;

    int int16_order[2];
    int int32_order[4];
    int int64_order[8];

    int float32_order[4];
    int float64_order[8];
};


typedef struct tag_byte_order_s tag_byte_order_t;




/*
 * The base definition of the tag structure.  This is used
 * by the protocol-specific implementations.
 *
 * The base type only has a vtable for operations.
 */

#define TAG_BASE_STRUCT uint8_t is_bit:1; \
                        uint8_t tag_is_dirty:1; \
                        uint8_t read_in_flight:1; \
                        uint8_t read_complete:1; \
                        uint8_t write_in_flight:1; \
                        uint8_t write_complete:1; \
                        uint8_t bit; \
                        int8_t status; \
                        int32_t size; \
                        int32_t tag_id; \
                        int32_t auto_sync_read_ms; \
                        int32_t auto_sync_write_ms; \
                        uint8_t *data; \
                        tag_byte_order_t *byte_order; \
                        mutex_p ext_mutex; \
                        mutex_p api_mutex; \
                        cond_p tag_cond_wait; \
                        tag_vtable_p vtable; \
                        void (*callback)(int32_t tag_id, int event, int status); \
                        int64_t read_cache_expire; \
                        int64_t read_cache_ms; \
                        int64_t auto_sync_next_read; \
                        int64_t auto_sync_next_write



struct plc_tag_t {
    TAG_BASE_STRUCT;
};

#define PLC_TAG_P_NULL ((plc_tag_p)0)


/* the following may need to be used where the tag is already mapped or is not yet mapped */
extern int lib_init(void);
extern void lib_teardown(void);
// extern int plc_tag_abort_mapped(plc_tag_p tag);
// extern int plc_tag_destroy_mapped(plc_tag_p tag);
// extern int plc_tag_status_mapped(plc_tag_p tag);
extern int plc_tag_tickler_wake_impl(const char *func, int line_num);
#define plc_tag_tickler_wake()  plc_tag_tickler_wake_impl(__func__, __LINE__)
extern int init_generic_tag(plc_tag_p tag);