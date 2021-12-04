/***************************************************************************
 *   Copyright (C) 2021 by Kyle Hayes                                      *
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
#include <platform.h>
#include <lib/libplctag.h>
#include <lib/tag.h>
#include <ab/defs.h>
#include <ab/ab_common.h>
#include <ab/cip.h>
#include <ab/tag.h>
#include <ab/session.h>
#include <ab/eip_cip.h>  /* for the Logix decode types. */
#include <ab/eip_cip_special.h>
#include <ab/error_codes.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/vector.h>


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
//START_PACK typedef struct {
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
        uint32_le instance_id;  /* monotonically increasing but not contiguous */
        uint16_le symbol_type;   /* type of the symbol. */
        uint16_le element_length; /* length of one array element in bytes. */
        uint32_le array_dims[3];  /* array dimensions. */
        uint16_le string_len;   /* string length count. */
        //uint8_t string_name[82]; /* MAGIC string name bytes (string_len of them, zero padded) */
} END_PACK tag_list_entry;


/* generic functions */


/* raw tag functions */
//static int raw_tag_read_start(ab_tag_p tag);
static int raw_tag_tickler(ab_tag_p tag);
static int raw_tag_write_start(ab_tag_p tag);
static int raw_tag_check_write_status_connected(ab_tag_p tag);
static int raw_tag_check_write_status_unconnected(ab_tag_p tag);
static int raw_tag_build_write_request_connected(ab_tag_p tag);
static int raw_tag_build_write_request_unconnected(ab_tag_p tag);



/* listing tag functions. */
static int listing_tag_read_start(ab_tag_p tag);
static int listing_tag_tickler(ab_tag_p tag);
//static int listing_tag_write_start(ab_tag_p tag);
static int listing_tag_check_read_status_connected(ab_tag_p tag);
static int listing_tag_build_read_request_connected(ab_tag_p tag);



/* UDT tag functions. */
static int udt_tag_read_start(ab_tag_p tag);
static int udt_tag_tickler(ab_tag_p tag);
//static int listing_tag_write_start(ab_tag_p tag);
static int udt_tag_check_read_metadata_status_connected(ab_tag_p tag);
static int udt_tag_build_read_metadata_request_connected(ab_tag_p tag);
static int udt_tag_check_read_fields_status_connected(ab_tag_p tag);
static int udt_tag_build_read_fields_request_connected(ab_tag_p tag);


/* define the vtable for raw tag type. */
struct tag_vtable_t raw_tag_vtable = {
    (tag_vtable_func)ab_tag_abort, /* shared */
    (tag_vtable_func)NULL, /* read */
    (tag_vtable_func)ab_tag_status, /* shared */
    (tag_vtable_func)raw_tag_tickler,
    (tag_vtable_func)raw_tag_write_start,
    (tag_vtable_func)NULL, /* wake_plc */

    /* attribute accessors */
    ab_get_int_attrib,
    ab_set_int_attrib
};

/* define the vtable for listing tag type. */
struct tag_vtable_t listing_tag_vtable = {
    (tag_vtable_func)ab_tag_abort, /* shared */
    (tag_vtable_func)listing_tag_read_start,
    (tag_vtable_func)ab_tag_status, /* shared */
    (tag_vtable_func)listing_tag_tickler,
    (tag_vtable_func)NULL, /* write */
    (tag_vtable_func)NULL, /* wake_plc */

    /* attribute accessors */
    ab_get_int_attrib,
    ab_set_int_attrib
};


/* define the vtable for udt tag type. */
struct tag_vtable_t udt_tag_vtable = {
    (tag_vtable_func)ab_tag_abort, /* shared */
    (tag_vtable_func)udt_tag_read_start,
    (tag_vtable_func)ab_tag_status, /* shared */
    (tag_vtable_func)udt_tag_tickler,
    (tag_vtable_func)NULL, /* write */
    (tag_vtable_func)NULL, /* wake_plc */

    /* attribute accessors */
    ab_get_int_attrib,
    ab_set_int_attrib
};



tag_byte_order_t listing_tag_logix_byte_order = {
    .is_allocated = 0,

    .int16_order = {0,1},
    .int32_order = {0,1,2,3},
    .int64_order = {0,1,2,3,4,5,6,7},
    .float32_order = {0,1,2,3},
    .float64_order = {0,1,2,3,4,5,6,7},

    .str_is_defined = 1,
    .str_is_counted = 1,
    .str_is_fixed_length = 0,
    .str_is_zero_terminated = 0,
    .str_is_byte_swapped = 0,

    .str_count_word_bytes = 2,
    .str_max_capacity = 0,
    .str_total_length = 0,
    .str_pad_bytes = 0
};

/* strings are zero terminated. */
tag_byte_order_t udt_tag_logix_byte_order = {
    .is_allocated = 0,

    .int16_order = {0,1},
    .int32_order = {0,1,2,3},
    .int64_order = {0,1,2,3,4,5,6,7},
    .float32_order = {0,1,2,3},
    .float64_order = {0,1,2,3,4,5,6,7},

    .str_is_defined = 1,
    .str_is_counted = 0,
    .str_is_fixed_length = 0,
    .str_is_zero_terminated = 1,
    .str_is_byte_swapped = 0,

    .str_count_word_bytes = 0,
    .str_max_capacity = 0,
    .str_total_length = 0,
    .str_pad_bytes = 0
};



/*************************************************************************
 **************************** API Functions ******************************
 ************************************************************************/


/* Raw tag functions */


int setup_raw_tag(ab_tag_p tag)
{
    pdebug(DEBUG_DETAIL, "Starting.");

    /* set up raw tag. */
    tag->special_tag = 1;
    tag->elem_type = AB_TYPE_TAG_RAW;
    tag->elem_count = 1;
    tag->elem_size = 1;

    tag->byte_order = &logix_tag_byte_order;

    tag->vtable = &raw_tag_vtable;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;

}



int raw_tag_tickler(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW,"Starting.");

    if(tag->read_in_progress) {
        pdebug(DEBUG_WARN, "Something started a read on a raw tag.  This is not supported!");
        tag->read_in_progress = 0;
        tag->read_in_flight = 0;

        return rc;
    }

    if(tag->write_in_progress) {
        if(tag->use_connected_msg) {
            rc = raw_tag_check_write_status_connected(tag);
        } else {
            rc = raw_tag_check_write_status_unconnected(tag);
        }

        tag->status = (int8_t)rc;

        /* if the operation completed, make a note so that the callback will be called. */
        if(!tag->write_in_progress) {
            pdebug(DEBUG_DETAIL, "Write complete.");
            tag->write_complete = 1;
        }

        pdebug(DEBUG_SPEW, "Done.");

        return rc;
    }

    pdebug(DEBUG_SPEW, "Done.  No operation in progress.");

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

int raw_tag_write_start(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    if(tag->read_in_progress) {
        pdebug(DEBUG_WARN, "Raw tag found with a read in flight!");
        return PLCTAG_ERR_BAD_STATUS;
    }

    if(tag->write_in_progress) {
        pdebug(DEBUG_WARN, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    /* the write is now in flight */
    tag->write_in_progress = 1;

    if(tag->use_connected_msg) {
        rc = raw_tag_build_write_request_connected(tag);
    } else {
        rc = raw_tag_build_write_request_unconnected(tag);
    }

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"Unable to build write request!");
        tag->write_in_progress = 0;

        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}





/*
 * raw_tag_check_write_status_connected
 *
 * This routine must be called with the tag mutex locked.  It checks the current
 * status of a write operation.  If the write is done, it triggers the clean up.
 */

static int raw_tag_check_write_status_connected(ab_tag_p tag)
{
    eip_cip_co_resp* cip_resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_ERROR,"Null tag pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if (!tag->req) {
        tag->write_in_progress = 0;
        tag->offset = 0;

        pdebug(DEBUG_WARN,"Write in progress, but no request in flight!");

        return PLCTAG_ERR_WRITE;
    }

    /* request can be used by two threads at once. */
    spin_block(&tag->req->lock) {
        if(!tag->req->resp_received) {
            rc = PLCTAG_STATUS_PENDING;
            break;
        }

        /* check to see if it was an abort on the session side. */
        if(tag->req->status != PLCTAG_STATUS_OK) {
            rc = tag->req->status;
            tag->req->abort_request = 1;

            pdebug(DEBUG_WARN,"Session reported failure of request: %s.", plc_tag_decode_error(rc));

            tag->write_in_progress = 0;
            tag->offset = 0;

            break;
        }
    }

    if(rc != PLCTAG_STATUS_OK) {
        if(rc_is_error(rc)) {
            /* the request is dead, from session side. */
            tag->req = rc_dec(tag->req);
        }

        return rc;
    }

    /* the request is ours exclusively. */

    /* point to the data */
    cip_resp = (eip_cip_co_resp*)(tag->req->data);

    do {
        if (le2h16(cip_resp->encap_command) != AB_EIP_CONNECTED_SEND) {
            pdebug(DEBUG_WARN, "Unexpected EIP packet type received: %d!", cip_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if (le2h32(cip_resp->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "EIP command failed, response code: %d", le2h32(cip_resp->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        /* the client needs to handle the raw CIP response. */

        // if (cip_resp->reply_service != (AB_EIP_CMD_CIP_WRITE_FRAG | AB_EIP_CMD_CIP_OK)
        //     && cip_resp->reply_service != (AB_EIP_CMD_CIP_WRITE | AB_EIP_CMD_CIP_OK)
        //     && cip_resp->reply_service != (AB_EIP_CMD_CIP_RMW | AB_EIP_CMD_CIP_OK)) {
        //     pdebug(DEBUG_WARN, "CIP response reply service unexpected: %d", cip_resp->reply_service);
        //     rc = PLCTAG_ERR_BAD_DATA;
        //     break;
        // }

        // if (cip_resp->status != AB_CIP_STATUS_OK && cip_resp->status != AB_CIP_STATUS_FRAG) {
        //     pdebug(DEBUG_WARN, "CIP read failed with status: 0x%x %s", cip_resp->status, decode_cip_error_short((uint8_t *)&cip_resp->status));
        //     pdebug(DEBUG_INFO, decode_cip_error_long((uint8_t *)&cip_resp->status));
        //     rc = decode_cip_error_code((uint8_t *)&cip_resp->status);
        //     break;
        // }
    } while(0);

    /* write is done in one way or another. */
    tag->write_in_progress = 0;

    if(rc == PLCTAG_STATUS_OK) {
        /* copy the data into the tag. */
        uint8_t *data_start = (uint8_t *)(&cip_resp->reply_service);
        uint8_t *data_end = tag->req->data + (tag->req->request_size);
        int data_size = (int)(unsigned int)(data_end - data_start);
        uint8_t *tag_data_buffer = mem_realloc(tag->data, data_size);

        if(tag_data_buffer) {
            tag->data = tag_data_buffer;
            tag->size = data_size;

            mem_copy(tag->data, data_start, data_size);
        } else {
            pdebug(DEBUG_WARN, "Unable to reallocate tag data buffer!");
            rc = PLCTAG_ERR_NO_MEM;
        }
    } else {
        pdebug(DEBUG_WARN,"Write failed!");

        tag->offset = 0;
    }

    /* clean up the request. */
    tag->req->abort_request = 1;
    tag->req = rc_dec(tag->req);

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}





/*
 * raw_tag_check_write_status_unconnected
 *
 * This routine must be called with the tag mutex locked.  It checks the current
 * status of a write operation.  If the write is done, it triggers the clean up.
 */

static int raw_tag_check_write_status_unconnected(ab_tag_p tag)
{
    eip_cip_uc_resp* cip_resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_ERROR,"Null tag pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if (!tag->req) {
        tag->write_in_progress = 0;
        tag->offset = 0;

        pdebug(DEBUG_WARN,"Write in progress, but no request in flight!");

        return PLCTAG_ERR_WRITE;
    }

    /* request can be used by two threads at once. */
    spin_block(&tag->req->lock) {
        if(!tag->req->resp_received) {
            rc = PLCTAG_STATUS_PENDING;
            break;
        }

        /* check to see if it was an abort on the session side. */
        if(tag->req->status != PLCTAG_STATUS_OK) {
            rc = tag->req->status;
            tag->req->abort_request = 1;

            pdebug(DEBUG_WARN,"Session reported failure of request: %s.", plc_tag_decode_error(rc));

            tag->write_in_progress = 0;
            tag->offset = 0;

            break;
        }
    }

    if(rc != PLCTAG_STATUS_OK) {
        if(rc_is_error(rc)) {
            /* the request is dead, from session side. */
            tag->req = rc_dec(tag->req);
        }

        return rc;
    }

    /* the request is ours exclusively. */

    /* point to the data */
    cip_resp = (eip_cip_uc_resp*)(tag->req->data);

    do {
        if (le2h16(cip_resp->encap_command) != AB_EIP_CONNECTED_SEND) {
            pdebug(DEBUG_WARN, "Unexpected EIP packet type received: %d!", cip_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if (le2h32(cip_resp->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "EIP command failed, response code: %d", le2h32(cip_resp->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        /* the client needs to handle the raw CIP response. */

        // if (cip_resp->reply_service != (AB_EIP_CMD_CIP_WRITE_FRAG | AB_EIP_CMD_CIP_OK)
        //     && cip_resp->reply_service != (AB_EIP_CMD_CIP_WRITE | AB_EIP_CMD_CIP_OK)
        //     && cip_resp->reply_service != (AB_EIP_CMD_CIP_RMW | AB_EIP_CMD_CIP_OK)) {
        //     pdebug(DEBUG_WARN, "CIP response reply service unexpected: %d", cip_resp->reply_service);
        //     rc = PLCTAG_ERR_BAD_DATA;
        //     break;
        // }

        // if (cip_resp->status != AB_CIP_STATUS_OK && cip_resp->status != AB_CIP_STATUS_FRAG) {
        //     pdebug(DEBUG_WARN, "CIP read failed with status: 0x%x %s", cip_resp->status, decode_cip_error_short((uint8_t *)&cip_resp->status));
        //     pdebug(DEBUG_INFO, decode_cip_error_long((uint8_t *)&cip_resp->status));
        //     rc = decode_cip_error_code((uint8_t *)&cip_resp->status);
        //     break;
        // }
    } while(0);

    /* write is done in one way or another. */
    tag->write_in_progress = 0;

    if(rc == PLCTAG_STATUS_OK) {
        /* copy the data into the tag. */
        uint8_t *data_start = (uint8_t *)(&cip_resp->reply_service);
        uint8_t *data_end = data_start + le2h16(cip_resp->cpf_udi_item_length);
        int data_size = (int)(unsigned int)(data_end - data_start);
        uint8_t *tag_data_buffer = mem_realloc(tag->data, data_size);

        if(tag_data_buffer) {
            tag->data = tag_data_buffer;
            tag->size = data_size;

            mem_copy(tag->data, data_start, data_size);
        } else {
            pdebug(DEBUG_WARN, "Unable to reallocate tag data buffer!");
            rc = PLCTAG_ERR_NO_MEM;
        }
    } else {
        pdebug(DEBUG_WARN,"Write failed!");

        tag->offset = 0;
    }

    /* clean up the request. */
    tag->req->abort_request = 1;
    tag->req = rc_dec(tag->req);

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}






int raw_tag_build_write_request_connected(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_req* cip = NULL;
    uint8_t* data = NULL;
    ab_request_p req = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    if(tag->size > session_get_max_payload(tag->session)) {
        pdebug(DEBUG_WARN, "Amount to write exceeds negotiated session size %d!", session_get_max_payload(tag->session));
        return PLCTAG_ERR_TOO_LARGE;
    }

    cip = (eip_cip_co_req*)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_cip_co_req);

    /*
     * set up the embedded CIP request packet.  The user/client needs
     * to set up the entire CIP request.   We just copy it here.
     */

    /* copy the tag data into the request */
    mem_copy(data, tag->data, tag->size);
    data += tag->size;

    /* now we go back and fill in the fields of the static part */

    /* encap fields */
    cip->encap_command = h2le16(AB_EIP_CONNECTED_SEND); /* ALWAYS 0x0070 Unconnected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                 /* ALWAYS 2 */
    cip->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI);/* ALWAYS 0x00A1 connected address item */
    cip->cpf_cai_item_length = h2le16(4);            /* ALWAYS 4, size of connection ID*/
    cip->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI);/* ALWAYS 0x00B1 - connected Data Item */
    cip->cpf_cdi_item_length = h2le16((uint16_t)(data - (uint8_t*)(&cip->cpf_conn_seq_num))); /* REQ: fill in with length of remaining data. */

    /* set the size of the request */
    req->request_size = (int)(data - (req->data));

    /* allow packing if the tag allows it. */
    req->allow_packing = tag->allow_packing;

    /* reset the tag size so that incoming data overwrites the old. */
    tag->size = 0;

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, req);

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to add request to session! rc=%d", rc);
        tag->req = rc_dec(req);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_INFO, "Done");

    return PLCTAG_STATUS_OK;
}




int raw_tag_build_write_request_unconnected(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    eip_cip_uc_req* cip = NULL;
    uint8_t* data = NULL;
    uint8_t *embed_start = NULL;
    uint8_t *embed_end = NULL;
    ab_request_p req = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    cip = (eip_cip_uc_req*)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_cip_uc_req);

    embed_start = data;

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
     * set up the embedded CIP request packet.  The user/client needs
     * to set up the entire CIP request.   We just copy it here.
     */

    /* copy the tag data into the request */
    mem_copy(data, tag->data, tag->size);
    data += tag->size;

    /* now we go back and fill in the fields of the static part */

    /* mark the end of the embedded packet */
    embed_end = data;

    /*
     * after the embedded packet, we need to tell the message router
     * how to get to the target device.
     */

    /* Now copy in the routing information for the embedded message */
    *data = (tag->session->conn_path_size) / 2; /* in 16-bit words */
    data++;
    *data = 0;
    data++;    /* copy the tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* encap fields */
    cip->encap_command = h2le16(AB_EIP_UNCONNECTED_SEND); /* ALWAYS 0x006F Unconnected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    cip->cpf_nai_item_type = h2le16(AB_EIP_ITEM_NAI); /* ALWAYS 0 */
    cip->cpf_nai_item_length = h2le16(0);             /* ALWAYS 0 */
    cip->cpf_udi_item_type = h2le16(AB_EIP_ITEM_UDI); /* ALWAYS 0x00B2 - Unconnected Data Item */
    cip->cpf_udi_item_length = h2le16((uint16_t)(data - (uint8_t*)(&(cip->cm_service_code)))); /* REQ: fill in with length of remaining data. */

    /* CM Service Request - Connection Manager */
    cip->cm_service_code = AB_EIP_CMD_UNCONNECTED_SEND; /* 0x52 Unconnected Send */
    cip->cm_req_path_size = 2;                          /* 2, size in 16-bit words of path, next field */
    cip->cm_req_path[0] = 0x20;                         /* class */
    cip->cm_req_path[1] = 0x06;                         /* Connection Manager */
    cip->cm_req_path[2] = 0x24;                         /* instance */
    cip->cm_req_path[3] = 0x01;                         /* instance 1 */

    /* Unconnected send needs timeout information */
    cip->secs_per_tick = AB_EIP_SECS_PER_TICK; /* seconds per tick */
    cip->timeout_ticks = AB_EIP_TIMEOUT_TICKS; /* timeout = srd_secs_per_tick * src_timeout_ticks */

    /* size of embedded packet */
    cip->uc_cmd_length = h2le16((uint16_t)(embed_end - embed_start));

    /* set the size of the request */
    req->request_size = (int)(data - (req->data));

    /* allow packing if the tag allows it. */
    req->allow_packing = tag->allow_packing;

    /* reset the tag size so that incoming data overwrites the old. */
    tag->size = 0;

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, req);

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to add request to session! rc=%d", rc);
        tag->req = rc_dec(req);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_INFO, "Done");

    return PLCTAG_STATUS_OK;
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

int setup_tag_listing_tag(ab_tag_p tag, const char *name)
{
    int rc = PLCTAG_STATUS_OK;
    char **tag_parts = NULL;

    pdebug(DEBUG_DETAIL, "Starting.");

    do {
        /* is it a bare tag listing? */
        if(str_cmp_i(name, "@tags") == 0) {
            pdebug(DEBUG_DETAIL, "Tag is a bare tag listing tag.");
            break;
        }

        /* is it a program tag listing request? */
        if(str_length(name) >= str_length("PROGRAM:x.@tags")) {
            tag_parts = str_split(name, ".");

            /* check to make sure that we have at least one part. */
            if(!tag_parts) {
                pdebug(DEBUG_WARN, "Tag %s is not a tag listing request.", name);
                rc = PLCTAG_ERR_BAD_PARAM;
                break;
            }

            /* check that we have exactly two parts. */
            if(tag_parts[0] != NULL && tag_parts[1] != NULL && tag_parts[2] == NULL) {
                /* we have exactly two parts. Make sure the last part is "@tags" */
                if(str_cmp_i(tag_parts[1], "@tags") != 0) {
                    pdebug(DEBUG_WARN, "Tag %s is not a tag listing request.", name);
                    rc = PLCTAG_ERR_BAD_PARAM;
                    break;
                }

                if(str_length(tag_parts[0]) <= str_length("PROGRAM:x")) {
                    pdebug(DEBUG_WARN, "Tag %s is not a tag listing request.", name);
                    rc = PLCTAG_ERR_BAD_PARAM;
                    break;
                }

                /* make sure the first part is "PROGRAM:" */
                if(str_cmp_i_n(tag_parts[0], "PROGRAM:", str_length("PROGRAM:"))) {
                    pdebug(DEBUG_WARN, "Tag %s is not a tag listing request.", name);
                    rc = PLCTAG_ERR_NOT_FOUND;
                    break;
                }

                /* we have a program tag request! */
                if(cip_encode_tag_name(tag, tag_parts[0]) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Tag %s program listing is not able to be encoded!", name);
                    rc = PLCTAG_ERR_BAD_PARAM;
                    break;
                }
            } else {
                pdebug(DEBUG_WARN, "Tag %s is not a tag listing request.", name);
                rc = PLCTAG_ERR_NOT_FOUND;
                break;
            }
        } else {
            pdebug(DEBUG_WARN, "Program tag %s listing tag string malformed.");
            rc = PLCTAG_ERR_BAD_PARAM;
            break;
        }
    } while(0);

    /* clean up */
    if(tag_parts) {
        mem_free(tag_parts);
    }

    /* did we find a listing tag? */
    if(rc == PLCTAG_STATUS_OK) {
        /* yes we did */
        tag->special_tag = 1;
        tag->elem_type = AB_TYPE_TAG_ENTRY;
        tag->elem_count = 1;
        tag->elem_size = 1;

        tag->byte_order = &listing_tag_logix_byte_order;

        tag->vtable = &listing_tag_vtable;

        pdebug(DEBUG_INFO, "Done. Found tag listing tag name %s.", name);
    } else {
        pdebug(DEBUG_WARN, "Done. Tag %s is not a well-formed tag listing name, error %s.", name, plc_tag_decode_error(rc));
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

int listing_tag_read_start(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    if(tag->write_in_progress) {
        pdebug(DEBUG_WARN, "A write is in progress on a listing tag!");
        return PLCTAG_ERR_BAD_STATUS;
    }

    if(tag->read_in_progress) {
        pdebug(DEBUG_WARN, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    /* mark the tag read in progress */
    tag->read_in_progress = 1;

    /* build the new request */
    rc = listing_tag_build_read_request_connected(tag);

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"Unable to build read request!");

        tag->read_in_progress = 0;

        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}



int listing_tag_tickler(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW,"Starting.");

    if (tag->read_in_progress) {
        if(tag->elem_type == AB_TYPE_TAG_RAW) {
            pdebug(DEBUG_WARN, "Something started a read on a raw tag.  This is not supported!");
            tag->read_in_progress = 0;
            tag->read_in_flight = 0;
        }

        rc = listing_tag_check_read_status_connected(tag);
        // if (rc != PLCTAG_STATUS_PENDING) {
        //     pdebug(DEBUG_WARN,"Error %s getting tag list read status!", plc_tag_decode_error(rc));
        // }

        tag->status = (int8_t)rc;

        /* if the operation completed, make a note so that the callback will be called. */
        if(!tag->read_in_progress) {
            pdebug(DEBUG_DETAIL, "Read complete.");
            tag->read_complete = 1;
        }

        pdebug(DEBUG_SPEW,"Done.  Read in progress.");

        return rc;
    }

    pdebug(DEBUG_SPEW, "Done.  No operation in progress.");

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

static int listing_tag_check_read_status_connected(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_resp* cip_resp;
    uint8_t* data;
    uint8_t* data_end;
    int partial_data = 0;

    static int symbol_index=0;


    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_ERROR,"Null tag pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if (!tag->req) {
        tag->read_in_progress = 0;
        tag->offset = 0;

        pdebug(DEBUG_WARN,"Read in progress, but no request in flight!");

        return PLCTAG_ERR_READ;
    }

    /* request can be used by two threads at once. */
    spin_block(&tag->req->lock) {
        if(!tag->req->resp_received) {
            rc = PLCTAG_STATUS_PENDING;
            break;
        }

        /* check to see if it was an abort on the session side. */
        if(tag->req->status != PLCTAG_STATUS_OK) {
            rc = tag->req->status;
            tag->req->abort_request = 1;

            pdebug(DEBUG_WARN,"Session reported failure of request: %s.", plc_tag_decode_error(rc));

            tag->read_in_progress = 0;
            tag->offset = 0;
            tag->next_id = 0;
            tag->size = tag->elem_count * tag->elem_size;

            break;
        }
    }

    if(rc != PLCTAG_STATUS_OK) {
        if(rc_is_error(rc)) {
            /* the request is dead, from session side. */
            tag->req = rc_dec(tag->req);
        }

        return rc;
    }

    /* the request is ours exclusively. */

    /* point to the data */
    cip_resp = (eip_cip_co_resp*)(tag->req->data);

    /* point to the start of the data */
    data = (tag->req->data) + sizeof(eip_cip_co_resp);

    /* point the end of the data */
    data_end = (tag->req->data + le2h16(cip_resp->encap_length) + sizeof(eip_encap));

    /* check the status */
    do {
        ptrdiff_t payload_size = (data_end - data);

        if (le2h16(cip_resp->encap_command) != AB_EIP_CONNECTED_SEND) {
            pdebug(DEBUG_WARN, "Unexpected EIP packet type received: %d!", cip_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if (le2h32(cip_resp->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "EIP command failed, response code: %d", le2h32(cip_resp->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if (cip_resp->reply_service != (AB_EIP_CMD_CIP_LIST_TAGS | AB_EIP_CMD_CIP_OK) ) {
            pdebug(DEBUG_WARN, "CIP response reply service unexpected: %d", cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if (cip_resp->status != AB_CIP_STATUS_OK && cip_resp->status != AB_CIP_STATUS_FRAG) {
            pdebug(DEBUG_WARN, "CIP read failed with status: 0x%x %s", cip_resp->status, decode_cip_error_short((uint8_t *)&cip_resp->status));
            pdebug(DEBUG_INFO, decode_cip_error_long((uint8_t *)&cip_resp->status));
            rc = decode_cip_error_code((uint8_t *)&cip_resp->status);
            break;
        }

        /* check to see if this is a partial response. */
        partial_data = (cip_resp->status == AB_CIP_STATUS_FRAG);

        /*
         * check to see if there is any data to process.  If this is a packed
         * response, there might not be.
         */
        if(payload_size > 0) {
            uint8_t *current_entry_data = data;
            int new_size = (int)payload_size + tag->offset;

            /* copy the data into the tag and realloc if we need more space. */

            if(new_size > tag->size) {
                uint8_t *new_buffer = NULL;

                tag->elem_count = tag->size = new_size;

                pdebug(DEBUG_DETAIL, "Increasing tag buffer size to %d bytes.", new_size);

                new_buffer = (uint8_t*)mem_realloc(tag->data, new_size);
                if(!new_buffer) {
                    pdebug(DEBUG_WARN, "Unable to reallocate tag data memory!");
                    rc = PLCTAG_ERR_NO_MEM;
                    break;
                }

                tag->data = new_buffer;
                tag->elem_count = tag->size = new_size;
            }

            /* copy the data into the tag's data buffer. */
            mem_copy(tag->data + tag->offset, data, (int)payload_size);

            tag->offset += (int)payload_size;

            pdebug(DEBUG_DETAIL, "current offset %d", tag->offset);

            /* scan through the data to get the next ID to use. */
            while((data_end - current_entry_data) > 0) {
                tag_list_entry *current_entry = (tag_list_entry*)current_entry_data;

                /* first element is the symbol instance ID */
                tag->next_id = (uint16_t)(le2h32(current_entry->instance_id) + 1);

                pdebug(DEBUG_DETAIL, "Next ID: %d", tag->next_id);

                /* skip past to the next instance. */
                current_entry_data += (sizeof(*current_entry) + le2h16(current_entry->string_len));

                symbol_index++;
            }
        } else {
            pdebug(DEBUG_DETAIL, "Response returned no data and no error.");
        }

        /* set the return code */
        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* clean up the request */
    tag->req->abort_request = 1;
    tag->req = rc_dec(tag->req);

    /* are we actually done? */
    if (rc == PLCTAG_STATUS_OK) {
        /* keep going if we are not done yet. */
        if (partial_data) {
            /* call read start again to get the next piece */
            pdebug(DEBUG_DETAIL, "calling listing_tag_build_read_request_connected() to get the next chunk.");
            rc = listing_tag_build_read_request_connected(tag);
        } else {
            /* done! */
            pdebug(DEBUG_DETAIL, "Done reading tag list data!");

            pdebug(DEBUG_DETAIL, "total symbols: %d", symbol_index);

            tag->elem_count = tag->offset;

            tag->first_read = 0;
            tag->offset = 0;
            tag->next_id = 0;

            /* this read is done. */
            tag->read_in_progress = 0;
        }
    }

    /* this is not an else clause because the above if could result in bad rc. */
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        /* error ! */
        pdebug(DEBUG_WARN, "Error received: %s!", plc_tag_decode_error(rc));

        tag->offset = 0;
        tag->next_id = 0;

        /* clean up everything. */
        ab_tag_abort(tag);
    }

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}




int listing_tag_build_read_request_connected(ab_tag_p tag)
{
    eip_cip_co_req* cip = NULL;
    //tag_list_req *list_req = NULL;
    ab_request_p req = NULL;
    int rc = PLCTAG_STATUS_OK;
    uint8_t *data_start = NULL;
    uint8_t *data = NULL;
    uint16_le tmp_u16 = UINT16_LE_INIT(0);

    pdebug(DEBUG_INFO, "Starting.");

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    /* point the request struct at the buffer */
    cip = (eip_cip_co_req*)(req->data);

    /* point to the end of the struct */
    data_start = data = (uint8_t*)(cip + 1);

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
    *data = (uint8_t)(3 + ((tag->encoded_name_size-1)/2)); /* size in words of routing header + routing and instance ID. */
    data++;

    /* add in the encoded name, but without the leading word count byte! */
    if(tag->encoded_name_size > 1) {
        mem_copy(data, &tag->encoded_name[1], (tag->encoded_name_size-1));
        data += (tag->encoded_name_size-1);
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
    tmp_u16 = h2le16((uint16_t)4);  /* MAGIC, we have four attributes we want. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* first attribute: symbol type */
    tmp_u16 = h2le16((uint16_t)0x02);  /* MAGIC, symbol type. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* second attribute: base type size in bytes */
    tmp_u16 = h2le16((uint16_t)0x07);  /* MAGIC, element size in bytes. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* third attribute: tag array dimensions */
    tmp_u16 = h2le16((uint16_t)0x08);  /* MAGIC, array dimensions. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* fourth attribute: symbol/tag name */
    tmp_u16 = h2le16((uint16_t)0x01);  /* MAGIC, symbol name. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* now we go back and fill in the fields of the static part */

    /* encap fields */
    cip->encap_command = h2le16(AB_EIP_CONNECTED_SEND); /* ALWAYS 0x0070 Connected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                 /* ALWAYS 2 */
    cip->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI);/* ALWAYS 0x00A1 connected address item */
    cip->cpf_cai_item_length = h2le16(4);            /* ALWAYS 4, size of connection ID*/
    cip->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI);/* ALWAYS 0x00B1 - connected Data Item */
    cip->cpf_cdi_item_length = h2le16((uint16_t)((int)(data - data_start) + (int)sizeof(cip->cpf_conn_seq_num)));

    /* set the size of the request */
    req->request_size = (int)((int)sizeof(*cip) + (int)(data - data_start));

    req->allow_packing = tag->allow_packing;

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, req);

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to add request to session! rc=%d", rc);
        tag->req = rc_dec(req);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_INFO, "Done");

    return PLCTAG_STATUS_OK;
}









/******************************************************************
 ******************* UDT listing functions ************************
 ******************************************************************/




/*
 * Handle UDT tag set up.
 */

int setup_udt_tag(ab_tag_p tag, const char *name)
{
    int rc = PLCTAG_STATUS_OK;
    const char *tag_id_str = name + str_length("@udt/");
    int tag_id = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* decode the UDT ID */
    rc = str_to_int(tag_id_str, &tag_id);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Badly formatted or missing UDT id in UDT string %s!", name);
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(tag_id < 0 || tag_id > 4095) {
        pdebug(DEBUG_WARN, "UDT ID must be between 0 and 4095 but was %d!", tag_id);
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

    pdebug(DEBUG_INFO, "Done. Found UDT tag name %s.", name);

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

int udt_tag_read_start(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    if(tag->write_in_progress) {
        pdebug(DEBUG_WARN, "A write is in progress on a UDT tag!");
        return PLCTAG_ERR_BAD_STATUS;
    }

    if(tag->read_in_progress) {
        pdebug(DEBUG_WARN, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    /* mark the tag read in progress */
    tag->read_in_progress = 1;

    /* set up the state for the requests (there are two!) */
    tag->udt_get_fields = 0;
    tag->offset = 0;

    /* build the new request */
    rc = udt_tag_build_read_metadata_request_connected(tag);
    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"Unable to build read request!");

        tag->read_in_progress = 0;

        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}



int udt_tag_tickler(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW,"Starting.");

    if (tag->read_in_progress) {
        if(tag->elem_type == AB_TYPE_TAG_RAW) {
            pdebug(DEBUG_WARN, "Something started a read on a raw tag.  This is not supported!");
            tag->read_in_progress = 0;
            tag->read_in_flight = 0;
        }

        if(tag->udt_get_fields) {
            rc = udt_tag_check_read_fields_status_connected(tag);
        } else {
            rc = udt_tag_check_read_metadata_status_connected(tag);
        }

        tag->status = (int8_t)rc;

        /* if the operation completed, make a note so that the callback will be called. */
        if(!tag->read_in_progress) {
            pdebug(DEBUG_DETAIL, "Read complete.");
            tag->read_complete = 1;
        }

        pdebug(DEBUG_SPEW,"Done.  Read in progress.");

        return rc;
    }

    pdebug(DEBUG_SPEW, "Done.  No operation in progress.");

    return tag->status;
}









/*
 * udt_tag_check_read_metadata_status_connected
 *
 * This routine checks for any outstanding tag udt requests.  It will
 * terminate when there is no data in the response and the error is not "more data".
 *
 * This is not thread-safe!  It should be called with the tag mutex
 * locked!
 */

static int udt_tag_check_read_metadata_status_connected(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_resp* cip_resp;
    uint8_t* data;
    uint8_t* data_end;
    int partial_data = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_ERROR,"Null tag pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if (!tag->req) {
        tag->read_in_progress = 0;
        tag->offset = 0;

        pdebug(DEBUG_WARN,"Read in progress, but no request in flight!");

        return PLCTAG_ERR_READ;
    }

    /* request can be used by two threads at once. */
    spin_block(&tag->req->lock) {
        if(!tag->req->resp_received) {
            rc = PLCTAG_STATUS_PENDING;
            break;
        }

        /* check to see if it was an abort on the session side. */
        if(tag->req->status != PLCTAG_STATUS_OK) {
            rc = tag->req->status;
            tag->req->abort_request = 1;

            pdebug(DEBUG_WARN,"Session reported failure of request: %s.", plc_tag_decode_error(rc));

            tag->read_in_progress = 0;
            tag->offset = 0;
            tag->udt_get_fields = 0;
            tag->size = 0;

            break;
        }
    }

    if(rc != PLCTAG_STATUS_OK) {
        if(rc_is_error(rc)) {
            /* the request is dead, from session side. */
            tag->req = rc_dec(tag->req);
        }

        return rc;
    }

    /* the request is ours exclusively. */

    /* point to the data */
    cip_resp = (eip_cip_co_resp*)(tag->req->data);

    /* point to the start of the data */
    data = (tag->req->data) + sizeof(eip_cip_co_resp);

    /* point the end of the data */
    data_end = (tag->req->data + le2h16(cip_resp->encap_length) + sizeof(eip_encap));

    /* check the status */
    do {
        ptrdiff_t payload_size = (data_end - data);

        if (le2h16(cip_resp->encap_command) != AB_EIP_CONNECTED_SEND) {
            pdebug(DEBUG_WARN, "Unexpected EIP packet type received: %d!", cip_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if (le2h32(cip_resp->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "EIP command failed, response code: %d", le2h32(cip_resp->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if (cip_resp->reply_service != (AB_EIP_CMD_CIP_GET_ATTR_LIST | AB_EIP_CMD_CIP_OK) ) {
            pdebug(DEBUG_WARN, "CIP response reply service unexpected: %d", cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if (cip_resp->status != AB_CIP_STATUS_OK && cip_resp->status != AB_CIP_STATUS_FRAG) {
            pdebug(DEBUG_WARN, "CIP read failed with status: 0x%x %s", cip_resp->status, decode_cip_error_short((uint8_t *)&cip_resp->status));
            pdebug(DEBUG_INFO, decode_cip_error_long((uint8_t *)&cip_resp->status));
            rc = decode_cip_error_code((uint8_t *)&cip_resp->status);
            break;
        }

        /* check to see if this is a partial response. */
        partial_data = (cip_resp->status == AB_CIP_STATUS_FRAG);

        /*
         * check to see if there is any data to process.  If this is a packed
         * response, there might not be.
         */
        if(payload_size > 0 && !partial_data) {
            uint8_t *new_buffer = NULL;
            int new_size = 14; /* MAGIC, size of the header below */
            uint32_le tmp_u32;
            uint16_le tmp_u16;
            uint8_t *payload = (uint8_t *)(cip_resp + 1);

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

            pdebug(DEBUG_DETAIL, "Increasing tag buffer size to %d bytes.", new_size); /* MAGIC */

            new_buffer = (uint8_t*)mem_realloc(tag->data, new_size);
            if(!new_buffer) {
                pdebug(DEBUG_WARN, "Unable to reallocate tag data memory!");
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
            mem_copy(tag->data + 12, payload + 28, (int)(unsigned int)(sizeof(tmp_u16)));

            pdebug(DEBUG_DETAIL, "current size %d", tag->size);
            pdebug_dump_bytes(DEBUG_DETAIL, tag->data, tag->size);
        } else {
            pdebug(DEBUG_DETAIL, "Response returned no data and no error.");
        }

        /* set the return code */
        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* clean up the request */
    tag->req->abort_request = 1;
    tag->req = rc_dec(tag->req);

    /* are we actually done? */
    if (rc == PLCTAG_STATUS_OK) {
        /* keep going if we are not done yet. */
        if (partial_data) {
            /* call read start again to try again.  The data returned might be zero bytes if this is a packed result */
            pdebug(DEBUG_DETAIL, "calling udt_tag_build_read_metadata_request_connected() to try again.");
            rc = udt_tag_build_read_metadata_request_connected(tag);
        } else {
            /* done! */
            pdebug(DEBUG_DETAIL, "Done reading udt metadata!");

            tag->elem_count = 1;
            tag->offset = 0;
            tag->udt_get_fields = 1;

            pdebug(DEBUG_DETAIL, "calling udt_tag_build_read_fields_request_connected() to get field data.");
            rc = udt_tag_build_read_fields_request_connected(tag);
        }
    }

    /* this is not an else clause because the above if could result in bad rc. */
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        /* error ! */
        pdebug(DEBUG_WARN, "Error received: %s!", plc_tag_decode_error(rc));

        tag->offset = 0;
        tag->udt_get_fields = 0;

        /* clean up everything. */
        ab_tag_abort(tag);
    }

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}




int udt_tag_build_read_metadata_request_connected(ab_tag_p tag)
{
    eip_cip_co_req* cip = NULL;
    //tag_list_req *list_req = NULL;
    ab_request_p req = NULL;
    int rc = PLCTAG_STATUS_OK;
    uint8_t *data_start = NULL;
    uint8_t *data = NULL;
    uint16_le tmp_u16 = UINT16_LE_INIT(0);

    pdebug(DEBUG_INFO, "Starting.");

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    /* point the request struct at the buffer */
    cip = (eip_cip_co_req*)(req->data);

    /* point to the end of the struct */
    data_start = data = (uint8_t*)(cip + 1);

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
    tmp_u16 = h2le16((uint16_t)4);  /* MAGIC, we have four attributes we want. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* first attribute: symbol type */
    tmp_u16 = h2le16((uint16_t)0x04);  /* MAGIC, Total field definition size in 32-bit words. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* second attribute: base type size in bytes */
    tmp_u16 = h2le16((uint16_t)0x05);  /* MAGIC, struct size in bytes. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* third attribute: tag array dimensions */
    tmp_u16 = h2le16((uint16_t)0x02);  /* MAGIC, number of structure members. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* fourth attribute: symbol/tag name */
    tmp_u16 = h2le16((uint16_t)0x01);  /* MAGIC, struct type/handle. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* now we go back and fill in the fields of the static part */

    /* encap fields */
    cip->encap_command = h2le16(AB_EIP_CONNECTED_SEND); /* ALWAYS 0x0070 Connected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                 /* ALWAYS 2 */
    cip->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI);/* ALWAYS 0x00A1 connected address item */
    cip->cpf_cai_item_length = h2le16(4);            /* ALWAYS 4, size of connection ID*/
    cip->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI);/* ALWAYS 0x00B1 - connected Data Item */
    cip->cpf_cdi_item_length = h2le16((uint16_t)((int)(data - data_start) + (int)sizeof(cip->cpf_conn_seq_num)));

    /* set the size of the request */
    req->request_size = (int)((int)sizeof(*cip) + (int)(data - data_start));

    req->allow_packing = tag->allow_packing;

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, req);

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to add request to session! rc=%d", rc);
        tag->req = rc_dec(req);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_INFO, "Done");

    return PLCTAG_STATUS_OK;
}








/*
 * udt_tag_check_read_fields_status_connected
 *
 * This routine checks for any outstanding tag udt field data requests.  It will
 * terminate when there is no data in the response and the error is not "more data".
 *
 * This is not thread-safe!  It should be called with the tag mutex
 * locked!
 */

int udt_tag_check_read_fields_status_connected(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_resp* cip_resp;
    uint8_t* data;
    uint8_t* data_end;
    int partial_data = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_ERROR,"Null tag pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if (!tag->req) {
        tag->read_in_progress = 0;
        tag->offset = 0;

        pdebug(DEBUG_WARN,"Read in progress, but no request in flight!");

        return PLCTAG_ERR_READ;
    }

    /* request can be used by two threads at once. */
    spin_block(&tag->req->lock) {
        if(!tag->req->resp_received) {
            rc = PLCTAG_STATUS_PENDING;
            break;
        }

        /* check to see if it was an abort on the session side. */
        if(tag->req->status != PLCTAG_STATUS_OK) {
            rc = tag->req->status;
            tag->req->abort_request = 1;

            pdebug(DEBUG_WARN,"Session reported failure of request: %s.", plc_tag_decode_error(rc));

            tag->read_in_progress = 0;
            tag->offset = 0;
            tag->udt_get_fields = 0;
            tag->size = 0;

            break;
        }
    }

    if(rc != PLCTAG_STATUS_OK) {
        if(rc_is_error(rc)) {
            /* the request is dead, from session side. */
            tag->req = rc_dec(tag->req);
        }

        return rc;
    }

    /* the request is ours exclusively. */

    /* point to the data */
    cip_resp = (eip_cip_co_resp*)(tag->req->data);

    /* point to the start of the data */
    data = (tag->req->data) + sizeof(eip_cip_co_resp);

    /* point the end of the data */
    data_end = (tag->req->data + le2h16(cip_resp->encap_length) + sizeof(eip_encap));

    /* check the status */
    do {
        ptrdiff_t payload_size = (data_end - data);

        if (le2h16(cip_resp->encap_command) != AB_EIP_CONNECTED_SEND) {
            pdebug(DEBUG_WARN, "Unexpected EIP packet type received: %d!", cip_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if (le2h32(cip_resp->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "EIP command failed, response code: %d", le2h32(cip_resp->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if (cip_resp->reply_service != (AB_EIP_CMD_CIP_READ | AB_EIP_CMD_CIP_OK) ) {
            pdebug(DEBUG_WARN, "CIP response reply service unexpected: %d", cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if (cip_resp->status != AB_CIP_STATUS_OK && cip_resp->status != AB_CIP_STATUS_FRAG) {
            pdebug(DEBUG_WARN, "CIP read failed with status: 0x%x %s", cip_resp->status, decode_cip_error_short((uint8_t *)&cip_resp->status));
            pdebug(DEBUG_INFO, decode_cip_error_long((uint8_t *)&cip_resp->status));
            rc = decode_cip_error_code((uint8_t *)&cip_resp->status);
            break;
        }

        /* check to see if this is a partial response. */
        partial_data = (cip_resp->status == AB_CIP_STATUS_FRAG);

        /*
         * check to see if there is any data to process.  If this is a packed
         * response, there might not be.
         */
        if(payload_size > 0) {
            uint8_t *new_buffer = NULL;
            int new_size = (int)(tag->size) + (int)payload_size;

            pdebug(DEBUG_DETAIL, "Increasing tag buffer size to %d bytes.", new_size);

            new_buffer = (uint8_t*)mem_realloc(tag->data, new_size);
            if(!new_buffer) {
                pdebug(DEBUG_WARN, "Unable to reallocate tag data memory!");
                rc = PLCTAG_ERR_NO_MEM;
                break;
            }

            /* copy the data into the tag's data buffer. */
            mem_copy(new_buffer + tag->offset + 14, data, (int)payload_size); /* MAGIC, offset plus the header. */

            tag->data = new_buffer;
            tag->size = new_size;
            tag->elem_size = new_size;

            tag->offset += (int)payload_size;

            pdebug(DEBUG_DETAIL, "payload of %d (%x) bytes resulting in current offset %d", (int)payload_size, (int)payload_size, tag->offset);
        } else {
            pdebug(DEBUG_DETAIL, "Response returned no data and no error.");
        }

        /* set the return code */
        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* clean up the request */
    tag->req->abort_request = 1;
    tag->req = rc_dec(tag->req);

    /* are we actually done? */
    if (rc == PLCTAG_STATUS_OK) {
        /* keep going if we are not done yet. */
        if (partial_data) {
            /* call read start again to try again.  The data returned might be zero bytes if this is a packed result */
            pdebug(DEBUG_DETAIL, "calling udt_tag_build_read_metadata_request_connected() to try again.");
            rc = udt_tag_build_read_fields_request_connected(tag);
        } else {
            /* done! */
            pdebug(DEBUG_DETAIL, "Done reading udt field data.  Tag buffer contains:");
            pdebug_dump_bytes(DEBUG_DETAIL, tag->data, tag->size);

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
        pdebug(DEBUG_WARN, "Error received: %s!", plc_tag_decode_error(rc));

        tag->offset = 0;
        tag->udt_get_fields = 0;

        /* clean up everything. */
        ab_tag_abort(tag);
    }

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}




int udt_tag_build_read_fields_request_connected(ab_tag_p tag)
{
    eip_cip_co_req* cip = NULL;
    //tag_list_req *list_req = NULL;
    ab_request_p req = NULL;
    int rc = PLCTAG_STATUS_OK;
    uint8_t *data_start = NULL;
    uint8_t *data = NULL;
    uint16_le tmp_u16 = UINT16_LE_INIT(0);
    uint32_le tmp_u32 = UINT32_LE_INIT(0);
    uint32_t total_size = 0;
    uint32_t neg_4 = (~(uint32_t)4) + 1; /* twos-complement */

    pdebug(DEBUG_INFO, "Starting.");

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    /* calculate the total size we need to get. */
    mem_copy(&tmp_u32, tag->data + 2, (int)(unsigned int)(sizeof(tmp_u32)));
    total_size = (4 * le2h32(tmp_u32)) - 23; /* formula according to the docs. */

    pdebug(DEBUG_DETAIL, "Calculating total size of request, %d to %d.", (int)(unsigned int)total_size, (int)(unsigned int)((total_size + (uint32_t)3) & (uint32_t)neg_4));

    /* make the total size a multiple of 4 bytes.  Round up. */
    total_size = (total_size + 3) & (uint32_t)neg_4;

    /* point the request struct at the buffer */
    cip = (eip_cip_co_req*)(req->data);

    /* point to the end of the struct */
    data_start = data = (uint8_t*)(cip + 1);

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
    pdebug(DEBUG_DETAIL, "Total size %d less offset %d gives %d bytes for the request.", total_size, tag->offset, ((int)(unsigned int)total_size - tag->offset));
    tmp_u16 = h2le16((uint16_t)(total_size - (uint16_t)(unsigned int)tag->offset));
    mem_copy(data, &tmp_u16, (int)(unsigned int)sizeof(tmp_u16));
    data += sizeof(tmp_u16);

    /* now we go back and fill in the fields of the static part */

    /* encap fields */
    cip->encap_command = h2le16(AB_EIP_CONNECTED_SEND); /* ALWAYS 0x0070 Connected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                 /* ALWAYS 2 */
    cip->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI);/* ALWAYS 0x00A1 connected address item */
    cip->cpf_cai_item_length = h2le16(4);            /* ALWAYS 4, size of connection ID*/
    cip->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI);/* ALWAYS 0x00B1 - connected Data Item */
    cip->cpf_cdi_item_length = h2le16((uint16_t)((int)(data - data_start) + (int)sizeof(cip->cpf_conn_seq_num)));

    /* set the size of the request */
    req->request_size = (int)((int)sizeof(*cip) + (int)(data - data_start));

    req->allow_packing = tag->allow_packing;

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, req);

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to add request to session! rc=%d", rc);
        tag->req = rc_dec(req);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_INFO, "Done");

    return PLCTAG_STATUS_OK;
}


