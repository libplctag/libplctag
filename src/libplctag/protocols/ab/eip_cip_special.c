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
#include <libplctag/protocols/ab/error_codes.h>
#include <libplctag/protocols/ab/session.h>
#include <libplctag/protocols/ab/tag.h>
#include <platform.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/vector.h>


/* byte offset of the UDT handle/type attribute within a Get_Attributes_List response payload */
#define UDT_METADATA_HANDLE_OFFSET (28)

/*
 * size of the fixed-size fields preceding the CIP response in an identity reply:
 * interface_handle (one uint32_le) followed by timeout, item_count, NAI type, NAI length,
 * UDI type, and UDI length (six uint16_le fields).
 */


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


/* raw tag functions */
// static int raw_tag_read_start(ab_tag_p tag);
static int raw_tag_tickler(plc_tag_p tag_arg);
static int raw_tag_write_start(plc_tag_p tag_arg);
static int raw_tag_check_write_status(ab_tag_p tag);
static int raw_tag_build_write_request(ab_tag_p tag);


/* listing tag functions. */
static int listing_tag_read_start(plc_tag_p tag_arg);
static int listing_tag_tickler(plc_tag_p tag_arg);
// static int listing_tag_write_start(ab_tag_p tag);
static int listing_tag_check_read_status(ab_tag_p tag);
static int listing_tag_build_read_request(ab_tag_p tag);


/* UDT tag functions. */
static int udt_tag_read_start(plc_tag_p tag_arg);
static int udt_tag_tickler(plc_tag_p tag_arg);
// static int listing_tag_write_start(ab_tag_p tag);
static int udt_tag_check_read_metadata_status(ab_tag_p tag);
static int udt_tag_build_read_metadata_request(ab_tag_p tag);
static int udt_tag_check_read_fields_status(ab_tag_p tag);
static int udt_tag_build_read_fields_request(ab_tag_p tag);
static uint32_t udt_fields_size(uint32_t words);


/* define the vtable for raw tag type. */
struct tag_vtable_t raw_tag_vtable = {
    .abort = ab_tag_vtable_abort,
    .read = NULL,
    .status = ab_tag_status,
    .tickler = raw_tag_tickler,
    .write = raw_tag_write_start,
    .wake_plc = NULL,
    .tag_data_written = NULL,

    /* attribute accessors */
    .attribs = ab_attribs,
};

/* define the vtable for listing tag type. */
struct tag_vtable_t listing_tag_vtable = {
    .abort = ab_tag_vtable_abort,
    .read = listing_tag_read_start,
    .status = ab_tag_status,
    .tickler = listing_tag_tickler,
    .write = NULL,
    .wake_plc = NULL,
    .tag_data_written = NULL,

    /* attribute accessors */
    .attribs = ab_attribs,
};


/* define the vtable for udt tag type. */
struct tag_vtable_t udt_tag_vtable = {
    .abort = ab_tag_vtable_abort,
    .read = udt_tag_read_start,
    .status = ab_tag_status,
    .tickler = udt_tag_tickler,
    .write = NULL,
    .wake_plc = NULL,
    .tag_data_written = NULL,

    /* attribute accessors */
    .attribs = ab_attribs,
};


tag_byte_order_t listing_tag_logix_byte_order = {.is_allocated = 0,

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

/* strings are zero terminated. */
tag_byte_order_t udt_tag_logix_byte_order = {.is_allocated = 0,

                                             .int16_order = {0, 1},
                                             .int32_order = {0, 1, 2, 3},
                                             .int64_order = {0, 1, 2, 3, 4, 5, 6, 7},
                                             .float32_order = {0, 1, 2, 3},
                                             .float64_order = {0, 1, 2, 3, 4, 5, 6, 7},

                                             .str_is_defined = 1,
                                             .str_is_counted = 0,
                                             .str_is_fixed_length = 0,
                                             .str_is_zero_terminated = 1,
                                             .str_is_byte_swapped = 0,

                                             .str_pad_to_multiple_bytes = 1,
                                             .str_count_word_bytes = 0,
                                             .str_max_capacity = 0,
                                             .str_total_length = 0,
                                             .str_pad_bytes = 0};


/*************************************************************************
 **************************** API Functions ******************************
 ************************************************************************/


/* Raw tag functions */


int setup_raw_tag(ab_tag_p tag) {
    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Starting.");

    /* set up raw tag. */
    tag->special_tag = 1;
    tag->elem_type = AB_TYPE_TAG_RAW;
    tag->elem_count = 1;
    tag->elem_size = 1;

    tag->byte_order = &logix_tag_byte_order;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Setting vtable to %p.", (void *)&raw_tag_vtable);

    tag->vtable = &raw_tag_vtable;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}


int raw_tag_tickler(plc_tag_p tag_arg) {
    ab_tag_p tag = (ab_tag_p)tag_arg;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Starting.");

    rc = check_request_status(tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Error %s getting request status!",
               plc_tag_decode_error(rc));
        return rc;
    }

    if(tag->read_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Something started a read on a raw tag.  This is not supported!");
        ab_tag_abort_request(tag);

        /* fire the event anyway */
        tag->read_complete = 1;
        return PLCTAG_ERR_UNSUPPORTED;
    }

    if(tag->write_in_progress) {
        rc = raw_tag_check_write_status(tag);
        tag->status = (int8_t)rc;

        /* if the operation completed, make a note so that the callback will be called. */
        if(!tag->write_in_progress) { tag->write_complete = 1; }

        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Done.");
        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Done.  No operation in progress.");

    return tag->status;
}


/*
 * raw_tag_write_start
 *
 * This must be called from one thread alone, or while the tag mutex is
 * locked.
 *
 * The routine starts the process of writing to a tag.
 */
int raw_tag_write_start(plc_tag_p tag_arg) {
    ab_tag_p tag = (ab_tag_p)tag_arg;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Starting");

    if(tag->read_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Raw tag found with a read in flight!");
        return PLCTAG_ERR_BAD_STATUS;
    }

    if(tag->write_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    /* the write is now in flight */
    tag->write_in_progress = 1;

    rc = raw_tag_build_write_request(tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unable to build write request!");
        tag->write_in_progress = 0;
        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_PENDING;
}


/*
 * raw_tag_check_write_status
 *
 * This routine must be called with the tag mutex locked.  The whole CIP reply, header
 * included, becomes the tag's data.
 */
int raw_tag_check_write_status(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    int data_size = tag->req->request_size;
    uint8_t *tag_data_buffer = NULL;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Starting.");

    tag_data_buffer = mem_realloc(tag->data, data_size);
    if(tag_data_buffer) {
        tag->data = tag_data_buffer;
        tag->size = data_size;
        mem_copy(tag->data, tag->req->data, data_size);
    } else {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unable to reallocate tag data buffer!");
        rc = PLCTAG_ERR_NO_MEM;
    }

    /* clean up regardless */
    ab_tag_abort_request(tag);

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}


/* The application wrote the whole CIP request into the tag; send it as it is. */
int raw_tag_build_write_request(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Starting.");

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &tag->req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  rc=%s",
               plc_tag_decode_error(rc));
        return rc;
    }

    if(tag->size > tag->req->request_capacity || tag->size > session_get_available_cip_payload_space(tag->session)) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Raw request of %d bytes is too large!", tag->size);
        ab_tag_abort_request(tag);
        return PLCTAG_ERR_TOO_LARGE;
    }

    mem_copy(tag->req->data, tag->data, tag->size);
    tag->req->request_size = tag->size;
    tag->req->connected = (tag->use_connected_msg != 0);

    /* allow packing if the tag allows it. */
    tag->req->allow_packing = tag->allow_packing;

    /* reset the tag size so that incoming data overwrites the old. */
    tag->size = 0;

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, tag->req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to add request to session! Error %s",
               plc_tag_decode_error(rc));
        ab_tag_abort_request(tag);
        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done");

    return PLCTAG_STATUS_OK;
}


/******************************************************************
 ******************* identity tag functions ***********************
 ******************************************************************/

/* identity tag functions */
static int identity_tag_read_start(plc_tag_p tag_arg);
static int identity_tag_tickler(plc_tag_p tag_arg);
static int identity_tag_check_read_status(ab_tag_p tag);
static int identity_tag_build_read_request(ab_tag_p tag);


/* define the vtable for identity tag type. */
struct tag_vtable_t identity_tag_vtable = {.abort = ab_tag_vtable_abort,
                                           .read = identity_tag_read_start,
                                           .status = ab_tag_status,
                                           .tickler = identity_tag_tickler,
                                           .write = NULL,
                                           .wake_plc = NULL,

                                           /* attribute accessors */
                                           .attribs = ab_attribs};


int setup_identity_tag(ab_tag_p tag) {
    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Starting.");

    /* set up identity tag */
    tag->special_tag = 1;
    tag->elem_type = AB_TYPE_TAG_IDENTITY;
    tag->elem_count = 1;
    tag->elem_size = 1;

    tag->byte_order = &logix_tag_byte_order;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Setting vtable to %p.", (void *)&identity_tag_vtable);

    tag->vtable = &identity_tag_vtable;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}


int identity_tag_read_start(plc_tag_p tag_arg) {
    ab_tag_p tag = (ab_tag_p)tag_arg;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Starting");

    if(tag->write_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Identity tag found with a write in flight!");
        return PLCTAG_ERR_BAD_STATUS;
    }

    if(tag->read_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Read operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    /* mark the tag read in progress */
    tag->read_in_progress = 1;

    rc = identity_tag_build_read_request(tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unable to build identity read request!");
        tag->read_in_progress = 0;
        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_PENDING;
}


int identity_tag_tickler(plc_tag_p tag_arg) {
    ab_tag_p tag = (ab_tag_p)tag_arg;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Starting.");

    rc = check_request_status(tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Error %s getting request status!",
               plc_tag_decode_error(rc));
        return rc;
    }

    if(tag->write_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Identity tags do not support writes!");
        ab_tag_abort_request(tag);
        tag->write_complete = 1;
        return PLCTAG_ERR_UNSUPPORTED;
    }

    if(tag->read_in_progress) {
        rc = identity_tag_check_read_status(tag);
        tag->status = (int8_t)rc;

        if(!tag->read_in_progress) { tag->read_complete = 1; }

        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Done.");
        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Done.  No operation in progress.");

    return tag->status;
}


/* Get_Attributes_All on the Identity object, instance 1. */
int identity_tag_build_read_request(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req = NULL;
    static uint8_t get_identity[] = {
        0x01,       /* service, Get_Attributes_All */
        0x02,       /* path size in 16-bit words */
        0x20, 0x01, /* class segment, Identity */
        0x24, 0x01  /* instance segment, instance 1 */
    };

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Starting.");

    rc = session_create_request(tag->session, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  rc=%s",
               plc_tag_decode_error(rc));
        return rc;
    }

    mem_copy(req->data, get_identity, (int)sizeof(get_identity));
    req->request_size = (int)sizeof(get_identity);
    req->connected = (tag->use_connected_msg != 0);
    req->allow_packing = 0; /* identity requests should not be packed */

    /* save the request for later */
    critical_block(&tag->api_mutex) { tag->req = req; }

    rc = session_add_request(tag->session, req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to add request to session! rc=%s",
               plc_tag_decode_error(rc));
        ab_tag_abort_request(tag);
        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}


/* The tag's data is the Identity attributes, after the CIP reply header and its status words. */
int identity_tag_check_read_status(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    uint8_t *reply = tag->req->data;
    uint8_t *data_end = tag->req->data + tag->req->request_size;
    uint8_t *data = NULL;
    int data_size = 0;
    uint8_t *tag_data_buffer = NULL;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Starting.");

    do {
        /* a successful Unconnected Send may come back wrapped; step into the embedded reply. */
        if(reply[0] == (AB_EIP_CMD_UNCONNECTED_SEND | AB_EIP_CMD_CIP_OK) && reply[2] == AB_CIP_STATUS_OK) {
            reply += sizeof(cip_header);

            if((data_end - reply) < (ptrdiff_t)sizeof(cip_header)) {
                rc = PLCTAG_ERR_TOO_SMALL;
                break;
            }
        }

        if(reply[0] != (0x01 | AB_EIP_CMD_CIP_OK)) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unexpected CIP reply service 0x%02x!",
                   (unsigned int)reply[0]);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(reply[2] != AB_CIP_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "CIP status is not OK: 0x%02x",
                   (unsigned int)reply[2]);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        data = reply + sizeof(cip_header) + ((size_t)reply[3] * 2);

        if((intptr_t)data > (intptr_t)data_end) {
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        data_size = (int)(data_end - data);

        tag_data_buffer = mem_realloc(tag->data, data_size);
        if(!tag_data_buffer) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unable to reallocate tag data buffer!");
            rc = PLCTAG_ERR_NO_MEM;
            break;
        }

        tag->data = tag_data_buffer;
        tag->size = data_size;
        mem_copy(tag->data, data, data_size);
    } while(0);

    /* clean up the request */
    ab_tag_abort_request(tag);

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}


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
        tag->elem_type = AB_TYPE_TAG_ENTRY;
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

int listing_tag_read_start(plc_tag_p tag_arg) {
    ab_tag_p tag = (ab_tag_p)tag_arg;
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

    /* a new read lists from the first tag: elem_count is the entries in this listing alone. */
    tag->elem_count = 0;
    tag->size = 0;
    tag->offset = 0;
    tag->next_id = 0;

    /* build the new request */
    rc = listing_tag_build_read_request(tag);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unable to build read request!");

        tag->read_in_progress = 0;

        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_PENDING;
}


int listing_tag_tickler(plc_tag_p tag_arg) {
    ab_tag_p tag = (ab_tag_p)tag_arg;
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

        rc = listing_tag_check_read_status(tag);

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
 * listing_tag_check_read_status
 *
 * This routine checks for any outstanding tag list requests.  It will
 * terminate when there is no data in the response and the error is not "more data".
 *
 * This is not thread-safe!  It should be called with the tag mutex
 * locked!
 */

int listing_tag_check_read_status(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    uint8_t *data;
    uint8_t *data_end;
    bool partial_data = false;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Starting.");

    /* if we got here then we have a request and it was processed correctly. */

    /* point to the start of the data */
    data = (tag->req->data) + sizeof(cip_header);

    /* point the end of the data */
    data_end = tag->req->data + tag->req->request_size;

    /* check the status */
    do {
        ptrdiff_t payload_size = (data_end - data);

        rc = cip_check_reply(tag->tag_id, tag->req->data, data_end, AB_EIP_CMD_CIP_LIST_TAGS, &partial_data);
        if(rc != PLCTAG_STATUS_OK) { break; }

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
                   "calling listing_tag_build_read_request() to get the next chunk.");
            rc = listing_tag_build_read_request(tag);
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


int listing_tag_build_read_request(ab_tag_p tag) {
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

    /* point to the end of the struct */
    data_start = data = req->data;

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

    /* Check if the payload size exceeds available space before setting request_size */
    int packet_payload_size = (int)(data - data_start);
    int available_payload = session_get_available_cip_payload_space(tag->session);

    if(packet_payload_size > available_payload) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
               "Request payload (%d bytes) exceeds available space (%d bytes)!", packet_payload_size, available_payload);
        ab_tag_abort_request(tag);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* set the size of the request */
    req->request_size = (int)(data - data_start);
    req->connected = (tag->use_connected_msg != 0);

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


/******************************************************************
 ******************* UDT listing functions ************************
 ******************************************************************/


/*
 * Handle UDT tag set up.
 */

int setup_udt_tag(ab_tag_p tag, const char *name) {
    int rc = PLCTAG_STATUS_OK;
    const char *tag_id_str = name + str_length("@udt/");
    int tag_id = 0;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Starting.");

    /* decode the UDT ID */
    rc = str_to_int(tag_id_str, &tag_id);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Badly formatted or missing UDT id in UDT string %s!",
               name);
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(tag_id < 0 || tag_id > 4095) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "UDT ID must be between 0 and 4095 but was %d!", tag_id);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    /*fill in the blanks. */
    tag->udt_id = (uint16_t)(unsigned int)tag_id;
    tag->special_tag = 1;
    tag->elem_type = AB_TYPE_TAG_UDT;
    tag->elem_count = 1;
    tag->elem_size = 1;

    tag->byte_order = &udt_tag_logix_byte_order;

    tag->vtable = &udt_tag_vtable;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done. Found UDT tag name %s.", name);

    return rc;
}


/*
 * udt_tag_read_start
 *
 * This function must be called only from within one thread, or while
 * the tag's mutex is locked.
 *
 * The function starts the process of getting UDT data from the PLC.
 */

int udt_tag_read_start(plc_tag_p tag_arg) {
    ab_tag_p tag = (ab_tag_p)tag_arg;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Starting");

    if(tag->write_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "A write is in progress on a UDT tag!");
        return PLCTAG_ERR_BAD_STATUS;
    }

    if(tag->read_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    /* mark the tag read in progress */
    tag->read_in_progress = 1;

    /* set up the state for the requests (there are two!) */
    tag->udt_get_fields = 0;
    tag->offset = 0;

    /* build the new request */
    rc = udt_tag_build_read_metadata_request(tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unable to build read request!");

        tag->read_in_progress = 0;

        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_PENDING;
}


int udt_tag_tickler(plc_tag_p tag_arg) {
    ab_tag_p tag = (ab_tag_p)tag_arg;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Starting.");

    rc = check_request_status(tag);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(tag->write_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
               "Something started a write on a UDT tag.   This is not supported!");

        ab_tag_abort_request(tag);

        /* fire the event anyway. */
        tag->write_complete = 1;

        return PLCTAG_ERR_UNSUPPORTED;
    }

    if(tag->read_in_progress) {
        if(tag->udt_get_fields) {
            rc = udt_tag_check_read_fields_status(tag);
        } else {
            rc = udt_tag_check_read_metadata_status(tag);
        }

        tag->status = (int8_t)rc;

        /* if the operation completed, make a note so that the callback will be called. */
        if(!tag->read_in_progress) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Read complete.");
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
 * udt_tag_check_read_metadata_status
 *
 * This routine checks for any outstanding tag udt requests.  It will
 * terminate when there is no data in the response and the error is not "more data".
 *
 * This is not thread-safe!  It should be called with the tag mutex
 * locked!
 */

int udt_tag_check_read_metadata_status(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    cip_header *cip_resp;
    uint8_t *data;
    uint8_t *data_end;
    bool partial_data = false;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Starting.");

    /* if we got here then we have a response and it is valid. */

    /* point to the data */
    cip_resp = (cip_header *)(tag->req->data);

    /* point to the start of the data */
    data = (tag->req->data) + sizeof(cip_header);

    /* point the end of the data */
    data_end = tag->req->data + tag->req->request_size;

    /* check the status */
    do {
        ptrdiff_t payload_size = (data_end - data);

        rc = cip_check_reply(tag->tag_id, tag->req->data, data_end, AB_EIP_CMD_CIP_GET_ATTR_LIST, &partial_data);
        if(rc != PLCTAG_STATUS_OK) { break; }

        /*
         * check to see if there is any data to process.  If this is a packed
         * response, there might not be.
         */

        /* FIXME - it is not clear, but since we do not update tag->offset, a partial result
           will trigger a full retry.
       */
        if(payload_size > 0 && !partial_data) {
            /* we got the metadata, so the transfer made progress. */
            uint8_t *new_buffer = NULL;
            int new_size = 14; /* MAGIC, size of the header below */
            uint32_le tmp_u32;
            uint16_le tmp_u16;
            uint8_t *payload = (uint8_t *)(cip_resp + 1);
            int min_payload_size = UDT_METADATA_HANDLE_OFFSET + (int)sizeof(uint16_le);

            /*
             * We are going to build a 14-byte fake header in the buffer:
             *
             * Bytes   Meaning
             * 0-1     16-bit UDT ID
             * 2-5     32-bit UDT member description size, in 32-bit words.
             * 6-9     32-bit UDT instance size, in bytes.
             * 10-11   16-bit UDT number of members (fields).
             * 12-13   16-bit UDT handle/type.
             */

            if(payload_size < min_payload_size) {
                pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
                       "UDT metadata response is too short, got %d bytes but need at least %d!", (int)payload_size,
                       min_payload_size);
                rc = PLCTAG_ERR_TOO_SMALL;
                break;
            }

            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Increasing tag buffer size to %d bytes.",
                   new_size); /* MAGIC */

            new_buffer = (uint8_t *)mem_realloc(tag->data, new_size);
            if(!new_buffer) {
                pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unable to reallocate tag data memory!");
                rc = PLCTAG_ERR_NO_MEM;
                break;
            }

            tag->data = new_buffer;
            tag->size = new_size;
            tag->elem_count = 1;
            tag->elem_size = new_size;

            /* fill in the data. */

            /* put in the UDT ID */
            tmp_u16 = h2le16(tag->udt_id);
            mem_copy(tag->data + 0, &tmp_u16, (int)(unsigned int)(sizeof(tmp_u16)));

            /* copy in the UDT member description size in 32-bit words */
            mem_copy(tag->data + 2, payload + 6, (int)(unsigned int)(sizeof(tmp_u32)));

            /* copy in the UDT instance size in bytes */
            mem_copy(tag->data + 6, payload + 14, (int)(unsigned int)(sizeof(tmp_u32)));

            /* copy in the UDT number of members */
            mem_copy(tag->data + 10, payload + 22, (int)(unsigned int)(sizeof(tmp_u16)));

            /* copy in the UDT number of members */
            mem_copy(tag->data + 12, payload + UDT_METADATA_HANDLE_OFFSET, (int)(unsigned int)(sizeof(tmp_u16)));

            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "current size %d", tag->size);
            pdebug_dump_bytes(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, tag->data, tag->size);
        } else if(partial_data) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Partial response, no data to process yet.");

            /* tag->offset is not advanced here, so a partial result retries the whole read. */
            tag->fragment_retry_count++;
        } else {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "UDT metadata response contained no data!");
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        /* set the return code */
        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* the old request is done */
    ab_tag_abort_request(tag);

    /* are we actually done? */
    if(rc == PLCTAG_STATUS_OK) {
        /* keep going if we are not done yet, unless we are getting nowhere. */
        if(partial_data && tag->fragment_retry_count > MAX_FRAGMENT_RETRIES) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
                   "Got %d partial responses in a row with no data.  The transfer is not making progress, giving up.",
                   tag->fragment_retry_count);
            rc = PLCTAG_ERR_PARTIAL;
        } else if(partial_data) {
            /* call read start again to try again.  The data returned might be zero bytes if this is a packed result */
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id,
                   "calling udt_tag_build_read_metadata_request() to try again.");
            rc = udt_tag_build_read_metadata_request(tag);
        } else {
            /* done!  Start the next transfer with a clean progress counter. */
            tag->fragment_retry_count = 0;

            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Done reading udt metadata!");

            tag->elem_count = 1;
            tag->offset = 0;
            tag->udt_get_fields = 1;

            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id,
                   "calling udt_tag_build_read_fields_request() to get field data.");
            rc = udt_tag_build_read_fields_request(tag);

            /* an OK from the builder means that we need to return PENDING because we just queued the new request*/
            if(rc == PLCTAG_STATUS_OK) { rc = PLCTAG_STATUS_PENDING; }
        }
    }

    /* this is not an else clause because the above if could result in bad rc. */
    if(rc_is_error(rc)) {
        /* error ! */
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Error received: %s!", plc_tag_decode_error(rc));

        tag->offset = 0;
        tag->udt_get_fields = 0;

        /* clean up everything in case we managed to create a request before failing. */
        ab_tag_abort_request(tag);
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}


int udt_tag_build_read_metadata_request(ab_tag_p tag) {
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

    /* point to the end of the struct */
    data_start = data = req->data;

    /*
     * set up the embedded CIP UDT metadata request packet
        uint8_t request_service;    AB_EIP_CMD_CIP_GET_ATTR_LIST=0x03
        uint8_t request_path_size;  3 word = 6 bytes
        uint8_t request_path[6];        0x20    get class
                                        0x6C    UDT class
                                        0x25    get instance (16-bit)
                                        0x00    padding
                                        0x00    instance byte 0
                                        0x00    instance byte 1
        uint16_le instance_id;      NOTE! this is the last two bytes above for convenience!
        uint16_le num_attributes;   0x04    number of attributes to get
        uint16_le requested_attributes[4];      0x04    attribute #4 - Number of 32-bit words in the template definition.
                                                0x05    attribute #5 - Number of bytes in the structure on the wire.
                                                0x02    attribute #2 - Number of structure members.
                                                0x01    attribute #1 - Handle/type of structure.
    */

    *data = AB_EIP_CMD_CIP_GET_ATTR_LIST;
    data++;

    /* request path size, in 16-bit words */
    *data = (uint8_t)(3); /* size in words of routing header + routing and instance ID. */
    data++;

    /* add in the routing header . */

    /* first the fixed part. */
    data[0] = 0x20; /* class type */
    data[1] = 0x6C; /* UDT class */
    data[2] = 0x25; /* 16-bit instance ID type */
    data[3] = 0x00; /* padding */
    data += 4;

    /* now the instance ID */
    tmp_u16 = h2le16((uint16_t)tag->udt_id);
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* set up the request itself.  We are asking for a number of attributes. */

    /* set up the request attributes, first the number of attributes. */
    tmp_u16 = h2le16((uint16_t)4); /* MAGIC, we have four attributes we want. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* first attribute: symbol type */
    tmp_u16 = h2le16((uint16_t)0x04); /* MAGIC, Total field definition size in 32-bit words. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* second attribute: base type size in bytes */
    tmp_u16 = h2le16((uint16_t)0x05); /* MAGIC, struct size in bytes. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* third attribute: tag array dimensions */
    tmp_u16 = h2le16((uint16_t)0x02); /* MAGIC, number of structure members. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* fourth attribute: symbol/tag name */
    tmp_u16 = h2le16((uint16_t)0x01); /* MAGIC, struct type/handle. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* Check if the payload size exceeds available space before setting request_size */
    int packet_payload_size = (int)(data - data_start);
    int available_payload = session_get_available_cip_payload_space(tag->session);

    if(packet_payload_size > available_payload) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
               "Request payload (%d bytes) exceeds available space (%d bytes)!", packet_payload_size, available_payload);
        ab_tag_abort_request(tag);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* set the size of the request */
    req->request_size = (int)(data - data_start);
    req->connected = (tag->use_connected_msg != 0);

    req->allow_packing = tag->allow_packing;

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to add request to session! rc=%d", rc);
        ab_tag_abort_request(tag);
        return rc;
    }

    tag->read_in_progress = 1;

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done");

    return PLCTAG_STATUS_OK;
}


/*
 * udt_tag_check_read_fields_status
 *
 * This routine checks for any outstanding tag udt field data requests.  It will
 * terminate when there is no data in the response and the error is not "more data".
 *
 * This is not thread-safe!  It should be called with the tag mutex
 * locked!
 */

int udt_tag_check_read_fields_status(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    uint8_t *data;
    uint8_t *data_end;
    bool partial_data = false;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Starting.");

    /* the request is there and valid */

    /* point to the start of the data */
    data = (tag->req->data) + sizeof(cip_header);

    /* point the end of the data */
    data_end = tag->req->data + tag->req->request_size;

    /* check the status */
    do {
        ptrdiff_t payload_size = (data_end - data);

        rc = cip_check_reply(tag->tag_id, tag->req->data, data_end, AB_EIP_CMD_CIP_READ, &partial_data);
        if(rc != PLCTAG_STATUS_OK) { break; }

        /*
         * check to see if there is any data to process.  If this is a packed
         * response, there might not be.
         */
        if(payload_size > 0) {
            /* we got data, so the transfer is moving again. */
            tag->fragment_retry_count = 0;

            uint8_t *new_buffer = NULL;
            int new_size = (int)(tag->size) + (int)payload_size;

            /* a PLC can keep returning fragments forever.  Do not grow without bound. */
            if(((ptrdiff_t)tag->size + payload_size) > (ptrdiff_t)AB_MAX_TAG_DATA_SIZE) {
                pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
                       "UDT field data size of %d bytes is larger than the maximum of %d bytes!",
                       (int)((ptrdiff_t)tag->size + payload_size), AB_MAX_TAG_DATA_SIZE);
                rc = PLCTAG_ERR_TOO_LARGE;
                break;
            }

            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Increasing tag buffer size to %d bytes.",
                   new_size);

            new_buffer = (uint8_t *)mem_realloc(tag->data, new_size);
            if(!new_buffer) {
                pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unable to reallocate tag data memory!");
                rc = PLCTAG_ERR_NO_MEM;
                break;
            }

            /* copy the data into the tag's data buffer. */
            mem_copy(new_buffer + tag->offset + 14, data, (int)payload_size); /* MAGIC, offset plus the header. */

            tag->data = new_buffer;
            tag->size = new_size;
            tag->elem_size = new_size;

            tag->offset += (int)payload_size;

            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id,
                   "payload of %d (%x) bytes resulting in current offset %d", (int)payload_size, (int)payload_size, tag->offset);
        } else {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Response returned no data and no error.");

            /* no payload means no forward progress on a fragmented transfer. */
            tag->fragment_retry_count++;
        }
    } while(0);

    /* get rid of the old request. we are done with it. */
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
            /* call read start again to try again.  The data returned might be zero bytes if this is a packed result */
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id,
                   "calling udt_tag_build_read_metadata_request() to try again.");
            rc = udt_tag_build_read_fields_request(tag);

            /* if we get OK, we need to return PENDING for the new request. */
            if(rc == PLCTAG_STATUS_OK) { rc = PLCTAG_STATUS_PENDING; }
        } else {
            /* done!  Start the next transfer with a clean progress counter. */
            tag->fragment_retry_count = 0;

            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id,
                   "Done reading UDT field data.  Tag buffer contains:");
            pdebug_dump_bytes(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, tag->data, tag->size);

            tag->elem_count = 1;

            /* this read is done. */
            tag->udt_get_fields = 0;
            tag->read_in_progress = 0;
            tag->offset = 0;
        }
    }

    /* this is not an else clause because the above if could result in bad rc. */
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        /* error ! */
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Error received: %s!", plc_tag_decode_error(rc));

        tag->offset = 0;
        tag->udt_get_fields = 0;

        /* clean up everything. */
        ab_tag_abort_request(tag);
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}


/*
 * The field definitions are (4 * words) - 23 bytes, from the docs, rounded up to 4 bytes.  The
 * request carries the size in 16 bits.  0 (refused) under 6 words or over 16 bits.
 */
static uint32_t udt_fields_size(uint32_t words) {
    uint64_t bytes = 0;

    if(words < 6) { return 0; }

    bytes = (((uint64_t)words * 4) - 23 + 3) & ~(uint64_t)3;

    return (bytes > UINT16_MAX) ? 0 : (uint32_t)bytes;
}


int udt_tag_build_read_fields_request(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    uint8_t *data_start = NULL;
    uint8_t *data = NULL;
    uint16_le tmp_u16 = UINT16_LE_INIT(0);
    uint32_le tmp_u32 = UINT32_LE_INIT(0);
    uint32_t total_size = 0;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Starting.");

    /* the size comes from the PLC's metadata reply. */
    if(tag->size < 6) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "UDT metadata of %d bytes is too short!", tag->size);
        return PLCTAG_ERR_BAD_REPLY;
    }

    mem_copy(&tmp_u32, tag->data + 2, (int)(unsigned int)(sizeof(tmp_u32)));
    total_size = udt_fields_size(le2h32(tmp_u32));
    if(!total_size) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "UDT definition size of %u words is not valid!",
               (unsigned int)le2h32(tmp_u32));
        return PLCTAG_ERR_BAD_REPLY;
    }

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &tag->req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  rc=%d", rc);
        ab_tag_abort_request(tag);
        return rc;
    }

    /* point the request struct at the buffer */

    /* point to the end of the struct */
    data_start = data = tag->req->data;

    /*
     * set up the embedded CIP UDT metadata request packet
        uint8_t request_service;        AB_EIP_CMD_CIP_READ=0x4C
        uint8_t request_path_size;      3 word = 6 bytes
        uint8_t request_path[6];        0x20    get class
                                        0x6C    UDT class
                                        0x25    get instance (16-bit)
                                        0x00    padding
                                        0x00    instance byte 0
                                        0x00    instance byte 1
        uint32_t offset;                Byte offset in ongoing requests.
        uint16_t total_size;            Total size of request in bytes.
    */

    *data = AB_EIP_CMD_CIP_READ;
    data++;

    /* request path size, in 16-bit words */
    *data = (uint8_t)(3); /* size in words of routing header + routing and instance ID. */
    data++;

    /* add in the routing header . */

    /* first the fixed part. */
    data[0] = 0x20; /* class type */
    data[1] = 0x6C; /* UDT class */
    data[2] = 0x25; /* 16-bit instance ID type */
    data[3] = 0x00; /* padding */
    data += 4;

    /* now the instance ID */
    tmp_u16 = h2le16((uint16_t)tag->udt_id);
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* set the offset */
    tmp_u32 = h2le32((uint32_t)(tag->offset));
    mem_copy(data, &tmp_u32, (int)(unsigned int)sizeof(tmp_u32));
    data += sizeof(tmp_u32);

    /* set the total size */
    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id,
           "Total size %d less offset %d gives %d bytes for the request.", total_size, tag->offset,
           ((int)(unsigned int)total_size - tag->offset));
    tmp_u16 = h2le16((uint16_t)(total_size - (uint16_t)(unsigned int)tag->offset));
    mem_copy(data, &tmp_u16, (int)(unsigned int)sizeof(tmp_u16));
    data += sizeof(tmp_u16);

    /* Check if the payload size exceeds available space before setting request_size */
    int packet_payload_size = (int)(data - data_start);
    int available_payload = session_get_available_cip_payload_space(tag->session);

    if(packet_payload_size > available_payload) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
               "Request payload (%d bytes) exceeds available space (%d bytes)!", packet_payload_size, available_payload);
        ab_tag_abort_request(tag);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* set the size of the request */
    tag->req->request_size = (int)(data - data_start);
    tag->req->connected = (tag->use_connected_msg != 0);

    tag->req->allow_packing = tag->allow_packing;

    tag->read_in_progress = 1;

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, tag->req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to add request to session! rc=%d", rc);
        ab_tag_abort_request(tag);
        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done");

    return PLCTAG_STATUS_OK;
}
