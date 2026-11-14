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
#include <stdbool.h>
#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/protocols/ab/ab_common.h>
#include <libplctag/protocols/ab/cip.h>
#include <libplctag/protocols/ab/defs.h>
#include <libplctag/protocols/ab/eip_cip.h>
#include <libplctag/protocols/ab/error_codes.h>
#include <libplctag/protocols/ab/session.h>
#include <libplctag/protocols/ab/tag.h>
#include <platform.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/vector.h>


/* tag listing packet format is as follows for controller tags:

CIP Tag Info command
    uint8_t request_service    0x55
    uint8_t request_path_size  3 - 6 bytes
    uint8_t   0x20    get class
    uint8_t   0x6B    tag info/symbol class
    uint8_t   0x25    get instance (16-bit)
    uint8_t   0x00    padding
    uint8_t   0x00    instance byte 0
    uint8_t   0x00    instance byte 1
    uint16_t  0x04    number of attributes to get
    uint16_t  0x02    attribute #2 - symbol type
    uint16_t  0x07    attribute #7 - base type size (array element) in bytes
    uint16_t  0x08    attribute #8 - array dimensions (3xu32)
    uint16_t  0x01    attribute #1 - symbol name

*/

/* tag listing packet format is as follows for program tags:

CIP Tag Info command
    uint8_t request_service    0x55
    uint8_t request_path_size  N bytes
      uint8_t   0x91    Symbolic segment header
      uint8_t   name_length   Length in bytes.
      uint8_t   name[N] program name, i.e. 'PROGRAM:foobar'
      (uint8_t padding) optional if program name is odd length.
      uint8_t   0x20    get class
      uint8_t   0x6B    tag info/symbol class
      uint8_t   0x25    get instance (16-bit)
      uint8_t   0x00    padding
      uint8_t   0x00    instance byte 0
      uint8_t   0x00    instance byte 1
    uint16_t  0x04    number of attributes to get
    uint16_t  0x02    attribute #2 - symbol type
    uint16_t  0x07    attribute #7 - base type size (array element) in bytes
    uint16_t  0x08    attribute #8 - array dimensions (3xu32)
    uint16_t  0x01    attribute #1 - symbol name

*/

//
// START_PACK typedef struct {
//    uint8_t request_service;    /* AB_EIP_CMD_CIP_LIST_TAGS=0x55 */
//    uint8_t request_path_size;  /* 3 word = 6 bytes */
//    uint8_t request_path[4];    /* MAGIC
//                                    0x20    get class
//                                    0x6B    tag info/symbol class
//                                    0x25    get instance (16-bit)
//                                    0x00    padding
//                                    0x00    instance byte 0
//                                    0x00    instance byte 1
//                                */
//    uint16_le instance_id;      /* actually last two bytes above */
//    uint16_le num_attributes;   /* 0x04    number of attributes to get */
//    uint16_le requested_attributes[4];  /*
//                                            0x02 attribute #2 - symbol type
//                                            0x07 attribute #7 - base type size (array element) in bytes
//                                            0x08    attribute #8 - array dimensions (3xu32)
//                                            0x01    attribute #1 - symbol name
//                                        */
//
//} END_PACK tag_list_req_DEAD;

/*
 * This is a pseudo UDT structure for each tag entry when listing all the tags
 * in a PLC.
 */

START_PACK typedef struct {
    uint32_le instance_id;    /* monotonically increasing but not contiguous */
    uint16_le symbol_type;    /* type of the symbol. */
    uint16_le element_length; /* length of one array element in bytes. */
    uint32_le array_dims[3];  /* array dimensions. */
    uint16_le string_len;     /* string length count. */
                              // uint8_t string_name[82]; /* MAGIC string name bytes (string_len of them, zero padded) */
} END_PACK tag_list_entry;


static int build_read_request(ab_tag_p tag, int byte_offset);
// static int build_tag_list_request_connected(ab_tag_p tag);
static int build_write_request(ab_tag_p tag, int byte_offset);
static int build_write_bit_request(ab_tag_p tag);
static int check_read_status(ab_tag_p tag);
static int check_write_status(ab_tag_p tag);
static int calculate_write_data_per_packet(ab_tag_p tag);
static bool uses_segments(ab_tag_p tag);
static bool name_has_index(ab_tag_p tag);
static int segment_read_budget(ab_tag_p tag);
static bool reads_in_segments(ab_tag_p tag);
static int encode_segment_path(ab_tag_p tag, uint8_t *dest, int byte_offset, int size);
static int check_size_probe_status(ab_tag_p tag);

static int tag_read_start(plc_tag_p tag_arg);
static int tag_tickler(plc_tag_p tag_arg);
static int tag_write_start(plc_tag_p tag_arg);

/* define the exported vtable for this tag type. */
struct tag_vtable_t eip_cip_vtable = {
    .abort = (tag_vtable_func)ab_tag_abort_request,
    .read = (tag_vtable_func)tag_read_start,
    .status = (tag_vtable_func)ab_tag_status,
    .tickler = (tag_vtable_func)tag_tickler,
    .write = (tag_vtable_func)tag_write_start,
    .wake_plc = NULL,
    .tag_data_written = NULL,

    /* attribute accessors */
    .attribs = ab_attribs,
};

/* default string types used for ControlLogix-class PLCs. */
tag_byte_order_t logix_tag_byte_order = {.is_allocated = 0,

                                         .int16_order = {0, 1},
                                         .int32_order = {0, 1, 2, 3},
                                         .int64_order = {0, 1, 2, 3, 4, 5, 6, 7},
                                         .float32_order = {0, 1, 2, 3},
                                         .float64_order = {0, 1, 2, 3, 4, 5, 6, 7},

                                         .str_is_defined = 1,
                                         .str_is_counted = 1,
                                         .str_is_fixed_length = 1,
                                         .str_is_zero_terminated = 0,
                                         .str_is_byte_swapped = 0,

                                         .str_pad_to_multiple_bytes = 1,
                                         .str_count_word_bytes = 4,
                                         .str_max_capacity = 82,
                                         .str_total_length = 88,
                                         .str_pad_bytes = 2};


/* Omron NJ/NX strings: a 2-byte count, then the characters, zero terminated. */
tag_byte_order_t omron_tag_byte_order = {.is_allocated = 0,

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


/*
 * A PLC without the fragmented services (Omron NJ/NX) moves a variable too large for one packet
 * in byte ranges instead: the plain Read and Write Tag services with a Simple Data Segment, 0x80,
 * 3 words, a u32 byte offset and a u16 byte count, on the end of the path.
 */
#define CIP_DATA_SEGMENT ((uint8_t)0x80)
#define CIP_DATA_SEGMENT_WORDS ((uint8_t)0x03)
#define CIP_DATA_SEGMENT_SIZE (8)

/* the widest type info a reply carries: an abbreviated struct, A0 02 and a CRC. */
#define CIP_MAX_REPLY_TYPE_INFO_SIZE (4)

/* the largest atomic type; a segment is cut on a multiple of it so no member is split. */
#define CIP_MAX_ATOMIC_SIZE (8)

/* an NJ/NX refuses a plain read too large for one reply with this, and does not say the size. */
#define CIP_STATUS_REPLY_TOO_LARGE ((uint8_t)0x11)

#define CIP_SYMBOLIC_SEGMENT ((uint8_t)0x91)
#define CIP_CMD_GET_ATTRIBUTES_ALL ((uint8_t)0x01)
#define CIP_DATA_TYPE_STRING ((uint8_t)0xD0)
#define CIP_DATA_TYPE_SHORT_STRING ((uint8_t)0xDA)


tag_byte_order_t logix_tag_listing_byte_order = {.is_allocated = 0,

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


/*************************************************************************
 **************************** API Functions ******************************
 ************************************************************************/


int tag_tickler(plc_tag_p tag_arg) {
    int rc = PLCTAG_STATUS_OK;
    ab_tag_p tag = (ab_tag_p)tag_arg;

    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_SPEW, tag->tag_id, "Starting.");

    rc = check_request_status(tag);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(tag->read_in_progress) {
        rc = check_read_status(tag);

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

        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_SPEW, tag->tag_id, "Done with status %s.", plc_tag_decode_error(rc));

        return rc;
    }

    if(tag->write_in_progress) {
        rc = check_write_status(tag);

        tag->status = (int8_t)rc;

        /* if the operation completed, make a note so that the callback will be called. */
        if(!tag->write_in_progress) { tag->write_complete = 1; }

        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_SPEW, tag->tag_id, "Done with status %s.", plc_tag_decode_error(rc));

        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_SPEW, tag->tag_id, "Done.  No operation in progress.");

    return tag->status;
}


/*
 * tag_read_start
 *
 * This function must be called only from within one thread, or while
 * the tag's mutex is locked.
 *
 * The function starts the process of getting tag data from the PLC.
 */

int tag_read_start(plc_tag_p tag_arg) {
    int rc = PLCTAG_STATUS_OK;
    ab_tag_p tag = (ab_tag_p)tag_arg;

    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_INFO, tag->tag_id, "Starting");

    if(tag->read_in_progress || tag->write_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    /* mark the tag read in progress */
    tag->read_in_progress = 1;

    rc = build_read_request(tag, tag->offset);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id, "Unable to build read request!");

        tag->read_in_progress = 0;

        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_PENDING;
}


/*
 * tag_write_start
 *
 * This must be called from one thread alone, or while the tag mutex is
 * locked.
 *
 * The routine starts the process of writing to a tag.
 */

int tag_write_start(plc_tag_p tag_arg) {
    int rc = PLCTAG_STATUS_OK;
    ab_tag_p tag = (ab_tag_p)tag_arg;

    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_INFO, tag->tag_id, "Starting");

    if(tag->read_in_progress || tag->write_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id, "Read or write operation already in flight!");
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
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id,
               "No read has completed yet, doing pre-read to get type information.");

        tag->pre_write_read = 1;
        tag->write_in_progress = 0; /* temporarily mask this off */

        return tag_read_start((plc_tag_p)tag);
    }


    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id, "Unable to calculate write sizes!");
        tag->write_in_progress = 0;

        return rc;
    }

    rc = build_write_request(tag, tag->offset);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id, "Unable to build write request!");
        tag->write_in_progress = 0;

        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_PENDING;
}


int build_read_request(ab_tag_p tag, int byte_offset) {
    uint8_t *data = NULL;
    ab_request_p req = NULL;
    int rc = PLCTAG_STATUS_OK;
    uint8_t read_cmd = AB_EIP_CMD_CIP_READ_FRAG;

    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_INFO, tag->tag_id, "Starting.");

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    /* the session writes the EIP and CPF framing; this is only the CIP request. */
    data = req->data;

    /*
     * set up the embedded CIP read packet
     * The format is:
     *
     * uint8_t cmd
     * LLA formatted name
     * uint16_t # of elements to read
     */

    /* without the fragmented service the whole tag comes back in one reply or not at all. */
    read_cmd = ab_plc_type_get(tag->plc_type)->fragmented ? AB_EIP_CMD_CIP_READ_FRAG : AB_EIP_CMD_CIP_READ;

    /* reply_size stays 0, unknown, unless this PLC needs it and it can be worked out. */
    if(tag->size_probe) {
        /* Get Attributes All on the variable gives its size; the reply size is not known. */
        *data++ = CIP_CMD_GET_ATTRIBUTES_ALL;
        mem_copy(data, tag->encoded_name, tag->encoded_name_size);
        data += tag->encoded_name_size;
    } else if(reads_in_segments(tag)) {
        int chunk = segment_read_budget(tag);

        if(chunk > tag->size - byte_offset) { chunk = tag->size - byte_offset; }

        /* the plain Read Tag service, the byte range in the path, and one element. */
        *data++ = AB_EIP_CMD_CIP_READ;
        data += encode_segment_path(tag, data, byte_offset, chunk);
        *((uint16_le *)data) = h2le16((uint16_t)1);
        data += sizeof(uint16_le);

        req->reply_size = (int)sizeof(cip_header) + CIP_MAX_REPLY_TYPE_INFO_SIZE + chunk;

        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, "Reading a segment of %d bytes at %d.", chunk, byte_offset);
    } else {
        *data = read_cmd;
        data++;

        /* copy the tag name into the request */
        mem_copy(data, tag->encoded_name, tag->encoded_name_size);
        data += tag->encoded_name_size;

        /* add the count of elements to read. */
        *((uint16_le *)data) = h2le16((uint16_t)(tag->elem_count));
        data += sizeof(uint16_le);

        if(read_cmd == AB_EIP_CMD_CIP_READ_FRAG) {
            /* add the byte offset for this request */
            *((uint32_le *)data) = h2le32((uint32_t)byte_offset);
            data += sizeof(uint32_le);
        } else if(tag->size > 0) {
            /* the size the last read found.  A create read does not know it, so it goes alone. */
            req->reply_size = (int)sizeof(cip_header) + CIP_MAX_REPLY_TYPE_INFO_SIZE + tag->size;
        }
    }


    /* Check if the payload size exceeds available space before setting request_size */
    int packet_payload_size = (int)(data - req->data);
    int available_payload = session_get_available_cip_payload_space(tag->session);

    if(packet_payload_size > available_payload) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id, "Request payload (%d bytes) exceeds available space (%d bytes)!",
               packet_payload_size, available_payload);
        ab_tag_abort_request(tag);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* set the size of the request */
    req->request_size = (int)(data - (req->data));
    req->connected = (tag->use_connected_msg != 0);

    req->allow_packing = tag->allow_packing;

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id, "Unable to add request to session! rc=%d", rc);
        ab_tag_abort_request(tag);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_INFO, tag->tag_id, "Done");

    return PLCTAG_STATUS_OK;
}




int build_write_bit_request(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    uint8_t *data = NULL;
    ab_request_p req = NULL;
    int i;

    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_INFO, tag->tag_id, "Starting.");

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    rc = calculate_write_data_per_packet(tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_ERROR, tag->tag_id, "Unable to calculate valid write data per packet!.  rc=%s",
               plc_tag_decode_error(rc));
        return rc;
    }

    if(tag->write_data_per_packet < (tag->size * 2) + 2) { /* 2 masks plus a count word. */
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_ERROR, tag->tag_id, "Insufficient space to write bit masks!");
        return PLCTAG_ERR_TOO_SMALL;
    }


    /* the session writes the EIP and CPF framing; this is only the CIP request. */
    data = req->data;

    /*
     * set up the embedded CIP read packet
     * The format is:
     *
     * uint8_t cmd
     * LLA formatted name
     * uint16_t # size of a mask element
     * OR mask
     * AND mask
     */

    /*
     * set up the CIP Read-Modify-Write request type.
     */
    *data = AB_EIP_CMD_CIP_RMW;
    data++;

    /* copy the tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* write an INT of the mask size. */
    *data = (uint8_t)(tag->elem_size & 0xFF);
    data++;
    *data = (uint8_t)((tag->elem_size >> 8) & 0xFF);
    data++;

    /* write the OR mask */
    for(i = 0; i < tag->elem_size; i++) {
        if((tag->bit / 8) == i) {
            uint8_t mask = (uint8_t)(1 << (tag->bit % 8));

            /* if the bit is set, then we want to mask it on. */
            if(tag->data[tag->bit / 8] & mask) {
                *data = mask;
            } else {
                *data = (uint8_t)0;
            }

            pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, "adding OR mask byte %d: %x", i, *data);

            data++;
        } else {
            /* this is not the data we care about. */
            *data = (uint8_t)0;

            pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, "adding OR mask byte %d: %x", i, *data);

            data++;
        }
    }

    /* write the AND mask */
    for(i = 0; i < tag->elem_size; i++) {
        if((tag->bit / 8) == i) {
            uint8_t mask = (uint8_t)(1 << (tag->bit % 8));

            /* if the bit is set, then we want to _not_ mask it off. */
            if(tag->data[tag->bit / 8] & mask) {
                *data = (uint8_t)0xFF;
            } else {
                *data = (uint8_t)(~mask);
            }

            pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, "adding AND mask byte %d: %x", i, *data);

            data++;
        } else {
            /* this is not the data we care about. */
            *data = (uint8_t)0xFF;

            pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, "adding AND mask byte %d: %x", i, *data);

            data++;
        }
    }

    /* let the rest of the system know that the write is complete after this. */
    tag->offset = tag->size;


    /* set the size of the request */
    req->request_size = (int)(data - (req->data));
    req->connected = (tag->use_connected_msg != 0);

    /* allow packing if the tag allows it. */
    req->allow_packing = tag->allow_packing;

    /* a bit write reply is only its header. */
    if(!ab_plc_type_get(tag->plc_type)->fragmented) { req->reply_size = (int)sizeof(cip_header); }

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_ERROR, tag->tag_id, "Unable to add request to session! rc=%d", rc);
        ab_tag_abort_request(tag);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_INFO, tag->tag_id, "Done");

    return PLCTAG_STATUS_OK;
}




int build_write_request(ab_tag_p tag, int byte_offset) {
    int rc = PLCTAG_STATUS_OK;
    uint8_t *data = NULL;
    ab_request_p req = NULL;
    int multiple_requests = 0;
    int write_size = 0;
    int str_pad_to_multiple_bytes = 1;
    bool segmented = false;

    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_INFO, tag->tag_id, "Starting.");

    if(tag->is_bit) { return build_write_bit_request(tag); }

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    rc = calculate_write_data_per_packet(tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_ERROR, tag->tag_id, "Unable to calculate valid write data per packet!.  rc=%s",
               plc_tag_decode_error(rc));
        return rc;
    }

    if(tag->write_data_per_packet < tag->size) { multiple_requests = 1; }

    segmented = multiple_requests && uses_segments(tag);

    if(multiple_requests && !segmented && !ab_plc_type_get(tag->plc_type)->fragmented) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id, "Tag too large for one write and this PLC has no fragmented write!");
        rc_dec(req);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* a write reply is only its header. */
    if(!ab_plc_type_get(tag->plc_type)->fragmented) { req->reply_size = (int)sizeof(cip_header); }


    /* the session writes the EIP and CPF framing; this is only the CIP request. */
    data = req->data;

    /*
     * set up the embedded CIP read packet
     * The format is:
     *
     * uint8_t cmd
     * LLA formatted name
     * data type to write
     * uint16_t # of elements to write
     * data to write
     */

    /*
     * set up the CIP Read request type.
     * Different if more than one request.
     *
     * This handles a bug where attempting fragmented requests
     * does not appear to work with a single boolean.
     */
    *data = (multiple_requests && !segmented) ? AB_EIP_CMD_CIP_WRITE_FRAG : AB_EIP_CMD_CIP_WRITE;
    data++;

    /* how much data to write? */
    write_size = tag->size - tag->offset;

    if(write_size > tag->write_data_per_packet) { write_size = tag->write_data_per_packet; }

    /* copy the tag name into the request, with the byte range on the end for a segment. */
    if(segmented) {
        data += encode_segment_path(tag, data, byte_offset, write_size);
    } else {
        mem_copy(data, tag->encoded_name, tag->encoded_name_size);
        data += tag->encoded_name_size;
    }

    /* copy encoded type info */
    if(tag->encoded_type_info_size) {
        mem_copy(data, tag->encoded_type_info, tag->encoded_type_info_size);
        data += tag->encoded_type_info_size;
    } else {
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id, "Data type unsupported!");
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* copy the item count, little endian.  A segment is one element of its byte range. */
    *((uint16_le *)data) = h2le16((uint16_t)(segmented ? 1 : tag->elem_count));
    data += sizeof(uint16_le);

    if(multiple_requests && !segmented) {
        /* put in the byte offset */
        *((uint32_le *)data) = h2le32((uint32_t)(byte_offset));
        data += sizeof(uint32_le);
    }

    /* now copy the data to write */
    mem_copy(data, tag->data + tag->offset, write_size);
    data += write_size;
    tag->offset += write_size;

    /* need to pad data to multiple of either 1, 2 or 4 bytes */
    /* for some PLCs (OmronNJ), padding causes issues when writing counted strings as it creates a mismatch between
        the length of the string and the count integer, therefor this padding can be disabled using the str_pad_16_bits attribute
     */
    str_pad_to_multiple_bytes = (int)tag->byte_order->str_pad_to_multiple_bytes;
    if(!segmented && (str_pad_to_multiple_bytes == 2 || str_pad_to_multiple_bytes == 4) && write_size != 0) {
        if(write_size % str_pad_to_multiple_bytes != 0) {
            int pad_size = str_pad_to_multiple_bytes - (write_size % str_pad_to_multiple_bytes);
            for(int i = 0; i < pad_size; i++) {
                *data = 0;
                data++;
            }
        }
    }


    /* set the size of the request */
    req->request_size = (int)(data - (req->data));
    req->connected = (tag->use_connected_msg != 0);

    /* allow packing if the tag allows it. */
    req->allow_packing = tag->allow_packing;

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_ERROR, tag->tag_id, "Unable to add request to session! rc=%d", rc);
        ab_tag_abort_request(tag);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_INFO, tag->tag_id, "Done");

    return PLCTAG_STATUS_OK;
}




/*
 * check_read_status
 *
 * This routine checks for any outstanding requests and copies in data
 * that has arrived.  At the end of the request, it will clean up the request
 * buffers.  This is not thread-safe!  It should be called with the tag mutex
 * locked!
 */

static int check_read_status(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    cip_header *cip_resp;
    uint8_t *data;
    uint8_t *data_end;
    int partial_data = 0;
    bool segmented = reads_in_segments(tag);

    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_SPEW, tag->tag_id, "Starting.");

    if(tag->size_probe) { return check_size_probe_status(tag); }

    /* The request is valid. */

    /* point to the data */
    cip_resp = (cip_header *)(tag->req->data);

    /* point to the start of the data */
    data = (tag->req->data) + sizeof(cip_header);

    /* point the end of the data */
    data_end = tag->req->data + tag->req->request_size;

    do {
        ptrdiff_t payload_size = 0;

        /* check the status */
        if(cip_resp->reply_service != (AB_EIP_CMD_CIP_READ_FRAG | AB_EIP_CMD_CIP_OK)
           && cip_resp->reply_service != (AB_EIP_CMD_CIP_READ | AB_EIP_CMD_CIP_OK)) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id, "CIP response reply service unexpected: %d",
                   cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        /* a partial reply to a plain read cannot be continued, so it is the error it says it is. */
        if(cip_resp->status != AB_CIP_STATUS_OK
           && (cip_resp->status != AB_CIP_STATUS_FRAG || !ab_plc_type_get(tag->plc_type)->fragmented)) {
            size_t status_size = cip_error_data_size((uint8_t *)&cip_resp->status, data_end);

            pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id, "CIP read failed with status: 0x%x %s", cip_resp->status,
                   decode_cip_error_short((uint8_t *)&cip_resp->status, status_size));
            pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_INFO, tag->tag_id,
                   decode_cip_error_long((uint8_t *)&cip_resp->status, status_size));

            rc = decode_cip_error_code((uint8_t *)&cip_resp->status, status_size);

            /* too large for one reply: find the size, then read it in segments. */
            if(cip_resp->status == CIP_STATUS_REPLY_TOO_LARGE && !segmented && uses_segments(tag)) {
                pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, "Too large for one reply, asking for its size.");
                tag->size_probe = true;
                rc = PLCTAG_STATUS_PENDING;
            }

            break;
        }

        /* check to see if this is a partial response. */
        partial_data = (cip_resp->status == AB_CIP_STATUS_FRAG);

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
                pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, "type byte = %d (0x%02x)", (int)*data, (int)*data);

                if(cip_lookup_encoded_type_size(*data, &type_length) == PLCTAG_STATUS_OK) {
                    /* found it and we got the type data size */

                    /* some types use the second byte to indicate how many bytes more are used. */
                    if(type_length == 0) {
                        if(payload_size < 2) {
                            pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id,
                                   "Response too short to hold extended type length byte!");
                            rc = PLCTAG_ERR_TOO_SMALL;
                            break;
                        }

                        type_length = *(data + 1) + 2;
                    }

                    if(type_length <= 0 || type_length > (int)sizeof(tag->encoded_type_info)
                       || type_length > (int)payload_size) {
                        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id,
                               "Type data length %d for type byte 0x%02x is out of range (max %d, available %d)!",
                               type_length, *data, (int)sizeof(tag->encoded_type_info), (int)payload_size);
                        rc = PLCTAG_ERR_TOO_LARGE;
                        break;
                    }

                    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, "Type data is %d bytes long.", type_length);
                    pdebug_dump_bytes(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, data, type_length);

                    tag->encoded_type_info_size = type_length;
                    mem_copy(tag->encoded_type_info, data, tag->encoded_type_info_size);
                } else {
                    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id, "Unsupported data type returned, type byte=0x%02x",
                           *data);
                    rc = PLCTAG_ERR_UNSUPPORTED;
                    break;
                }
            }

            /* skip past the type data */
            data += (tag->encoded_type_info_size);

            if((intptr_t)data > (intptr_t)data_end) {
                pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id,
                       "Response too short to hold remembered type info of %d bytes!", tag->encoded_type_info_size);
                rc = PLCTAG_ERR_TOO_SMALL;
                break;
            }

            /* check payload size now that we have bumped past the data type info. */
            payload_size = (data_end - data);

            /* copy the data into the tag and realloc if we need more space. */
            if(payload_size + tag->offset > tag->size) {
                /* a PLC can keep returning fragments forever.  Do not grow without bound. */
                if((payload_size + tag->offset) > (ptrdiff_t)AB_MAX_TAG_DATA_SIZE) {
                    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id,
                           "Tag data size of %d bytes is larger than the maximum of %d bytes!",
                           (int)(payload_size + tag->offset), AB_MAX_TAG_DATA_SIZE);
                    rc = PLCTAG_ERR_TOO_LARGE;
                    break;
                }

                tag->size = (int)payload_size + tag->offset;
                tag->elem_size = tag->size / tag->elem_count;

                pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, "Increasing tag buffer size to %d bytes.", tag->size);

                tag->data = (uint8_t *)mem_realloc(tag->data, tag->size);
                if(!tag->data) {
                    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id, "Unable to reallocate tag data memory!");
                    rc = PLCTAG_ERR_NO_MEM;
                    break;
                }
            }

            pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_INFO, tag->tag_id, "Got %d bytes of data", (int)payload_size);

            /*
             * copy the data, but only if this is not
             * a pre-read for a subsequent write!  We do not
             * want to overwrite the data the upstream has
             * put into the tag's data buffer.
             */
            if(!tag->pre_write_read) { mem_copy(tag->data + tag->offset, data, (int)(payload_size)); }

            /* bump the byte offset */
            tag->offset += (int)(payload_size);

            /* a segmented transfer is partial until every byte has come back. */
            if(segmented && tag->offset < tag->size) { partial_data = 1; }
        } else {
            pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, "Response returned no data and no error.");

            /* no payload means no forward progress on a fragmented transfer. */
            tag->fragment_retry_count++;

            if(segmented) { partial_data = 1; }
        }

        /* set the return code */
        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* clean up the request */
    ab_tag_abort_request_only(tag);

    /* the size probe goes next. */
    if(rc == PLCTAG_STATUS_PENDING) { rc = tag_read_start((plc_tag_p)tag); }

    /* are we actually done? */
    if(rc == PLCTAG_STATUS_OK) {
        /* skip if we are doing a pre-write read. */
        if(!tag->pre_write_read && partial_data && tag->fragment_retry_count > MAX_FRAGMENT_RETRIES) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id,
                   "Got %d fragment responses in a row with no data.  The transfer is not making progress, giving up.",
                   tag->fragment_retry_count);
            rc = PLCTAG_ERR_PARTIAL;
        } else if(!tag->pre_write_read && partial_data) {
            /* call read start again to get the next piece */
            pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, "calling tag_read_start() to get the next chunk.");
            rc = tag_read_start((plc_tag_p)tag);
        } else {
            tag->offset = 0;

            /* the transfer is over one way or the other, so start the next one clean. */
            tag->fragment_retry_count = 0;

            /* if this is a pre-read for a write, then pass off to the write routine */
            if(tag->pre_write_read) {
                pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, "Restarting write call now.");
                tag->pre_write_read = 0;
                rc = tag_write_start((plc_tag_p)tag);
            } else {
                pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, "Read complete.");
            }
        }
    }

    /* this is not an else clause because the above if could result in bad rc. */
    if(rc != PLCTAG_STATUS_PENDING) {
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id, "Error reading tag data! rc=%d %s", rc,
                   plc_tag_decode_error(rc));
        }

        /* clean up everything if this tag is not pending. */
        ab_tag_abort_request(tag);
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}




/*
 * check_write_status
 *
 * This routine must be called with the tag mutex locked.  It checks the current
 * status of a write operation.  If the write is done, it triggers the clean up.
 */

static int check_write_status(ab_tag_p tag) {
    cip_header *cip_resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_SPEW, tag->tag_id, "Starting.");

    /* the request reference is valid. */

    /* point to the data */
    cip_resp = (cip_header *)(tag->req->data);

    do {
        if(cip_resp->reply_service != (AB_EIP_CMD_CIP_WRITE_FRAG | AB_EIP_CMD_CIP_OK)
           && cip_resp->reply_service != (AB_EIP_CMD_CIP_WRITE | AB_EIP_CMD_CIP_OK)
           && cip_resp->reply_service != (AB_EIP_CMD_CIP_RMW | AB_EIP_CMD_CIP_OK)) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id, "CIP response reply service unexpected: %d",
                   cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(cip_resp->status != AB_CIP_STATUS_OK && cip_resp->status != AB_CIP_STATUS_FRAG) {
            size_t status_size = cip_error_data_size((uint8_t *)&cip_resp->status, tag->req->data + tag->req->request_size);

            pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id, "CIP read failed with status: 0x%x %s", cip_resp->status,
                   decode_cip_error_short((uint8_t *)&cip_resp->status, status_size));
            pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_INFO, tag->tag_id,
                   decode_cip_error_long((uint8_t *)&cip_resp->status, status_size));
            rc = decode_cip_error_code((uint8_t *)&cip_resp->status, status_size);
            break;
        }
    } while(0);

    ab_tag_abort_request_only(tag);

    if(rc == PLCTAG_STATUS_OK) {
        if(tag->offset < tag->size) {

            pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, "Write not complete, triggering next round.");
            rc = tag_write_start((plc_tag_p)tag);
        } else {
            /* only clear this if we are done. */
            tag->offset = 0;
        }
    } else {
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id, "Write failed!");

        tag->offset = 0;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}




int calculate_write_data_per_packet(ab_tag_p tag) {
    int overhead = 0;
    int data_per_packet = 0;
    int available_payload = 0;

    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, "Starting.");

    /* if we are here, then we have all the type data etc. */
    available_payload = session_get_available_cip_payload_space(tag->session);

    if(tag->use_connected_msg) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, "Connected tag.");
        overhead = 1                             /* service request, one byte */
                   + tag->encoded_name_size      /* full encoded name */
                   + tag->encoded_type_info_size /* encoded type size */
                   + 2                           /* element count, 16-bit int */
                   + 4                           /* byte offset, 32-bit int */
                   + 8;                          /* MAGIC fudge factor */
    } else {
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, "Unconnected tag.");
        overhead = 1                             /* CIP service Unconnected Send */
                   + 1                           /* path size */
                   + 4                           /* Connection Manager 20 06 24 1 */
                   + 1                           /* seconds per tick */
                   + 1                           /* timeout ticks */
                   + 2                           /* Embedded payload size */
                   + 1                           /* service request, one byte */
                   + tag->encoded_name_size      /* full encoded name */
                   + tag->encoded_type_info_size /* encoded type size */
                   + 2                           /* element count, 16-bit int */
                   + 4                           /* byte offset, 32-bit int */
                   + 8;                          /* MAGIC fudge factor */
    }

    /* a data segment in the path takes eight bytes where the offset field took four. */
    if(!ab_plc_type_get(tag->plc_type)->fragmented) { overhead += CIP_DATA_SEGMENT_SIZE - 4; }

    /* make sure that overhead is an even number of bytes */
    if(overhead & 1) { overhead++; }

    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, "Write overhead is %d bytes.", overhead);

    data_per_packet = available_payload - overhead;

    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id,
           "Write packet available payload is %d, write overhead is %d, and write data per packet is %d.", available_payload,
           overhead, data_per_packet);

    if(data_per_packet <= 0) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id,
               "Unable to send request.  Packet overhead, %d bytes, is too large for available payload, %d bytes!", overhead,
               available_payload);
        return PLCTAG_ERR_TOO_LARGE;
    }

    int elements_per_packet = 0;
    int element_size = 0;

    /*
     * elem_size is derived from the PLC's response as tag->size / tag->elem_count, so a PLC
     * that returns fewer bytes than the tag has elements drives it to zero.  It is the
     * divisor below, so check it here.
     */
    if(tag->elem_size <= 0) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id, "Tag element size of %d bytes is not usable!", tag->elem_size);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* if the tag size is less than 8 bytes, then use a multiple of the tag size.  Otherwise use
    8 bytes as the unit */
    if(tag->elem_size < 8) {
        elements_per_packet = data_per_packet / tag->elem_size;
        data_per_packet = elements_per_packet * tag->elem_size;
        element_size = tag->elem_size;
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, "Using tag size %d bytes for element size.", element_size);
    } else {
        /* round down to the nearest multiple of 8 bytes */
        elements_per_packet = data_per_packet / 8;
        data_per_packet = elements_per_packet * 8;
        element_size = 8;
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, "Using element size %d bytes.", element_size);
    }

    if(elements_per_packet < 1) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id,
               "Unable to send request.  Available payload, %d bytes, is too small to write at least %d bytes!",
               available_payload, element_size);
        return PLCTAG_ERR_TOO_LARGE;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, "Write data per packet is %d bytes.", data_per_packet);

    tag->write_data_per_packet = data_per_packet;

    pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * Segments are for a PLC without the fragmented services, a whole variable rather than an
 * indexed element, and elements on a fixed stride.  A string array has no stride: each
 * element is a count and that many characters, so a byte range does not name elements.
 */
static bool uses_segments(ab_tag_p tag) {
    if(ab_plc_type_get(tag->plc_type)->fragmented || tag->is_bit || name_has_index(tag)) { return false; }

    if(tag->elem_type == AB_TYPE_STRING || tag->elem_type == AB_TYPE_SHORT_STRING) { return false; }

    if(tag->encoded_type_info_size > 0
       && (tag->encoded_type_info[0] == CIP_DATA_TYPE_STRING || tag->encoded_type_info[0] == CIP_DATA_TYPE_SHORT_STRING)) {
        return false;
    }

    return true;
}


/* true when the encoded name addresses an array element rather than a whole variable. */
static bool name_has_index(ab_tag_p tag) {
    int i = 1; /* past the word count */

    while(i < tag->encoded_name_size) {
        int name_len = 0;

        /* anything but a symbolic segment is an element (or other) segment. */
        if(tag->encoded_name[i] != CIP_SYMBOLIC_SEGMENT || i + 1 >= tag->encoded_name_size) { return true; }

        name_len = tag->encoded_name[i + 1];
        i += 2 + name_len + (name_len & 1);
    }

    return false;
}


/*
 * The most data one segment can ask for: the reply has to hold it with its CIP header and type
 * info.  It is cut on the boundaries a split write uses, so no atomic member is split.
 */
static int segment_read_budget(ab_tag_p tag) {
    int split = (tag->elem_size > 0 && tag->elem_size < CIP_MAX_ATOMIC_SIZE) ? tag->elem_size : CIP_MAX_ATOMIC_SIZE;
    int budget = session_get_available_cip_payload_space(tag->session) - (int)sizeof(cip_header) - CIP_MAX_REPLY_TYPE_INFO_SIZE
                 - 8; /* MAGIC fudge factor, as for writes */

    return (budget / split) * split;
}


static bool reads_in_segments(ab_tag_p tag) { return tag->size > segment_read_budget(tag) && uses_segments(tag); }


/* the tag's encoded name with a Simple Data Segment for size bytes at byte_offset appended. */
static int encode_segment_path(ab_tag_p tag, uint8_t *dest, int byte_offset, int size) {
    uint8_t *data = dest;

    *data++ = (uint8_t)(tag->encoded_name[0] + CIP_DATA_SEGMENT_SIZE / 2);
    mem_copy(data, tag->encoded_name + 1, tag->encoded_name_size - 1);
    data += tag->encoded_name_size - 1;

    *data++ = CIP_DATA_SEGMENT;
    *data++ = CIP_DATA_SEGMENT_WORDS;
    *((uint32_le *)data) = h2le32((uint32_t)byte_offset);
    data += sizeof(uint32_le);
    *((uint16_le *)data) = h2le16((uint16_t)size);
    data += sizeof(uint16_le);

    return (int)(data - dest);
}


/*
 * The reply to Get Attributes All on the variable: its size (4), type (1) and array element
 * type (1).  The size is one element's, so the tag's elem_count elements from the start are
 * then read in segments.
 *
 * ponytail: that an NJ/NX refuses an oversized read with 0x11, rather than another error, is
 * not confirmed on hardware; the simulator does.
 */
static int check_size_probe_status(ab_tag_p tag) {
    cip_header *cip_resp = (cip_header *)(tag->req->data);
    uint8_t *data = tag->req->data + sizeof(cip_header);
    uint8_t *data_end = tag->req->data + tag->req->request_size;
    int64_t total = 0;
    int rc = PLCTAG_STATUS_OK;

    tag->size_probe = false;

    do {
        if(cip_resp->reply_service != (CIP_CMD_GET_ATTRIBUTES_ALL | AB_EIP_CMD_CIP_OK)) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id, "CIP response reply service unexpected: %d",
                   cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(cip_resp->status != AB_CIP_STATUS_OK) {
            size_t status_size = cip_error_data_size((uint8_t *)&cip_resp->status, data_end);

            pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id, "Variable size request failed with status: 0x%x %s",
                   cip_resp->status, decode_cip_error_short((uint8_t *)&cip_resp->status, status_size));
            rc = decode_cip_error_code((uint8_t *)&cip_resp->status, status_size);
            break;
        }

        if(data_end - data < 6) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id, "Variable attributes reply is too short!");
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        total = (int64_t)((uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24))
                * tag->elem_count;

        if(data[4] == CIP_DATA_TYPE_STRING || data[5] == CIP_DATA_TYPE_STRING || total <= 0 || total > AB_MAX_TAG_DATA_SIZE) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id,
                   "Variable of %" PRId64 " bytes, type 0x%02x, cannot be read in segments!", total, (unsigned int)data[4]);
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        if(total != tag->size) {
            uint8_t *new_data = (uint8_t *)mem_realloc(tag->data, (int)total);

            if(!new_data) {
                rc = PLCTAG_ERR_NO_MEM;
                break;
            }

            tag->data = new_data;
            tag->size = (int)total;
        }

        tag->elem_size = tag->size / tag->elem_count;
        tag->offset = 0;

        /* small enough for one read means the refusal was not about size; asking again would loop. */
        if(!reads_in_segments(tag)) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id, "Read of %d bytes was refused as too large!", tag->size);
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_DETAIL, tag->tag_id, "Variable is %d bytes, reading it in segments.", tag->size);
    } while(0);

    ab_tag_abort_request_only(tag);

    if(rc == PLCTAG_STATUS_OK) { rc = tag_read_start((plc_tag_p)tag); }

    if(rc != PLCTAG_STATUS_PENDING) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP, DEBUG_WARN, tag->tag_id, "Error reading tag data! rc=%d %s", rc, plc_tag_decode_error(rc));
        ab_tag_abort_request(tag);
    }

    return rc;
}
