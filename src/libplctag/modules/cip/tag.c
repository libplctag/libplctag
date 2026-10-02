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
#include <libplctag/modules/cip/tag.h>
#include <stdbool.h>
#include <stdint.h>
#include <platform.h>
#include <utils/atomic_utils.h>
#include <utils/debug.h>
#include <utils/rc.h>
#include <utils/vector.h>


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
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "Received a %d byte CIP response.",
               request->request_size);
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
