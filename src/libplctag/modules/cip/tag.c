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

/*
 * Tag operations shared by every CIP family.
 *
 * Aborting an in-flight request is the same job whatever the PLC is: flag the request so
 * the connection drops it, hand back the tag's reference to it, and clear the in-progress
 * flags.  Both families had their own copy of this; they had drifted in wording and in
 * one case in locking shape, but not in effect.
 */

#include <inttypes.h>
#include <libplctag/api/libplctag.h>
#include <libplctag/modules/cip/conn.h>
#include <libplctag/modules/cip/path.h>
#include <libplctag/lib/conn_attribs.h>
#include <libplctag/modules/cip/tag.h>
#include <stdbool.h>
#include <stdint.h>
#include <platform.h>
#include <utils/atomic_utils.h>
#include <utils/debug.h>
#include <utils/rc.h>
#include <utils/vector.h>

/* Default byte order for short strings */
tag_byte_order_t cip_short_string_byte_order = {.is_allocated = 0,

                                                .int16_order = {0, 1},
                                                .int32_order = {0, 1, 2, 3},
                                                .int64_order = {0, 1, 2, 3, 4, 5, 6, 7},
                                                .float32_order = {0, 1, 2, 3},
                                                .float64_order = {0, 1, 2, 3, 4, 5, 6, 7},

                                                .str_is_defined = 1,
                                                .str_is_counted = 1,
                                                .str_is_fixed_length = 0,
                                                .str_is_zero_terminated = 0,
                                                .str_is_byte_swapped = 0,

                                                .str_pad_to_multiple_bytes = 1,
                                                .str_count_word_bytes = 1,
                                                .str_max_capacity = 255,
                                                .str_total_length = 0,
                                                .str_pad_bytes = 0};


/*
 * Abort the request this tag has in flight, if any, and leave the tag idle.
 *
 * tag->offset is deliberately untouched: a fragmented transfer calls this between
 * fragments and has to keep its place.  cip_tag_abort_request() is the one that resets it.
 */

int cip_tag_abort_request_only(cip_tag_p tag) {
    if(!tag) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Called with a null tag pointer.");
        return PLCTAG_STATUS_OK;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "Starting.");

    if(tag->req) {
        cip_request_p req = NULL;

        critical_block(tag->api_mutex) { req = rc_inc(tag->req); }

        if(req) {
            spin_block(&req->lock) { atomic_set_int32(&req->abort_request, 1); }

            critical_block(tag->api_mutex) {
                if(tag->req == req) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id,
                           "rc_dec: Releasing tag-owned reference to request of tag %" PRId32 ".", tag->tag_id);
                    tag->req = NULL;
                    rc_dec(req);
                } else {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id,
                           "Request changed out from underneath us during abort process!");
                }
            }

            /* release this function's own reference. */
            req = rc_dec(req);
        }
    } else {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "Called without a request in flight.");
    }

    tag->read_in_progress = 0;
    tag->write_in_progress = 0;

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}


/* Abort the in-flight request and discard any partial transfer with it. */
int cip_tag_abort_request(cip_tag_p tag) {
    if(!tag) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Called with a null tag pointer.");
        return PLCTAG_STATUS_OK;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "Starting.");

    tag->offset = 0;

    cip_tag_abort_request_only(tag);

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * Collect the result of the tag's in-flight request, if it has finished.
 *
 * Returns PLCTAG_STATUS_PENDING while the request is still out, PLCTAG_STATUS_OK when
 * there is nothing in flight or the reply is ready to be parsed, and the request's own
 * error otherwise.  The caller owns tag->status: this never writes it, because it returns
 * OK for an idle tag and that would erase the result of the operation that just finished.
 */
int cip_check_request_status(cip_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    cip_request_p request = NULL;

    /* check early for null pointers */
    if(!tag) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Called with null tag pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    do {
        /* do we have an abort outstanding? */
        if(atomic_get_bool(&tag->abort_requested)) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, tag->tag_id, "Abort requested on tag %" PRId32 ".", tag->tag_id);
            cip_tag_abort_request(tag);
            atomic_set_bool(&tag->abort_requested, false);
            rc = PLCTAG_ERR_ABORT;
            break;
        }

        /* make sure the request cannot be pulled out from underneath us. */
        if(tag->req) {
            critical_block(tag->api_mutex) { request = rc_inc(tag->req); }
        }

        /* it was already gone. */
        if(!request) {
            if(tag->read_in_progress || tag->write_in_progress) {
                tag->read_in_progress = 0;
                tag->write_in_progress = 0;
                tag->offset = 0;

                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "A request was in progress, but no request in flight!");
            }

            rc = PLCTAG_STATUS_OK;
            break;
        }

        /* request can be used by more than one thread at once. */
        spin_block(&request->lock) {
            if(!request->resp_received) {
                rc = PLCTAG_STATUS_PENDING;
                break;
            }

            /* check to see if it was an abort on the session side. */
            if(request->status != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "Request completed with error status %s.",
                       plc_tag_decode_error(request->status));
                rc = request->status;
                break;
            }
        }

        /* if we failed above, punt out of the do/while loop. */
        if(rc != PLCTAG_STATUS_OK) { break; }


        /*
         * The connection has already checked the EIP status and the CPF layer and stripped
         * them, so what is left here is the CIP reply and there is no framing to inspect.
         * The tag's own status checker parses it from the first byte.
         */
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "Received a %d byte CIP response.", request->request_size);
    } while(0);

    /* if this is still hanging around, release the reference */
    if(request) { request = rc_dec(request); }

    if(rc_is_error(rc)) {
        /* the request is dead, from session side. */
        cip_tag_abort_request(tag);

        pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, tag->tag_id, "Response not OK with status %s.", plc_tag_decode_error(rc));
    }

    /* FIXME - This is not correct */
    // tag->status = (int8_t)rc;

    pdebug(DEBUG_MODULE_CIP, DEBUG_SPEW, tag->tag_id, "Done with tag status %s.", plc_tag_decode_error(rc));

    return rc;
}


/*
 * Encode a tag's symbolic name into tag->encoded_name as a CIP path.
 *
 * The grammar and the encoding are cip_encode_name()'s; this only moves the tag's fields
 * in and the results back out.  is_bit is a bitfield, so it cannot be written through a
 * pointer and has to be copied by hand.
 */
int cip_encode_tag_name(cip_tag_p tag, const char *name) {
    cip_name_t ctx = {.tag_id = tag->tag_id,
                      .elem_count = tag->elem_count,
                      .encoded_name = &tag->encoded_name[0],
                      .encoded_name_capacity = (int)sizeof(tag->encoded_name),
                      .encoded_name_size = 0,
                      .bit = tag->bit,
                      .is_bit = (tag->is_bit ? true : false)};
    int rc = cip_encode_name(&ctx, name);

    tag->encoded_name_size = ctx.encoded_name_size;
    tag->bit = ctx.bit;
    tag->is_bit = (ctx.is_bit ? (uint8_t)1 : (uint8_t)0);

    return rc;
}


int cip_setup_special_tag(cip_tag_p tag, cip_elem_type_t elem_type, tag_byte_order_t *byte_order, tag_vtable_p vtable,
                          debug_module_t debug_module) {
    pdebug(debug_module, DEBUG_DETAIL, tag->tag_id, "Starting.");

    tag->debug_module = debug_module;
    tag->special_tag = 1;
    tag->elem_type = elem_type;
    tag->elem_count = 1;
    tag->elem_size = 1;

    tag->byte_order = byte_order;

    pdebug(debug_module, DEBUG_DETAIL, tag->tag_id, "Setting vtable to %p.", vtable);

    tag->vtable = vtable;

    pdebug(debug_module, DEBUG_DETAIL, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * The vtable a tag gets before its family has filled one in.  Reaching one of these means
 * a tag type was created without its own implementation, which is a bug in that family's
 * tag_create rather than anything the caller did.
 */
int cip_tag_unimplemented_abort(plc_tag_p tag) {
    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "This should be overridden by a PLC-specific function!");

    return PLCTAG_ERR_NOT_IMPLEMENTED;
}


int cip_tag_unimplemented_read(plc_tag_p tag) {
    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "This should be overridden by a PLC-specific function!");

    return PLCTAG_ERR_NOT_IMPLEMENTED;
}


int cip_tag_unimplemented_status(plc_tag_p tag) {
    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "This should be overridden by a PLC-specific function!");

    return PLCTAG_ERR_NOT_IMPLEMENTED;
}


int cip_tag_unimplemented_tickler(plc_tag_p tag) {
    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "This should be overridden by a PLC-specific function!");

    return PLCTAG_ERR_NOT_IMPLEMENTED;
}


int cip_tag_unimplemented_write(plc_tag_p tag) {
    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "This should be overridden by a PLC-specific function!");

    return PLCTAG_ERR_NOT_IMPLEMENTED;
}


int cip_tag_status(cip_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    if(tag->read_in_progress || tag->write_in_progress) { return PLCTAG_STATUS_PENDING; }

    rc = tag->status;

    return rc;
}


/*
 * The tag vtable's abort entry point: stop whatever is in flight and leave the tag
 * reporting PLCTAG_ERR_ABORT.
 *
 * Distinct from the two internal helpers above.  cip_tag_abort_request_only() drops the
 * in-flight request and clears the direction flags; cip_tag_abort_request() also rewinds
 * the transfer offset; this one is what a caller's plc_tag_abort() reaches, and it is the
 * only one that touches tag->status.
 *
 * It aborts even when no request is in flight.  A tag can be left with a direction flag
 * set and nothing on the wire -- that is exactly the state an abort exists to clear -- so
 * returning early would leave the caller unable to recover it.
 */
int cip_tag_abort(cip_tag_p tag) {
    cip_request_p req = NULL;

    if(!tag) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Called with a null tag pointer.");
        return PLCTAG_STATUS_OK;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "Starting.");

    critical_block(tag->api_mutex) { req = rc_inc(tag->req); }

    if(req) {
        spin_block(&req->lock) { atomic_set_int32(&req->abort_request, 1); }

        cip_tag_abort_request(tag);

        req = rc_dec(req);
    } else {
        cip_tag_abort_request(tag);
    }

    tag->status = PLCTAG_ERR_ABORT;

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "Done.");

    return tag->status;
}


/*
 * The tag attributes every CIP family answers the same way.  They all read a cip_tag_t
 * and the cip_conn_t behind it, so there is nothing family-specific left in them; each
 * family's attribute table points straight at these.
 */
int32_t cip_tag_get_elem_size(plc_tag_p raw_tag, int32_t *result) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    *result = (int32_t)tag->elem_size;

    return PLCTAG_STATUS_OK;
}


int32_t cip_tag_get_elem_count(plc_tag_p raw_tag, int32_t *result) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    *result = (int32_t)tag->elem_count;

    return PLCTAG_STATUS_OK;
}


int32_t cip_tag_get_connection_status(plc_tag_p raw_tag, int32_t *result) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    return conn_get_status(tag->session ? &tag->session->watch.status : NULL, result);
}


int32_t cip_tag_get_connection_inactivity_timeout_ms(plc_tag_p raw_tag, int32_t *result) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    return conn_get_inactivity_timeout_ms(tag->session ? &tag->session->connection_inactivity_timeout_ms : NULL, result);
}


int32_t cip_tag_set_connection_inactivity_timeout_ms(plc_tag_p raw_tag, int32_t value) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    return conn_set_inactivity_timeout_ms(tag->session ? &tag->session->connection_inactivity_timeout_ms : NULL, value,
                                          tag->tag_id, DEBUG_MODULE_CIP);
}




int32_t cip_tag_get_use_connected_msg(plc_tag_p raw_tag, int32_t *result) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    *result = (int32_t)tag->use_connected_msg;

    return PLCTAG_STATUS_OK;
}


int32_t cip_tag_get_allow_packing(plc_tag_p raw_tag, int32_t *result) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    *result = (int32_t)tag->allow_packing;

    return PLCTAG_STATUS_OK;
}


int32_t cip_tag_get_gateway(plc_tag_p raw_tag, uint8_t *buffer, int32_t buffer_length) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    if(!tag->session) { return PLCTAG_ERR_NOT_FOUND; }

    return attr_copy_string(tag->session->host, buffer, buffer_length);
}


int32_t cip_tag_get_gateway_size(plc_tag_p raw_tag) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    if(!tag->session) { return PLCTAG_ERR_NOT_FOUND; }

    return attr_string_size(tag->session->host);
}


int32_t cip_tag_get_gateway_port(plc_tag_p raw_tag, int32_t *result) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    if(!tag->session) { return PLCTAG_ERR_NOT_FOUND; }

    *result = (int32_t)tag->session->port;

    return PLCTAG_STATUS_OK;
}


int32_t cip_tag_get_path(plc_tag_p raw_tag, uint8_t *buffer, int32_t buffer_length) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    if(!tag->session) { return PLCTAG_ERR_NOT_FOUND; }

    if((int32_t)tag->session->conn_path_size > buffer_length) { return PLCTAG_ERR_TOO_SMALL; }

    mem_copy((void *)buffer, (void *)tag->session->conn_path, (int)tag->session->conn_path_size);

    return (int32_t)tag->session->conn_path_size;
}


int32_t cip_tag_get_path_size(plc_tag_p raw_tag) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    if(!tag->session) { return PLCTAG_ERR_NOT_FOUND; }

    return (int32_t)tag->session->conn_path_size;
}


int32_t cip_tag_get_conn_only_use_old_forward_open(plc_tag_p raw_tag, int32_t *result) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    if(!tag->session) { return PLCTAG_ERR_NOT_FOUND; }

    *result = (int32_t)tag->session->only_use_old_forward_open;

    return PLCTAG_STATUS_OK;
}
