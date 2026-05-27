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

#include <libplctag/protocols/enip/enip.h>
#include <libplctag/protocols/enip/tag.h>
#include <utils/debug.h>
#include <utils/rc.h>


static int enip_connection_abort(plc_tag_p p_tag) {
    (void)p_tag;
    return PLCTAG_STATUS_OK;
}


static int enip_connection_status(plc_tag_p p_tag) {
    enip_connection_tag_t *tag = (enip_connection_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    return tag->status;
}


static int enip_connection_tickler(plc_tag_p p_tag) {
    enip_connection_tag_t *tag = (enip_connection_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    return PLCTAG_STATUS_OK;
}


static int enip_connection_get_int_attrib(plc_tag_p p_tag, const char *attrib_name, int default_value) {
    enip_connection_tag_t *tag = (enip_connection_tag_t *)p_tag;

    if(!tag || !attrib_name) { return default_value; }

    if(str_cmp_i(attrib_name, "queue_depth") == 0) { return tag->queue_depth; }

    if(str_cmp_i(attrib_name, "callback_latency_last_ms") == 0) { return tag->callback_latency_last_ms; }

    if(str_cmp_i(attrib_name, "callback_latency_max_ms") == 0) { return tag->callback_latency_max_ms; }

    return default_value;
}


static void enip_connection_tag_destructor(void *ptr) {
    enip_connection_tag_t *tag = (enip_connection_tag_t *)ptr;

    if(!tag) { return; }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "ENIP connection tag destructor.");
}


static struct tag_vtable_t enip_connection_tag_vtable = {
    .abort = enip_connection_abort,
    .status = enip_connection_status,
    .tickler = enip_connection_tickler,
    .get_int_attrib = enip_connection_get_int_attrib,
};


plc_tag_p enip_connection_tag_create(attr attribs,
                                     void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                     void *userdata, plc_tag_p src_tag) {
    enip_connection_tag_t *tag = (enip_connection_tag_t *)rc_alloc(sizeof(enip_connection_tag_t), enip_connection_tag_destructor);
    int rc;

    (void)src_tag;

    if(!tag) { return NULL; }

    tag->vtable = &enip_connection_tag_vtable;
    tag->protocol_type = TAG_PROTOCOL_ENIP_CONNECTION;

    rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        tag->status = (int8_t)rc;
        rc_dec(tag);
        return NULL;
    }

    tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, PLCTAG_STATUS_OK);

    return (plc_tag_p)tag;
}
