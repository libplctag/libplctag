/***************************************************************************
 *   Copyright (C) 2022 by Kyle Hayes                                      *
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

#include <platform.h>
#include <lib/libplctag.h>
#include <lib/tag.h>
#include <util/atomic_int.h>


/*
 * This defines the operations a tag can do.
 */

typedef enum {
    TAG_OP_IDLE = 0,
    TAG_OP_READ_REQUEST,
    TAG_OP_READ_RESPONSE,
    TAG_OP_WRITE_REQUEST,
    TAG_OP_WRITE_RESPONSE
} tag_op_t;

/*
 * a base type for all tags that use this device framework.
 */

typedef struct device_tag_s *device_tag_p;
struct device_tag_s {
    TAG_BASE_STRUCT;

    struct device_tag_s *next;

    tag_op_t op;
};
typedef struct device_tag_s device_tag_t;

/*
 * vtable for devices.   These need to be overridden by each device type.
 */
struct device_vtable_s {
    int (*initialize)(...);
    int (*terminate)(device_p device);
    int (*destroy)(device_p device);
    int (*wakeup)(device_p device);
    int (*add_tag)(device_p device, device_tag_p tag);
    int (*remove_tag)(device_p device, device_tag_p tag);

    /* provided by protocol/device implementation */
    int (*encode_read_request)(device_p device, device_tag_p tag);
    int (*decode_read_response)(device_p device, device_tag_p tag);
};
typedef struct device_vtable_s device_vtable_t;

typedef struct device_s *device_p;
struct device_s {
    device_p next;

    /* device identifying info */
    const char *device_name;
    int connection_pool_id;

    /* thread and protection */
    thread_p thread;
    mutex_p mutex;
    atomic_int terminating;

    /* tag list */
    device_tag_p tags_head, tags_tail;
    device_tag_p end_tag;

    /* device functions */
    device_vtable_t vtable;
};
typedef struct device_s device_t;

/* 
 * Each module needs to be defined by a struct like this. This 
 * then needs to be exported.
 */
struct device_module_s {
    int (*initialize_module)(void);
    int (*teardown_module)(void);
    int (*create_device)(attr attributes, device_p *device);
    int (*add_device)(device_p device);
    int (*remove_device)(device_p device);
    int (*create_tag)(attr attributes, device_tag_p *tag);
};
typedef struct device_module_s device_module_t;
typedef device_module_t *device_module_p;


/* globally visible entry point for all devices */
extern device_module_t device_module;
