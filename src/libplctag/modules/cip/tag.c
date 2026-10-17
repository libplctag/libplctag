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
#include <libplctag/modules/cip/services.h>
#include <libplctag/lib/conn_attribs.h>
#include <libplctag/lib/tag.h>
#include <libplctag/modules/cip/tag.h>
#include <stdbool.h>
#include <stdint.h>
#include <platform.h>
#include <utils/atomic_utils.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/rc.h>
#include <utils/vector.h>


static void cip_tag_destroy(cip_tag_p tag);
static int cip_tag_send(cip_tag_p tag);


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
 * Release everything a CIP tag owns.
 *
 * Called by the refcount machinery when the last reference goes, so it must tolerate being
 * handed a tag that never finished being built -- every field is checked before it is freed.
 * It must also tolerate a null tag, which is why nothing is logged with tag->tag_id before
 * that test: the Omron copy this replaced dereferenced the pointer in its first pdebug and
 * again inside the null branch itself.
 */
static void cip_tag_destroy(cip_tag_p tag) {
    cip_conn_p conn = NULL;

    if(!tag) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Tag pointer is null!");
        return;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, tag->tag_id, "Starting.");

    /* abort anything in flight */
    cip_tag_abort(tag);

    conn = tag->session;

    /* a tag should always have a connection.  Release it. */
    if(conn) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "rc_dec: Releasing connection reference of tag %" PRId32 ".",
               tag->tag_id);
        tag->session = rc_dec(conn);
    } else {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "No connection pointer!");
    }

    if(tag->ext_mutex) {
        mutex_destroy(&(tag->ext_mutex));
        tag->ext_mutex = NULL;
    }

    if(tag->api_mutex) {
        mutex_destroy(&(tag->api_mutex));
        tag->api_mutex = NULL;
    }

    if(tag->tag_cond_wait) {
        cond_destroy(&(tag->tag_cond_wait));
        tag->tag_cond_wait = NULL;
    }

    if(tag->byte_order && tag->byte_order->is_allocated) {
        mem_free(tag->byte_order);
        tag->byte_order = NULL;
    }

    if(tag->data) {
        mem_free(tag->data);
        tag->data = NULL;
    }

    if(tag->deferred_attribs) {
        attr_destroy(tag->deferred_attribs);
        tag->deferred_attribs = NULL;
    }

    if(tag->instance) {
        rc_dec(tag->instance);
        tag->instance = NULL;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, tag->tag_id, "Done.");
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
    tag->special_tag = true;
    tag->elem_type = elem_type;
    tag->elem_count = 1;
    tag->elem_size = 1;
    tag->create_read_status = PLCTAG_STATUS_PENDING;

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


int cip_tag_unimplemented_write(plc_tag_p tag) {
    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "This should be overridden by a PLC-specific function!");

    return PLCTAG_ERR_NOT_IMPLEMENTED;
}


void cip_tag_finish_create(cip_tag_p tag, int rc) {
    if(tag->create_read_status != PLCTAG_STATUS_PENDING) { return; }

    if(rc != PLCTAG_STATUS_OK) {
        cip_tag_fail_create(tag, (plctag_error_code_t)rc);
        return;
    }

    tag->create_read_status = PLCTAG_STATUS_OK;
    tag_created((plc_tag_p)tag, rc);
    plc_tag_generic_wake_tag((plc_tag_p)tag);
}


void cip_tag_fail_create(cip_tag_p tag, plctag_error_code_t rc) {
    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "Create failed with %s.", plc_tag_decode_error(rc));

    tag->create_read_status = rc;
    tag->status = (int8_t)rc;

    /* the create read is over; ending it queues CREATED. */
    tag_op_end((plc_tag_p)tag, rc);
    tag_created((plc_tag_p)tag, rc);

    /* a sync create may be waiting; it should see the answer now, not when its timeout ends. */
    plc_tag_generic_wake_tag((plc_tag_p)tag);
}


int cip_data_tag_abort(cip_tag_p tag) {
    /* the create's error must survive, so there is nothing left to abort. */
    if(tag->create_read_status < PLCTAG_STATUS_OK) { return PLCTAG_STATUS_OK; }

    if(tag->create_read_status == PLCTAG_STATUS_PENDING) { cip_tag_fail_create(tag, PLCTAG_ERR_ABORT); }

    /* the abort itself worked; ABORT is the tag's status, not this call's result. */
    cip_tag_abort(tag);

    return PLCTAG_STATUS_OK;
}


int cip_tag_status(cip_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    if(tag->create_read_status < PLCTAG_STATUS_OK) { return tag->create_read_status; }

    if(tag->create_read_status == PLCTAG_STATUS_PENDING) { return PLCTAG_STATUS_PENDING; }

    if(tag_op_busy(tag)) { return PLCTAG_STATUS_PENDING; }

    rc = tag->status;

    return rc;
}


/*
 * The tag vtable's abort entry point: stop whatever is in flight and leave the tag
 * reporting PLCTAG_ERR_ABORT.
 *
 * The connection forgets the tag first, under its own mutex, so the handler cannot be part way
 * through encoding or decoding for it when the flags below change.  It aborts even when nothing
 * is in flight: a tag can be left with an op state set and nothing queued -- that is
 * exactly the state an abort exists to clear.
 */
int cip_tag_abort(cip_tag_p tag) {
    if(!tag) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Called with a null tag pointer.");
        return PLCTAG_STATUS_OK;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "Starting.");

    if(tag->session) { cip_conn_forget_tag(tag->session, tag); }

    tag_op_set(tag, TAG_OP_IDLE);
    tag->offset = 0;
    tag->status = PLCTAG_ERR_ABORT;

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "Done.");

    return tag->status;
}


/*
 * Hand the tag to its connection, whose handler encodes the request the tag's read or write
 * op state calls for, sends it, and decodes the reply with the vtable's encode_* and decode_*.
 * The caller has started the op (tag_op_start()) first.
 */
static int cip_tag_send(cip_tag_p tag) {
    /* a tag the library is still building waits for activate to queue it. */
    if(!tag->activated) { return PLCTAG_STATUS_OK; }

    return cip_conn_queue_tag(tag->session, tag);
}


int cip_tag_activate(plc_tag_p tag_arg) {
    cip_tag_p tag = (cip_tag_p)tag_arg;

    tag->activated = true;

    /* the handler starts what the create started while the tag was being built, and arms auto-sync. */
    if(!tag->session) { return PLCTAG_STATUS_OK; }

    return cip_conn_queue_tag(tag->session, tag);
}


/*
 * The vtable's wake_plc and tag_data_written: something about the tag changed that its
 * handler has to look at -- an abort request, a new auto-sync period, data to write.  Making
 * the tag due now has the handler run the library's housekeeping for it.
 */
int cip_tag_wake_plc(plc_tag_p tag_arg) {
    cip_tag_p tag = (cip_tag_p)tag_arg;

    if(!tag->activated || !tag->session) { return PLCTAG_STATUS_OK; }

    return cip_conn_queue_tag(tag->session, tag);
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


int32_t cip_tag_get_connection_retry_max_delay_ms(plc_tag_p raw_tag, int32_t *result) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    return conn_get_retry_max_delay_ms(tag->session ? &tag->session->connection_retry_max_delay_ms : NULL, RETRY_WAIT_MAX_MS,
                                       result);
}


int32_t cip_tag_set_connection_retry_max_delay_ms(plc_tag_p raw_tag, int32_t value) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    return conn_set_retry_max_delay_ms(tag->session ? &tag->session->connection_retry_max_delay_ms : NULL, value, tag->tag_id,
                                       DEBUG_MODULE_CIP);
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


int cip_tag_read_start(plc_tag_p tag_arg) {
    cip_tag_p tag = (cip_tag_p)tag_arg;
    int rc = PLCTAG_STATUS_OK;

    if(tag->create_read_status < PLCTAG_STATUS_OK) { return tag->create_read_status; }

    rc = cip_tag_send(tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(tag->debug_module, DEBUG_WARN, tag->tag_id, "Unable to queue the read, %s!", plc_tag_decode_error(rc));
        tag->offset = 0;
        return rc;
    }

    return PLCTAG_STATUS_PENDING;
}


int cip_tag_write_start(plc_tag_p tag_arg) {
    cip_tag_p tag = (cip_tag_p)tag_arg;
    int rc = PLCTAG_STATUS_OK;

    if(tag->create_read_status < PLCTAG_STATUS_OK) { return tag->create_read_status; }

    /* the create is not done: the type and size, or for @raw the connection, are not known yet. */
    if(tag->create_read_status == PLCTAG_STATUS_PENDING) {
        pdebug(tag->debug_module, DEBUG_WARN, tag->tag_id, "The create is still in flight!");
        return PLCTAG_ERR_BUSY;
    }

    rc = cip_tag_send(tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(tag->debug_module, DEBUG_WARN, tag->tag_id, "Unable to queue the write, %s!", plc_tag_decode_error(rc));
        tag->offset = 0;
        return rc;
    }

    return PLCTAG_STATUS_PENDING;
}


cip_tag_p cip_tag_alloc(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                        void *userdata, tag_protocol_t protocol_type, tag_vtable_p default_vtable,
                        debug_module_t debug_module) {
    cip_tag_p tag = NULL;
    int rc = PLCTAG_STATUS_OK;

    tag = (cip_tag_p)rc_alloc(sizeof(struct cip_tag_t), (rc_cleanup_func)cip_tag_destroy);
    if(!tag) {
        pdebug(debug_module, DEBUG_ERROR, 0, "Unable to allocate memory for CIP tag!");
        return NULL;
    }

    /*
     * The default vtable goes on before anything can fail, so that an abort between here
     * and the family's own setup has something safe to dispatch through.
     */
    tag->vtable = default_vtable;
    tag->protocol_type = protocol_type;

    rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(debug_module, DEBUG_WARN, 0, "Unable to initialize generic tag parts!");
        rc_dec(tag);
        return NULL;
    }

    pdebug(debug_module, DEBUG_DETAIL, tag->tag_id, "tag=%p", tag);

    return tag;
}


int cip_tag_resolve_elem_count(cip_tag_p tag, attr attribs, debug_module_t debug_module) {
    tag->elem_count = attr_get_int(attribs, "elem_count", 1);

    /*
     * An array can legitimately have up to INT32_MAX elements, but it cannot have zero or a
     * negative number of them.  Several places divide the tag size by this to recover the
     * element size, so a zero here is a division by zero later.
     */
    if(tag->elem_count < 1) {
        pdebug(debug_module, DEBUG_WARN, tag->tag_id, "Element count must be at least one, was %d!", tag->elem_count);
        return PLCTAG_ERR_BAD_PARAM;
    }

    return PLCTAG_STATUS_OK;
}


int cip_tag_size_from_plc(cip_tag_p tag, debug_module_t debug_module) {
    /*
     * The CIP read and write services carry the element count in a two-byte field, and every
     * place that builds one casts to uint16_t explicitly, so nothing downstream objects to a
     * larger value -- the count simply wraps and the PLC is asked for a different number of
     * elements than the caller asked for, with no error raised anywhere.
     *
     * This lives here rather than in a caller because a tag that learns its size from the PLC
     * has no other size check at all: the element-size path below has one, and this path had
     * none on the Rockwell side.
     */
    if(tag->elem_count > UINT16_MAX) {
        pdebug(debug_module, DEBUG_WARN, tag->tag_id, "Element count must be no more than %d, was %d!", UINT16_MAX,
               tag->elem_count);
        return PLCTAG_ERR_TOO_LARGE;
    }

    tag->size = 0;
    tag->data = NULL;

    return PLCTAG_STATUS_OK;
}


int cip_tag_size_from_attributes(cip_tag_p tag, attr attribs, debug_module_t debug_module) {
    /* get the element size if it is not already set. */
    if(!tag->elem_size) { tag->elem_size = attr_get_int(attribs, "elem_size", 0); }

    if(tag->elem_size < 1) {
        pdebug(debug_module, DEBUG_WARN, tag->tag_id, "Element size must be at least one byte, was %d!", tag->elem_size);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * Both of these come from the attribute string, so their product can overflow.  Signed
     * overflow is undefined, so test by division before multiplying rather than checking the
     * result afterwards -- by then the damage is already done and the wrapped value looks
     * like a perfectly reasonable size.
     */
    if(tag->elem_count > CIP_MAX_TAG_DATA_SIZE / tag->elem_size) {
        pdebug(debug_module, DEBUG_WARN, tag->tag_id,
               "Tag of %d elements of %d bytes each is larger than the maximum tag size of %d bytes!", tag->elem_count,
               tag->elem_size, CIP_MAX_TAG_DATA_SIZE);
        return PLCTAG_ERR_TOO_LARGE;
    }

    tag->size = (tag->elem_count) * (tag->elem_size);

    /* this may be changed in the future if this is a tag list request. */
    tag->data = (uint8_t *)mem_alloc(tag->size);
    if(tag->data == NULL) {
        pdebug(debug_module, DEBUG_WARN, tag->tag_id, "Unable to allocate tag data!");
        return PLCTAG_ERR_NO_MEM;
    }

    return PLCTAG_STATUS_OK;
}


int cip_tag_parse_elem_type(cip_tag_p tag, attr attribs, debug_module_t debug_module) {
    /*
     * A CIP STRING and a CIP SHORT_STRING are both a count followed by exactly that many
     * characters, so neither has an element size.  One byte, as the tag listing and @udt tags
     * use for the same reason.
     */
    static const struct {
        const char *name;
        int size;
        cip_elem_type_t type;
    } elem_types[] = {
        {"lint", 8, CIP_TYPE_INT64},     {"ulint", 8, CIP_TYPE_INT64},
        {"dint", 4, CIP_TYPE_INT32},     {"udint", 4, CIP_TYPE_INT32},
        {"int", 2, CIP_TYPE_INT16},      {"uint", 2, CIP_TYPE_INT16},
        {"sint", 1, CIP_TYPE_INT8},      {"usint", 1, CIP_TYPE_INT8},
        {"bool", 1, CIP_TYPE_BOOL},      {"bool array", 4, CIP_TYPE_BOOL_ARRAY},
        {"real", 4, CIP_TYPE_FLOAT32},   {"lreal", 8, CIP_TYPE_FLOAT64},
        {"string", 1, CIP_TYPE_STRING},  {"short string", 1, CIP_TYPE_SHORT_STRING},
    };
    const char *elem_type = attr_get_str(attribs, "elem_type", NULL);

    if(!elem_type) {
        tag->elem_size = attr_get_int(attribs, "elem_size", 0);
        return PLCTAG_STATUS_OK;
    }

    for(size_t i = 0; i < (sizeof(elem_types) / sizeof(elem_types[0])); i++) {
        if(str_cmp_i(elem_type, elem_types[i].name) == 0) {
            pdebug(debug_module, DEBUG_DETAIL, tag->tag_id, "Found tag element type %s.", elem_type);
            tag->elem_size = elem_types[i].size;
            tag->elem_type = elem_types[i].type;
            return PLCTAG_STATUS_OK;
        }
    }

    pdebug(debug_module, DEBUG_WARN, tag->tag_id, "Unknown tag element type %s!", elem_type);
    return PLCTAG_ERR_UNSUPPORTED;
}


/* An operation is over: end it, which queues its event, and finish the create if it was the create read. */
void cip_tag_op_done(cip_tag_p tag, int rc) {
    bool is_read = tag_op_is_read(tag);

    pdebug(tag->debug_module, DEBUG_DETAIL, tag->tag_id, "%s done with status %s.", (is_read ? "Read" : "Write"),
           plc_tag_decode_error(rc));

    /* the op ends first, so that the create read ending queues CREATED rather than READ_COMPLETED. */
    tag_op_end((plc_tag_p)tag, rc);

    /* the first read completing is what completes the create. */
    if(is_read) { cip_tag_finish_create(tag, rc); }
}
