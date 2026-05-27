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


static int enip_tag_abort(plc_tag_p p_tag) {
    enip_tag_t *tag = (enip_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    tag->op_state = ENIP_TAG_OP_ERROR;
    tag->read_in_flight = 0;
    tag->write_in_flight = 0;

    return PLCTAG_STATUS_OK;
}


static int enip_tag_read(plc_tag_p p_tag) {
    enip_tag_t *tag = (enip_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    tag->op_state = ENIP_TAG_OP_REQUEST;
    tag->read_in_flight = 1;
    tag->read_complete = 0;

    return PLCTAG_STATUS_PENDING;
}


static int enip_tag_status(plc_tag_p p_tag) {
    enip_tag_t *tag = (enip_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    if(tag->status != PLCTAG_STATUS_OK) { return tag->status; }

    if(tag->read_in_flight || tag->write_in_flight) { return PLCTAG_STATUS_PENDING; }

    return PLCTAG_STATUS_OK;
}


static int enip_tag_tickler(plc_tag_p p_tag) {
    enip_tag_t *tag = (enip_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    return tag->status;
}


static int enip_tag_write(plc_tag_p p_tag) {
    enip_tag_t *tag = (enip_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    tag->op_state = ENIP_TAG_OP_REQUEST;
    tag->write_in_flight = 1;
    tag->write_complete = 0;

    return PLCTAG_STATUS_PENDING;
}


static int enip_tag_wake_plc(plc_tag_p p_tag) {
    enip_tag_t *tag = (enip_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    return PLCTAG_STATUS_PENDING;
}


static int enip_tag_data_written(plc_tag_p p_tag) {
    enip_tag_t *tag = (enip_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    return PLCTAG_STATUS_OK;
}


static void enip_tag_destructor(void *ptr) {
    enip_tag_t *tag = (enip_tag_t *)ptr;

    if(!tag) { return; }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "ENIP tag destructor.");
}


static struct tag_vtable_t enip_tag_vtable = {
    .abort = enip_tag_abort,
    .read = enip_tag_read,
    .status = enip_tag_status,
    .tickler = enip_tag_tickler,
    .write = enip_tag_write,
    .wake_plc = enip_tag_wake_plc,
    .tag_data_written = enip_tag_data_written,
};


plc_tag_p enip_protocol_tag_create(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                   void *userdata, plc_tag_p src_tag) {
    enip_tag_t *tag = (enip_tag_t *)rc_alloc(sizeof(enip_tag_t), enip_tag_destructor);
    int rc;

    (void)src_tag;

    if(!tag) { return NULL; }

    tag->vtable = &enip_tag_vtable;
    tag->protocol_type = TAG_PROTOCOL_ENIP;
    tag->metadata_required = 1;

    rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        tag->status = (int8_t)rc;
        rc_dec(tag);
        return NULL;
    }

    tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, PLCTAG_STATUS_OK);

    return (plc_tag_p)tag;
}
