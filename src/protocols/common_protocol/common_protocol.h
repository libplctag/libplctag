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

#include <lib/libplctag.h>
#include <lib/tag.h>
#include <platform.h>
#include <util/atomic_int.h>




typedef enum {
    TAG_OP_IDLE = 0,
    TAG_OP_READ_REQUEST,
    TAG_OP_READ_RESPONSE,
    TAG_OP_WRITE_REQUEST,
    TAG_OP_WRITE_RESPONSE
} tag_op_type_t;

typedef struct common_tag_t *common_tag_p;


struct common_tag_pdu_vtable_t {
    /* tag-specific behavior/functions */
    int (*create_read_request)(common_tag_p tag);
    int (*check_read_response)(common_tag_p tag);
    int (*create_write_request)(common_tag_p tag);
    int (*check_write_response)(common_tag_p tag);
};

/* common tag */

#define COMMON_TAG_FIELDS                           \
    TAG_BASE_STRUCT;                                \
    struct common_tag_t *next;                      \
    struct common_device_t *device;                 \
    struct common_tag_pdu_vtable_t *pdu_vtable;  \
    tag_op_type_t op

struct common_tag_t {
    COMMON_TAG_FIELDS;
};



struct common_tag_list_t {
    struct common_tag_t *head;
    struct common_tag_t *tail;
};

typedef struct common_tag_list_t *common_tag_list_p;

/* event return from wait_for_events() */
typedef enum {
    COMMON_DEVICE_EVENT_NONE = 0,
    COMMON_DEVICE_EVENT_ERROR = (1 << 1),
    COMMON_DEVICE_EVENT_TIMEOUT = (1 << 2),
    COMMON_DEVICE_EVENT_READ_READY = (1 << 3),
    COMMON_DEVICE_EVENT_WAKEUP = (1 << 4),
    COMMON_DEVICE_EVENT_WRITE_READY = (1 << 5),
    COMMON_DEVICE_EVENT_INACTIVE = (1 << 6)
};


typedef struct common_device_t *common_device_p;


struct common_protocol_vtable_t {
    int (*tickle)(common_device_p device);
    int (*connect)(common_device_p device);
    int (*disconnect)(common_device_p device);
    int (*process_pdu)(common_device_p device);
};

#define COMMON_DEVICE_FIELDS                    \
    struct common_device_t *next;               \
    char *server_name;                          \
    int connection_group_id;                    \
    sock_p sock;                                \
    int default_port;                           \
    cond_p cond_var;                            \
    thread_p handler_thread;                    \
    mutex_p mutex;                              \
    int state;                                  \
    atomic_int terminate;                       \
    struct common_protocol_vtable_t *protocol;  \
    struct common_tag_list_t tag_list;          \
    int pdu_ready_to_send;                      \
    int buffer_size;                            \
    int read_data_len;                          \
    int write_data_len;                         \
    int write_data_offset;                      \
    uint8_t *data


struct common_device_t {
    COMMON_DEVICE_FIELDS;
};


/* module functions */
extern int common_protocol_init(void);
extern void common_protocol_teardown(void);

/* device functions */
extern int common_protocol_get_device(const char *server_name, int default_port, int connection_group_id, common_device_p *device, common_device_p (*create_device)(void *arg), void *create_arg);
extern void common_device_destroy(common_device_p device);

/* tag functions. */
extern int common_tag_init(common_tag_p tag, attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata), void *userdata);
extern void common_tag_destroy(common_tag_p tag);

/* core tag vtable functions that are implemented here */
extern int common_tag_abort(plc_tag_p p_tag);
extern int common_tag_read_start(plc_tag_p p_tag);
extern int common_tag_status(plc_tag_p p_tag);
extern int common_tag_tickler(plc_tag_p p_tag);
extern int common_tag_write_start(plc_tag_p p_tag);
extern int common_tag_wake_device(plc_tag_p p_tag);


