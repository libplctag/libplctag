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

#include <ctype.h>
#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/protocols/cip/cip.h>
#include <libplctag/protocols/cip/conn.h>
#include <libplctag/protocols/cip/error_codes.h>
#include <libplctag/protocols/omron/cip.h>
#include <libplctag/protocols/omron/conn.h>
#include <libplctag/protocols/omron/defs.h>
#include <libplctag/protocols/omron/omron_common.h>
#include <libplctag/protocols/omron/omron_standard_tag.h>
#include <libplctag/protocols/omron/tag.h>
#include <utils/mem.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/rc.h>
#include <utils/vector.h>


static int build_read_request_connected(omron_tag_p tag, int byte_offset);
// static int build_tag_list_request_connected(omron_tag_p tag);
static int build_read_request_unconnected(omron_tag_p tag, int byte_offset);
static int build_write_request_connected(omron_tag_p tag, int byte_offset);
static int build_write_request_unconnected(omron_tag_p tag, int byte_offset);
static int check_read_status_connected(omron_tag_p tag);
static int check_read_status_unconnected(omron_tag_p tag);
static int check_write_status_connected(omron_tag_p tag);
static int check_write_status_unconnected(omron_tag_p tag);

static int tag_read_start(omron_tag_p tag);
static int tag_tickler(omron_tag_p tag);
static int tag_write_start(omron_tag_p tag);

/* define the exported vtable for this tag type. */
struct tag_vtable_t omron_standard_tag_vtable = {
    .abort = (tag_vtable_func)omron_tag_abort,
    .read = (tag_vtable_func)tag_read_start,
    .status = (tag_vtable_func)omron_tag_status,
    .tickler = (tag_vtable_func)tag_tickler,
    .write = (tag_vtable_func)tag_write_start,
    .wake_plc = NULL,
    .tag_data_written = NULL,

    /* attribute accessors */
    .get_int_attrib = omron_get_int_attrib,
    .set_int_attrib = omron_set_int_attrib,
    .get_byte_array_attrib = omron_get_byte_array_attrib,
};

// /* default string types used for ControlLogix-class PLCs. */
// tag_byte_order_t omron_udt_tag_byte_order = {
//     .is_allocated = 0,

//     .int16_order = {0,1},
//     .int32_order = {0,1,2,3},
//     .int64_order = {0,1,2,3,4,5,6,7},
//     .float32_order = {0,1,2,3},
//     .float64_order = {0,1,2,3,4,5,6,7},

//     .str_is_defined = 1,
//     .str_is_counted = 1,
//     .str_is_fixed_length = 1,
//     .str_is_zero_terminated = 0,
//     .str_is_byte_swapped = 0,

//     .str_pad_to_multiple_bytes = 1,
//     .str_count_word_bytes = 4,
//     .str_max_capacity = 82,
//     .str_total_length = 88,
//     .str_pad_bytes = 2
// };


/* default string types used for Omron-NJ/NX PLCs. */
tag_byte_order_t omron_njnx_tag_byte_order = {.is_allocated = 0,

                                              .int16_order = {0, 1},
                                              .int32_order = {0, 1, 2, 3},
                                              .int64_order = {0, 1, 2, 3, 4, 5, 6, 7},
                                              .float32_order = {0, 1, 2, 3},
                                              .float64_order = {0, 1, 2, 3, 4, 5, 6, 7},

                                              .str_is_defined = 1,
                                              .str_is_counted = 1,
                                              .str_is_fixed_length = 0,
                                              .str_is_zero_terminated = 1,
                                              .str_is_byte_swapped = 0,

                                              .str_pad_to_multiple_bytes = 1,
                                              .str_count_word_bytes = 2,
                                              .str_max_capacity = 0,
                                              .str_total_length = 0,
                                              .str_pad_bytes = 0};

// tag_byte_order_t omron_tag_listing_byte_order = {
//     .is_allocated = 0,

//     .int16_order = {0,1},
//     .int32_order = {0,1,2,3},
//     .int64_order = {0,1,2,3,4,5,6,7},
//     .float32_order = {0,1,2,3},
//     .float64_order = {0,1,2,3,4,5,6,7},

//     .str_is_defined = 1,
//     .str_is_counted = 1,
//     .str_is_fixed_length = 0,
//     .str_is_zero_terminated = 0,
//     .str_is_byte_swapped = 0,

//     .str_pad_to_multiple_bytes = 2,
//     .str_count_word_bytes = 2,
//     .str_max_capacity = 0,
//     .str_total_length = 0,
//     .str_pad_bytes = 0
// };


/*************************************************************************
 **************************** API Functions ******************************
 ************************************************************************/


/* the builders are shared; see protocols/cip/tag.h. */
static const cip_build_io_t build_io = {
    .add_request = (int (*)(void *, cip_request_p))conn_add_request,
    .abort_request = (int (*)(cip_tag_p))omron_tag_abort_request,
};


static int build_read_request_connected(omron_tag_p tag, int byte_offset) {
    return cip_build_read_request_connected((cip_tag_p)tag, (cip_conn_p)tag->conn, &build_io, byte_offset);
}


static int build_read_request_unconnected(omron_tag_p tag, int byte_offset) {
    return cip_build_read_request_unconnected((cip_tag_p)tag, (cip_conn_p)tag->conn, &build_io, byte_offset);
}


static int build_write_request_connected(omron_tag_p tag, int byte_offset) {
    return cip_build_write_request_connected((cip_tag_p)tag, (cip_conn_p)tag->conn, &build_io, byte_offset);
}


static int build_write_request_unconnected(omron_tag_p tag, int byte_offset) {
    return cip_build_write_request_unconnected((cip_tag_p)tag, (cip_conn_p)tag->conn, &build_io, byte_offset);
}


int tag_tickler(omron_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_SPEW, tag->tag_id, "Starting.");

    rc = omron_check_request_status(tag);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(tag->read_in_progress) {
        if(tag->use_connected_msg) {
            rc = check_read_status_connected(tag);
        } else {
            rc = check_read_status_unconnected(tag);
        }

        tag->status = (int8_t)rc;

        /* if the operation completed, make a note so that the callback will be called. */
        if(!tag->read_in_progress) {
            /* done! */
            if(tag->first_read) {
                tag->first_read = 0;
                tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, (int8_t)rc);
            }

            tag->read_complete = 1;
        }

        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_SPEW, tag->tag_id, "Done.  Read in progress.");

        return rc;
    }

    if(tag->write_in_progress) {
        if(tag->use_connected_msg) {
            rc = check_write_status_connected(tag);
        } else {
            rc = check_write_status_unconnected(tag);
        }

        tag->status = (int8_t)rc;

        /* if the operation completed, make a note so that the callback will be called. */
        if(!tag->write_in_progress) { tag->write_complete = 1; }

        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_SPEW, tag->tag_id, "Done. Write in progress.");

        return rc;
    }

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_SPEW, tag->tag_id, "Done.  No operation in progress.");

    return tag->status;
}


/*
 * tag_read_common_start
 *
 * This function must be called only from within one thread, or while
 * the tag's mutex is locked.
 *
 * The function starts the process of getting tag data from the PLC.
 */

int tag_read_start(omron_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_INFO, tag->tag_id, "Starting");

    if(tag->read_in_progress || tag->write_in_progress) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    /* mark the tag read in progress */
    tag->read_in_progress = 1;

    /* i is the index of the first new request */
    if(tag->use_connected_msg) {
        // if(tag->tag_list) {
        //     rc = build_tag_list_request_connected(tag);
        // } else {
        rc = build_read_request_connected(tag, tag->offset);
        // }
    } else {
        rc = build_read_request_unconnected(tag, tag->offset);
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Unable to build read request!");

        tag->read_in_progress = 0;

        return rc;
    }

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_PENDING;
}


/*
 * tag_write_common_start
 *
 * This must be called from one thread alone, or while the tag mutex is
 * locked.
 *
 * The routine starts the process of writing to a tag.
 */

int tag_write_start(omron_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_INFO, tag->tag_id, "Starting");

    if(tag->read_in_progress || tag->write_in_progress) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    /* the write is now in flight */
    tag->write_in_progress = 1;

    /*
     * if the tag has not been read yet, read it.
     *
     * This gets the type data and sets up the request
     * buffers.
     */

    if(tag->first_read) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id,
               "No read has completed yet, doing pre-read to get type information.");

        tag->pre_write_read = 1;
        tag->write_in_progress = 0; /* temporarily mask this off */

        return tag_read_start(tag);
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Unable to calculate write sizes!");
        tag->write_in_progress = 0;

        return rc;
    }

    if(tag->use_connected_msg) {
        rc = build_write_request_connected(tag, tag->offset);
    } else {
        rc = build_write_request_unconnected(tag, tag->offset);
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Unable to build write request!");
        tag->write_in_progress = 0;

        return rc;
    }

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_PENDING;
}


/*
 * check_read_status_connected
 *
 * This routine checks for any outstanding requests and copies in data
 * that has arrived.  At the end of the request, it will clean up the request
 * buffers.  This is not thread-safe!  It should be called with the tag mutex
 * locked!
 */

/*
 * Connected and unconnected differ in the EIP and CPF headers ahead of the CIP
 * reply, so where that reply sits and where its payload starts are passed in.
 */
static int check_read_status(omron_tag_p tag, cip_header *cip_resp, uint8_t *data) {
    int rc = PLCTAG_STATUS_OK;
    uint8_t *data_end;
    int partial_data = 0;

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_SPEW, tag->tag_id, "Starting.");

    /* the request reference is valid. */

    /* point the end of the data */
    data_end = tag->req->data + tag->req->request_size;

    /* check the status */
    do {
        ptrdiff_t payload_size = 0;

        /*
         * A non-zero encapsulation status means the request failed before CIP ever
         * saw it.  Only the unconnected form used to check this; nothing else in
         * either module checks it for an ordinary response, so it now covers both
         * frames.  The command check that sat beside it is gone: conn.c already
         * rejects a response whose EIP command does not answer the request that was
         * sent (see the req_encap_command comparison in recv_eip_response).
         */
        if(le2h32(((eip_encap *)(tag->req->data))->encap_status) != EIP_OK) {
            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "EIP command failed, response code: %d",
                   le2h32(((eip_encap *)(tag->req->data))->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(cip_resp->reply_service != (CIP_CMD_READ | CIP_CMD_OK)) {
            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "CIP response reply service unexpected: %d",
                   cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(cip_resp->status != CIP_STATUS_OK && cip_resp->status != CIP_STATUS_FRAG) {
            size_t status_size = cip_error_data_size((uint8_t *)&cip_resp->status, data_end);

            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "CIP read failed with status: 0x%x %s", cip_resp->status,
                   decode_cip_error_short((uint8_t *)&cip_resp->status, status_size));
            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_INFO, tag->tag_id,
                   "%s", decode_cip_error_long((uint8_t *)&cip_resp->status, status_size));

            rc = decode_cip_error_code((uint8_t *)&cip_resp->status, status_size);

            break;
        }

        /* check to see if this is a partial response. */
        partial_data = (cip_resp->status == CIP_STATUS_FRAG);

        /*
         * check to see if there is any data to process.  If this is a packed
         * response, there might not be.
         */
        payload_size = (data_end - data);
        if(payload_size > 0) {
            /* we got data, so the transfer is moving again. */
            tag->fragment_retry_count = 0;

            /* skip the copy if we already have type data */
            if(tag->encoded_type_info_size == 0) {
                int type_length = 0;

                /* the first byte of the response is a type byte. */
                pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "type byte = %d (0x%02x)", (int)*data, (int)*data);

                if(cip_lookup_encoded_type_size(*data, &type_length) == PLCTAG_STATUS_OK) {
                    /* found it and we got the type data size */

                    /* some types use the second byte to indicate how many bytes more are used. */
                    if(type_length == 0) {
                        if(payload_size < 2) {
                            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id,
                                   "Response too short to hold extended type length byte!");
                            rc = PLCTAG_ERR_TOO_SMALL;
                            break;
                        }

                        type_length = *(data + 1) + 2;
                    }

                    if(type_length <= 0 || type_length > (int)sizeof(tag->encoded_type_info)
                       || type_length > (int)payload_size) {
                        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id,
                               "Type data length %d for type byte 0x%02x is out of range (max %d, available %d)!",
                               type_length, *data, (int)sizeof(tag->encoded_type_info), (int)payload_size);
                        rc = PLCTAG_ERR_TOO_LARGE;
                        break;
                    }

                    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "Type data is %d bytes long.", type_length);
                    pdebug_dump_bytes(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, data, type_length);

                    tag->encoded_type_info_size = type_length;
                    mem_copy(tag->encoded_type_info, data, tag->encoded_type_info_size);
                } else {
                    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Unsupported data type returned, type byte=0x%02x",
                           *data);
                    rc = PLCTAG_ERR_UNSUPPORTED;
                    break;
                }
            }

            /* skip past the type data */
            data += (tag->encoded_type_info_size);

            if((intptr_t)data > (intptr_t)data_end) {
                pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id,
                       "Response too short to hold remembered type info of %d bytes!", tag->encoded_type_info_size);
                rc = PLCTAG_ERR_TOO_SMALL;
                break;
            }

            /* check payload size now that we have bumped past the data type info. */
            payload_size = (data_end - data);

            /* copy the data into the tag and realloc if we need more space. */
            if(payload_size + tag->offset > tag->size) {
                tag->size = (int)payload_size + tag->offset;
                tag->elem_size = tag->size / tag->elem_count;

                pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "Increasing tag buffer size to %d bytes.", tag->size);

                tag->data = (uint8_t *)mem_realloc(tag->data, tag->size);
                if(!tag->data) {
                    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Unable to reallocate tag data memory!");
                    rc = PLCTAG_ERR_NO_MEM;
                    break;
                }
            }

            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_INFO, tag->tag_id, "Got %d bytes of data", (int)payload_size);

            /*
             * copy the data, but only if this is not
             * a pre-read for a subsequent write!  We do not
             * want to overwrite the data the upstream has
             * put into the tag's data buffer.
             */
            if(!tag->pre_write_read) { mem_copy(tag->data + tag->offset, data, (int)(payload_size)); }

            /* bump the byte offset */
            tag->offset += (int)(payload_size);
        } else {
            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "Response returned no data and no error.");

            /* no payload means no forward progress on a fragmented transfer. */
            tag->fragment_retry_count++;
        }

        /* set the return code */
        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* clean up the request */
    omron_tag_abort(tag);

    /* are we actually done? */
    if(rc == PLCTAG_STATUS_OK) {
        /* this particular read is done. */
        tag->read_in_progress = 0;

        /* skip if we are doing a pre-write read. */
        if(!tag->pre_write_read && partial_data && tag->fragment_retry_count > MAX_FRAGMENT_RETRIES) {
            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id,
                   "Got %d fragment responses in a row with no data.  The transfer is not making progress, giving up.",
                   tag->fragment_retry_count);
            rc = PLCTAG_ERR_PARTIAL;
        } else if(!tag->pre_write_read && partial_data) {
            /* call read start again to get the next piece */
            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "calling tag_read_start() to get the next chunk.");
            /* FIXNE - the abort function above resets the offset. */
            rc = tag_read_start(tag);
        } else {
            tag->offset = 0;

            /* the transfer is over one way or the other, so start the next one clean. */
            tag->fragment_retry_count = 0;

            /* if this is a pre-read for a write, then pass off to the write routine */
            if(tag->pre_write_read) {
                pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "Restarting write call now.");
                tag->pre_write_read = 0;
                rc = tag_write_start(tag);
            }
        }
    }

    /* this is not an else clause because the above if could result in bad rc. */
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        /* error ! */
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Error received!");

        /* clean up everything. */
        omron_tag_abort(tag);
    }

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}

static int check_read_status_connected(omron_tag_p tag) {
    eip_cip_co_resp *resp = (eip_cip_co_resp *)(tag->req->data);

    return check_read_status(tag, (cip_header *)&(resp->reply_service), (tag->req->data) + sizeof(eip_cip_co_resp));
}


static int check_read_status_unconnected(omron_tag_p tag) {
    eip_cip_uc_resp *resp = (eip_cip_uc_resp *)(tag->req->data);

    return check_read_status(tag, (cip_header *)&(resp->reply_service), (tag->req->data) + sizeof(eip_cip_uc_resp));
}


/*
 * check_write_status_connected
 *
 * This routine must be called with the tag mutex locked.  It checks the current
 * status of a write operation.  If the write is done, it triggers the clean up.
 */

/*
 * As on the AB side, the connected and unconnected forms differed only in which
 * response struct the buffer was cast to, and both read only the CIP reply
 * header -- the same four bytes in either frame.  The entry points below say
 * where that header sits.
 *
 * The connected form also carried a null check that dereferenced tag->tag_id
 * inside the if(!tag) branch, so it would have crashed before it could return
 * PLCTAG_ERR_NULL_PTR.  Neither the unconnected form nor AB has one; it is gone.
 */
static int check_write_status(omron_tag_p tag, cip_header *cip_resp) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_SPEW, tag->tag_id, "Starting.");

    do {
        if(cip_resp->reply_service != (CIP_CMD_WRITE | CIP_CMD_OK)
           && cip_resp->reply_service != (CIP_CMD_RMW | CIP_CMD_OK)) {
            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "CIP response reply service unexpected: %d",
                   cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(cip_resp->status != CIP_STATUS_OK && cip_resp->status != CIP_STATUS_FRAG) {
            size_t status_size = cip_error_data_size((uint8_t *)&cip_resp->status, tag->req->data + tag->req->request_size);

            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "CIP read failed with status: 0x%x %s", cip_resp->status,
                   decode_cip_error_short((uint8_t *)&cip_resp->status, status_size));
            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_INFO, tag->tag_id,
                   "%s", decode_cip_error_long((uint8_t *)&cip_resp->status, status_size));
            rc = decode_cip_error_code((uint8_t *)&cip_resp->status, status_size);
            break;
        }
    } while(0);

    /* clean up the request. */
    omron_tag_abort_request_only(tag);

    /* write is done in one way or another. */
    tag->write_in_progress = 0;

    if(rc == PLCTAG_STATUS_OK) {
        if(tag->offset < tag->size) {

            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "Write not complete, triggering next round.");
            /* FIXME - the abort function above resets the offset */
            rc = tag_write_start(tag);
        } else {
            /* only clear this if we are done. */
            tag->offset = 0;
        }
    } else {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Write failed!");

        tag->offset = 0;
    }

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}

static int check_write_status_connected(omron_tag_p tag) {
    eip_cip_co_resp *resp = (eip_cip_co_resp *)(tag->req->data);

    return check_write_status(tag, (cip_header *)&(resp->reply_service));
}


static int check_write_status_unconnected(omron_tag_p tag) {
    eip_cip_uc_resp *resp = (eip_cip_uc_resp *)(tag->req->data);

    return check_write_status(tag, (cip_header *)&(resp->reply_service));
}

