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
#include <libplctag/protocols/ab/ab_common.h>
#include <libplctag/protocols/ab/cip.h>
#include <libplctag/protocols/ab/defs.h>
#include <libplctag/protocols/ab/eip_cip.h> /* for the Logix decode types. */
#include <libplctag/protocols/ab/eip_cip_special.h>
#include <libplctag/modules/cip/error_codes.h>
#include <libplctag/protocols/ab/session.h>
#include <libplctag/protocols/ab/tag.h>
#include <platform.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/vector.h>

START_PACK typedef struct {
    uint32_le instance_id;    /* monotonically increasing but not contiguous */
    uint16_le symbol_type;    /* type of the symbol. */
    uint16_le element_length; /* length of one array element in bytes. */
    uint32_le array_dims[3];  /* array dimensions. */
    uint16_le string_len;     /* string length count. */
                              // uint8_t string_name[82]; /* MAGIC string name bytes (string_len of them, zero padded) */
} END_PACK tag_list_entry;

/* listing tag functions. */
static int listing_tag_read_start(ab_tag_p tag);
static int listing_tag_tickler(ab_tag_p tag);
// static int listing_tag_write_start(ab_tag_p tag);
static int listing_tag_check_read_status_connected(ab_tag_p tag);
static int listing_tag_build_read_request_connected(ab_tag_p tag);

/* define the vtable for listing tag type. */
static struct tag_vtable_t listing_tag_vtable = {
    .abort = (tag_vtable_func)ab_tag_abort_request,
    .read = (tag_vtable_func)listing_tag_read_start,
    .status = (tag_vtable_func)ab_tag_status,
    .tickler = (tag_vtable_func)listing_tag_tickler,
    .write = NULL,
    .wake_plc = NULL,
    .tag_data_written = NULL,

    /* attribute accessors */
    .attribs = ab_attribs,
};

static tag_byte_order_t listing_tag_logix_byte_order = {.is_allocated = 0,

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
                                                        .str_count_word_bytes = 2,
                                                        .str_max_capacity = 0,
                                                        .str_total_length = 0,
                                                        .str_pad_bytes = 0};

/******************************************************************
 ******************* tag listing functions ************************
 ******************************************************************/


/*
 * Handle tag listing tag set up.
 *
 * There are two main cases here: 1) a bare tag listing, 2) a program tag listing.
 * We know that we got here because the string "@tags" was in the name.
 */

int setup_tag_listing_tag(ab_tag_p tag, const char *name) {
    int rc = PLCTAG_STATUS_OK;
    char **tag_parts = NULL;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Starting.");

    do {
        /* is it a bare tag listing? */
        if(str_cmp_i(name, "@tags") == 0) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Tag is a bare tag listing tag.");
            break;
        }

        /* is it a program tag listing request? */
        if(str_length(name) >= str_length("PROGRAM:x.@tags")) {
            tag_parts = str_split(name, ".");

            /* check to make sure that we have at least one part. */
            if(!tag_parts) {
                pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Tag %s is not a tag listing request.", name);
                rc = PLCTAG_ERR_BAD_PARAM;
                break;
            }

            /* check that we have exactly two parts. */
            if(tag_parts[0] != NULL && tag_parts[1] != NULL && tag_parts[2] == NULL) {
                /* we have exactly two parts. Make sure the last part is "@tags" */
                if(str_cmp_i(tag_parts[1], "@tags") != 0) {
                    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Tag %s is not a tag listing request.",
                           name);
                    rc = PLCTAG_ERR_BAD_PARAM;
                    break;
                }

                if(str_length(tag_parts[0]) <= str_length("PROGRAM:x")) {
                    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Tag %s is not a tag listing request.",
                           name);
                    rc = PLCTAG_ERR_BAD_PARAM;
                    break;
                }

                /* make sure the first part is "PROGRAM:" */
                if(str_cmp_i_n(tag_parts[0], "PROGRAM:", str_length("PROGRAM:"))) {
                    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Tag %s is not a tag listing request.",
                           name);
                    rc = PLCTAG_ERR_NOT_FOUND;
                    break;
                }

                /* we have a program tag request! */
                if(cip_encode_tag_name(tag, tag_parts[0]) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
                           "Tag %s program listing is not able to be encoded!", name);
                    rc = PLCTAG_ERR_BAD_PARAM;
                    break;
                }
            } else {
                pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Tag %s is not a tag listing request.", name);
                rc = PLCTAG_ERR_NOT_FOUND;
                break;
            }
        } else {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Program tag %s listing tag string malformed.",
                   name);
            rc = PLCTAG_ERR_BAD_PARAM;
            break;
        }
    } while(0);

    /* clean up */
    if(tag_parts) { mem_free(tag_parts); }

    /* did we find a listing tag? */
    if(rc == PLCTAG_STATUS_OK) {
        /* yes we did */
        tag->special_tag = 1;
        tag->elem_type = CIP_TYPE_TAG_ENTRY;
        tag->elem_count = 1;
        tag->elem_size = 1;

        tag->byte_order = &listing_tag_logix_byte_order;

        tag->vtable = &listing_tag_vtable;

        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done. Found tag listing tag name %s.", name);
    } else {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
               "Done. Tag %s is not a well-formed tag listing name, error %s.", name, plc_tag_decode_error(rc));
    }

    return rc;
}


/*
 * listing_tag_read_start
 *
 * This function must be called only from within one thread, or while
 * the tag's mutex is locked.
 *
 * The function starts the process of getting tag data from the PLC.
 */

int listing_tag_read_start(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Starting");

    if(tag->write_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "A write is in progress on a listing tag!");
        return PLCTAG_ERR_BAD_STATUS;
    }

    if(tag->read_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    /* mark the tag read in progress */
    tag->read_in_progress = 1;

    /* build the new request */
    rc = listing_tag_build_read_request_connected(tag);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unable to build read request!");

        tag->read_in_progress = 0;

        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_PENDING;
}


int listing_tag_tickler(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Starting.");

    rc = check_request_status(tag);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(tag->write_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
               "Something started a write on a listing tag.   This is not supported!");

        ab_tag_abort_request(tag);

        /* fire the event anyway. */
        tag->write_complete = 1;

        return PLCTAG_ERR_UNSUPPORTED;
    }

    if(tag->read_in_progress) {

        rc = listing_tag_check_read_status_connected(tag);

        tag->status = (int8_t)rc;

        /* if the operation completed, make a note so that the callback will be called. */
        if(!tag->read_in_progress) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Read complete.");
            tag->read_complete = 1;
        } else {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Done. Read in progress.");
        }

        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Done.  No operation in progress.");

    return tag->status;
}


/*
 * listing_tag_check_read_status_connected
 *
 * This routine checks for any outstanding tag list requests.  It will
 * terminate when there is no data in the response and the error is not "more data".
 *
 * This is not thread-safe!  It should be called with the tag mutex
 * locked!
 */

int listing_tag_check_read_status_connected(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_resp *cip_resp;
    uint8_t *data;
    uint8_t *data_end;
    int partial_data = 0;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Starting.");

    /* if we got here then we have a request and it was processed correctly. */

    /* point to the data */
    cip_resp = (eip_cip_co_resp *)(tag->req->data);

    /* point to the start of the data */
    data = (tag->req->data) + sizeof(eip_cip_co_resp);

    /* point the end of the data */
    data_end = tag->req->data + tag->req->request_size;

    /* check the status */
    do {
        ptrdiff_t payload_size = (data_end - data);

        if(cip_resp->reply_service != (AB_EIP_CMD_CIP_LIST_TAGS | AB_EIP_CMD_CIP_OK)) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "CIP response reply service unexpected: %d",
                   cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(cip_resp->status != AB_CIP_STATUS_OK && cip_resp->status != AB_CIP_STATUS_FRAG) {
            size_t status_size = cip_error_data_size((uint8_t *)&cip_resp->status, data_end);

            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "CIP read failed with status: 0x%x %s",
                   cip_resp->status, decode_cip_error_short((uint8_t *)&cip_resp->status, status_size));
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id,
                   decode_cip_error_long((uint8_t *)&cip_resp->status, status_size));
            rc = decode_cip_error_code((uint8_t *)&cip_resp->status, status_size);
            break;
        }

        /* check to see if this is a partial response. */
        partial_data = (cip_resp->status == AB_CIP_STATUS_FRAG);

        /*
         * check to see if there is any data to process.  If this is a packed
         * response, there might not be.
         */
        if(payload_size > 0) {
            /* we got data, so the transfer is moving again. */
            tag->fragment_retry_count = 0;

            uint8_t *current_entry_data = data;
            int new_size = (int)payload_size + tag->offset;

            /* a PLC can keep returning fragments forever.  Do not grow without bound. */
            if((payload_size + tag->offset) > (ptrdiff_t)AB_MAX_TAG_DATA_SIZE) {
                pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
                       "Tag list data size of %d bytes is larger than the maximum of %d bytes!",
                       (int)(payload_size + tag->offset), AB_MAX_TAG_DATA_SIZE);
                rc = PLCTAG_ERR_TOO_LARGE;
                break;
            }

            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Received %d bytes of tag list data.  Partial: %s",
                   (int)payload_size, partial_data ? "yes" : "no");
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "new size: %d", new_size);
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "current tag size: %d", tag->size);
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "current offset: %d", tag->offset);

            /* copy the data into the tag and realloc if we need more space. */

            if(new_size > tag->size) {
                uint8_t *new_buffer = NULL;

                /* tag->elem_count = */ tag->size = new_size;

                pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Increasing tag buffer size to %d bytes.",
                       new_size);

                new_buffer = (uint8_t *)mem_realloc(tag->data, new_size);
                if(!new_buffer) {
                    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unable to reallocate tag data memory!");
                    rc = PLCTAG_ERR_NO_MEM;
                    break;
                }

                tag->data = new_buffer;
                /* tag->elem_count = */ tag->size = new_size;
            }

            /* copy the data into the tag's data buffer. */
            mem_copy(tag->data + tag->offset, data, (int)payload_size);

            tag->offset += (int)payload_size;

            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "current offset %d", tag->offset);

            /* scan through the data to get the next ID to use. */
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Scanning through data for next ID.");
            while((data_end - current_entry_data) >= (ptrdiff_t)sizeof(tag_list_entry)) {
                tag_list_entry *current_entry = (tag_list_entry *)current_entry_data;
                ptrdiff_t entry_size = (ptrdiff_t)sizeof(*current_entry) + (ptrdiff_t)le2h16(current_entry->string_len);
                uint32_t next_instance_id = 0;

                if(entry_size > (data_end - current_entry_data)) {
                    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
                           "Tag list entry name length runs past the end of the response!");
                    rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                    break;
                }

                /*
                 * First element is the symbol instance ID.
                 *
                 * The listing protocol reports a 32-bit instance ID but the request that asks
                 * for the next chunk can only carry 16 bits of it, so increment in 32 bits and
                 * check before narrowing.  Truncating instead would wrap the ID back to a low
                 * value and restart the listing from the beginning, forever -- and a controller
                 * with enough tags to reach that point is doing nothing wrong.
                 */
                next_instance_id = le2h32(current_entry->instance_id) + 1;

                if(next_instance_id > UINT16_MAX) {
                    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
                           "Symbol instance ID %" PRIu32 " is too large to request the next chunk of the tag listing!",
                           next_instance_id);
                    rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                    break;
                }

                tag->next_id = next_instance_id;

                pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Next ID: %d", tag->next_id);

                /* skip past to the next instance. */
                current_entry_data += entry_size;

                tag->elem_count++;
            }
        } else {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Response returned no data and no error.");

            /* no payload means no forward progress on a fragmented transfer. */
            tag->fragment_retry_count++;
        }
    } while(0);

    /* clean up the request as we are done with it. */
    ab_tag_abort_request_only(tag);

    /* are we actually done? */
    if(rc == PLCTAG_STATUS_OK) {
        /* keep going if we are not done yet, unless we are getting nowhere. */
        if(partial_data && tag->fragment_retry_count > MAX_FRAGMENT_RETRIES) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
                   "Got %d partial responses in a row with no data.  The transfer is not making progress, giving up.",
                   tag->fragment_retry_count);
            rc = PLCTAG_ERR_PARTIAL;
        } else if(partial_data) {
            /* call read start again to get the next piece */
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id,
                   "calling listing_tag_build_read_request_connected() to get the next chunk.");
            rc = listing_tag_build_read_request_connected(tag);
        } else {
            /* done!  Start the next transfer with a clean progress counter. */
            tag->fragment_retry_count = 0;

            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Done reading tag list data!");

            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "total symbols: %d", tag->elem_count);

            // tag->elem_count = tag->offset;

            tag->first_read = 0;
            tag->offset = 0;
            tag->next_id = 0;

            /* this read is done. */
            tag->read_in_progress = 0;
        }
    }

    /* this is not an else clause because the above if could result in bad rc. */
    if(rc_is_error(rc)) {
        /* error ! */
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Error received: %s!", plc_tag_decode_error(rc));

        tag->offset = 0;
        tag->next_id = 0;

        /* clean up everything in case we left something dangling. */
        ab_tag_abort_request(tag);
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}


int listing_tag_build_read_request_connected(ab_tag_p tag) {
    eip_cip_co_req *cip = NULL;
    // tag_list_req *list_req = NULL;
    ab_request_p req = NULL;
    int rc = PLCTAG_STATUS_OK;
    uint8_t *data_start = NULL;
    uint8_t *data = NULL;
    uint16_le tmp_u16 = UINT16_LE_INIT(0);

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Starting.");

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    /* point the request struct at the buffer */
    cip = (eip_cip_co_req *)(req->data);

    /* point to the end of the struct */
    data_start = data = (uint8_t *)(cip + 1);

    /*
     * set up the embedded CIP tag list request packet
        uint8_t request_service;    AB_EIP_CMD_CIP_LIST_TAGS=0x55
        uint8_t request_path_size;  3 word = 6 bytes
        uint8_t request_path[6];        0x20    get class
                                        0x6B    tag info/symbol class
                                        0x25    get instance (16-bit)
                                        0x00    padding
                                        0x00    instance byte 0
                                        0x00    instance byte 1
        uint16_le instance_id;      NOTE! this is the last two bytes above for convenience!
        uint16_le num_attributes;   0x04    number of attributes to get
        uint16_le requested_attributes[4];      0x02 attribute #2 - symbol type
                                                0x07 attribute #7 - base type size (array element) in bytes
                                                0x08    attribute #8 - array dimensions (3xu32)
                                                0x01    attribute #1 - symbol name
    */

    *data = AB_EIP_CMD_CIP_LIST_TAGS;
    data++;

    /* request path size, in 16-bit words */
    *data = (uint8_t)(3 + ((tag->encoded_name_size - 1) / 2)); /* size in words of routing header + routing and instance ID. */
    data++;

    /* add in the encoded name, but without the leading word count byte! */
    if(tag->encoded_name_size > 1) {
        mem_copy(data, &tag->encoded_name[1], (tag->encoded_name_size - 1));
        data += (tag->encoded_name_size - 1);
    }

    /* add in the routing header . */

    /* first the fixed part. */
    data[0] = 0x20; /* class type */
    data[1] = 0x6B; /* tag info/symbol class */
    data[2] = 0x25; /* 16-bit instance ID type */
    data[3] = 0x00; /* padding */
    data += 4;

    /* now the instance ID */
    tmp_u16 = h2le16((uint16_t)tag->next_id);
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* set up the request itself.  We are asking for a number of attributes. */

    /* set up the request attributes, first the number of attributes. */
    tmp_u16 = h2le16((uint16_t)4); /* MAGIC, we have four attributes we want. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* first attribute: symbol type */
    tmp_u16 = h2le16((uint16_t)0x02); /* MAGIC, symbol type. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* second attribute: base type size in bytes */
    tmp_u16 = h2le16((uint16_t)0x07); /* MAGIC, element size in bytes. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* third attribute: tag array dimensions */
    tmp_u16 = h2le16((uint16_t)0x08); /* MAGIC, array dimensions. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* fourth attribute: symbol/tag name */
    tmp_u16 = h2le16((uint16_t)0x01); /* MAGIC, symbol name. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* now we go back and fill in the fields of the static part */

    /* encap fields */
    cip->encap_command = h2le16(AB_EIP_CONNECTED_SEND); /* ALWAYS 0x0070 Connected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    cip->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI); /* ALWAYS 0x00A1 connected address item */
    cip->cpf_cai_item_length = h2le16(4);             /* ALWAYS 4, size of connection ID*/
    cip->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI); /* ALWAYS 0x00B1 - connected Data Item */
    cip->cpf_cdi_item_length = h2le16((uint16_t)((int)(data - data_start) + (int)sizeof(cip->cpf_conn_seq_num)));

    /* Check if the payload size exceeds available space before setting request_size */
    int packet_payload_size = (int)(data - data_start) + (int)sizeof(cip->cpf_conn_seq_num);
    int available_payload = session_get_available_cip_payload_space(tag->session);

    if(packet_payload_size > available_payload) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
               "Request payload (%d bytes) exceeds available space (%d bytes)!", packet_payload_size, available_payload);
        ab_tag_abort_request(tag);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* set the size of the request */
    req->request_size = (int)((int)sizeof(*cip) + (int)(data - data_start));

    req->allow_packing = tag->allow_packing;

    /* add the request to the session's list. */
    tag->read_in_progress = 1;
    rc = session_add_request(tag->session, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to add request to session! rc=%d", rc);
        ab_tag_abort_request(tag);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done");

    return PLCTAG_STATUS_OK;
}
