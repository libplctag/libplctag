/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
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
#include <ab/eip_cip.h>
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



static int build_read_request_connected(ab_tag_p tag, int byte_offset);
//static int build_tag_list_request_connected(ab_tag_p tag);
static int build_read_request_unconnected(ab_tag_p tag, int byte_offset);
static int build_write_request_connected(ab_tag_p tag, int byte_offset);
static int build_write_request_unconnected(ab_tag_p tag, int byte_offset);
static int build_write_bit_request_connected(ab_tag_p tag);
static int build_write_bit_request_unconnected(ab_tag_p tag);
static int check_read_status_connected(ab_tag_p tag);
//static int check_read_tag_list_status_connected(ab_tag_p tag);
static int check_read_status_unconnected(ab_tag_p tag);
static int check_write_status_connected(ab_tag_p tag);
static int check_write_status_unconnected(ab_tag_p tag);
static int calculate_write_data_per_packet(ab_tag_p tag);

static int tag_read_start(ab_tag_p tag);
static int tag_tickler(ab_tag_p tag);
static int tag_write_start(ab_tag_p tag);

/* define the exported vtable for this tag type. */
struct tag_vtable_t eip_cip_vtable = {
    (tag_vtable_func)ab_tag_abort, /* shared */
    (tag_vtable_func)tag_read_start,
    (tag_vtable_func)ab_tag_status, /* shared */
    (tag_vtable_func)tag_tickler,
    (tag_vtable_func)tag_write_start,

    /* attribute accessors */
    ab_get_int_attrib,
    ab_set_int_attrib
};

/* default string types used for ControlLogix-class PLCs. */
tag_byte_order_t logix_tag_byte_order = {
    .is_allocated = 0,

    .int16_order = {0,1},
    .int32_order = {0,1,2,3},
    .int64_order = {0,1,2,3,4,5,6,7},
    .float32_order = {0,1,2,3},
    .float64_order = {0,1,2,3,4,5,6,7},

    .str_is_defined = 1,
    .str_is_counted = 1,
    .str_is_fixed_length = 1,
    .str_is_zero_terminated = 0,
    .str_is_byte_swapped = 0,

    .str_count_word_bytes = 4,
    .str_max_capacity = 82,
    .str_total_length = 88,
    .str_pad_bytes = 2
};

tag_byte_order_t logix_tag_listing_byte_order = {
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



/*************************************************************************
 **************************** API Functions ******************************
 ************************************************************************/


int tag_tickler(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW,"Starting.");

    if (tag->read_in_progress) {
        if(tag->use_connected_msg) {
            // if(tag->tag_list) {
            //     rc = check_read_tag_list_status_connected(tag);
            // } else {
                rc = check_read_status_connected(tag);
            // }
        } else {
            rc = check_read_status_unconnected(tag);
        }

        tag->status = (int8_t)rc;

        /* if the operation completed, make a note so that the callback will be called. */
        if(!tag->read_in_progress) {
            tag->read_complete = 1;
        }

        pdebug(DEBUG_SPEW,"Done.  Read in progress.");

        return rc;
    }

    if (tag->write_in_progress) {
        if(tag->use_connected_msg) {
            rc = check_write_status_connected(tag);
        } else {
            rc = check_write_status_unconnected(tag);
        }

        tag->status = (int8_t)rc;

        /* if the operation completed, make a note so that the callback will be called. */
        if(!tag->write_in_progress) {
            tag->write_complete = 1;
        }

        pdebug(DEBUG_SPEW, "Done. Write in progress.");

        return rc;
    }

    pdebug(DEBUG_SPEW, "Done.  No operation in progress.");

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

int tag_read_start(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    if(tag->read_in_progress || tag->write_in_progress) {
        pdebug(DEBUG_WARN, "Read or write operation already in flight!");
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

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"Unable to build read request!");

        tag->read_in_progress = 0;

        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

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

int tag_write_start(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    /* check to make sure that this kind of tag can be written. */
    // if(tag->special_tag) {
    //     pdebug(DEBUG_WARN, "A tag list cannot be written!");

    //     return PLCTAG_ERR_UNSUPPORTED;
    // }

    if(tag->read_in_progress || tag->write_in_progress) {
        pdebug(DEBUG_WARN, "Read or write operation already in flight!");
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

    if (tag->first_read) {
        pdebug(DEBUG_DETAIL, "No read has completed yet, doing pre-read to get type information.");

        tag->pre_write_read = 1;
        tag->write_in_progress = 0; /* temporarily mask this off */

        return tag_read_start(tag);
    }

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"Unable to calculate write sizes!");
        tag->write_in_progress = 0;

        return rc;
    }

    if(tag->use_connected_msg) {
        rc = build_write_request_connected(tag, tag->offset);
    } else {
        rc = build_write_request_unconnected(tag, tag->offset);
    }

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"Unable to build write request!");
        tag->write_in_progress = 0;

        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}



int build_read_request_connected(ab_tag_p tag, int byte_offset)
{
    eip_cip_co_req* cip = NULL;
    uint8_t* data = NULL;
    ab_request_p req = NULL;
    int rc = PLCTAG_STATUS_OK;
    uint8_t read_cmd = AB_EIP_CMD_CIP_READ_FRAG;

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
    data = (req->data) + sizeof(eip_cip_co_req);

    /*
     * set up the embedded CIP read packet
     * The format is:
     *
     * uint8_t cmd
     * LLA formatted name
     * uint16_t # of elements to read
     */

    //embed_start = data;

    /* set up the CIP Read request */
    if(tag->plc_type == AB_PLC_OMRON_NJNX) {
        read_cmd = AB_EIP_CMD_CIP_READ;
    } else {
        read_cmd = AB_EIP_CMD_CIP_READ_FRAG;
    }

    *data = read_cmd;
    data++;

    /* copy the tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* add the count of elements to read. */
    *((uint16_le*)data) = h2le16((uint16_t)(tag->elem_count));
    data += sizeof(uint16_le);

    if (read_cmd == AB_EIP_CMD_CIP_READ_FRAG) {
        /* add the byte offset for this request */
        *((uint32_le*)data) = h2le32((uint32_t)byte_offset);
        data += sizeof(uint32_le);
    }

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

    /* set the session so that we know what session the request is aiming at */
    //req->session = tag->session;

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


// int build_tag_list_request_connected(ab_tag_p tag)
// {
//     eip_cip_co_req* cip = NULL;
//     //tag_list_req *list_req = NULL;
//     ab_request_p req = NULL;
//     int rc = PLCTAG_STATUS_OK;
//     uint8_t *data_start = NULL;
//     uint8_t *data = NULL;
//     uint16_le tmp_u16 = UINT16_LE_INIT(0);

//     pdebug(DEBUG_INFO, "Starting.");

//     /* get a request buffer */
//     rc = session_create_request(tag->session, tag->tag_id, &req);
//     if (rc != PLCTAG_STATUS_OK) {
//         pdebug(DEBUG_ERROR, "Unable to get new request.  rc=%d", rc);
//         return rc;
//     }

//     /* point the request struct at the buffer */
//     cip = (eip_cip_co_req*)(req->data);

//     /* point to the end of the struct */
//     data_start = data = (uint8_t*)(cip + 1);

//     /*
//      * set up the embedded CIP tag list request packet
//         uint8_t request_service;    AB_EIP_CMD_CIP_LIST_TAGS=0x55
//         uint8_t request_path_size;  3 word = 6 bytes
//         uint8_t request_path[6];        0x20    get class
//                                         0x6B    tag info/symbol class
//                                         0x25    get instance (16-bit)
//                                         0x00    padding
//                                         0x00    instance byte 0
//                                         0x00    instance byte 1
//         uint16_le instance_id;      NOTE! this is the last two bytes above for convenience!
//         uint16_le num_attributes;   0x04    number of attributes to get
//         uint16_le requested_attributes[4];      0x02 attribute #2 - symbol type
//                                                 0x07 attribute #7 - base type size (array element) in bytes
//                                                 0x08    attribute #8 - array dimensions (3xu32)
//                                                 0x01    attribute #1 - symbol name
//     */

//     *data = AB_EIP_CMD_CIP_LIST_TAGS;
//     data++;

//     /* request path size, in 16-bit words */
//     *data = (uint8_t)(3 + ((tag->encoded_name_size-1)/2)); /* size in words of routing header + routing and instance ID. */
//     data++;

//     /* add in the encoded name, but without the leading word count byte! */
//     if(tag->encoded_name_size > 1) {
//         mem_copy(data, &tag->encoded_name[1], (tag->encoded_name_size-1));
//         data += (tag->encoded_name_size-1);
//     }

//     /* add in the routing header . */

//     /* first the fixed part. */
//     data[0] = 0x20; /* class type */
//     data[1] = 0x6B; /* tag info/symbol class */
//     data[2] = 0x25; /* 16-bit instance ID type */
//     data[3] = 0x00; /* padding */
//     data += 4;

//     /* now the instance ID */
//     tmp_u16 = h2le16((uint16_t)tag->next_id);
//     mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
//     data += (int)sizeof(tmp_u16);

//     /* set up the request itself.  We are asking for a number of attributes. */

//     /* set up the request attributes, first the number of attributes. */
//     tmp_u16 = h2le16((uint16_t)4);  /* MAGIC, we have four attributes we want. */
//     mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
//     data += (int)sizeof(tmp_u16);

//     /* first attribute: symbol type */
//     tmp_u16 = h2le16((uint16_t)0x02);  /* MAGIC, symbol type. */
//     mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
//     data += (int)sizeof(tmp_u16);

//     /* second attribute: base type size in bytes */
//     tmp_u16 = h2le16((uint16_t)0x07);  /* MAGIC, element size in bytes. */
//     mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
//     data += (int)sizeof(tmp_u16);

//     /* third attribute: tag array dimensions */
//     tmp_u16 = h2le16((uint16_t)0x08);  /* MAGIC, array dimensions. */
//     mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
//     data += (int)sizeof(tmp_u16);

//     /* fourth attribute: symbol/tag name */
//     tmp_u16 = h2le16((uint16_t)0x01);  /* MAGIC, symbol name. */
//     mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
//     data += (int)sizeof(tmp_u16);

//     /* now we go back and fill in the fields of the static part */

//     /* encap fields */
//     cip->encap_command = h2le16(AB_EIP_CONNECTED_SEND); /* ALWAYS 0x0070 Connected Send*/

//     /* router timeout */
//     cip->router_timeout = h2le16(1); /* one second timeout, enough? */

//     /* Common Packet Format fields for unconnected send. */
//     cip->cpf_item_count = h2le16(2);                 /* ALWAYS 2 */
//     cip->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI);/* ALWAYS 0x00A1 connected address item */
//     cip->cpf_cai_item_length = h2le16(4);            /* ALWAYS 4, size of connection ID*/
//     cip->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI);/* ALWAYS 0x00B1 - connected Data Item */
//     cip->cpf_cdi_item_length = h2le16((uint16_t)((int)(data - data_start) + (int)sizeof(cip->cpf_conn_seq_num)));

//     /* set the size of the request */
//     req->request_size = (int)((int)sizeof(*cip) + (int)(data - data_start));

//     req->allow_packing = tag->allow_packing;

//     /* add the request to the session's list. */
//     rc = session_add_request(tag->session, req);

//     if (rc != PLCTAG_STATUS_OK) {
//         pdebug(DEBUG_ERROR, "Unable to add request to session! rc=%d", rc);
//         tag->req = rc_dec(req);
//         return rc;
//     }

//     /* save the request for later */
//     tag->req = req;

//     pdebug(DEBUG_INFO, "Done");

//     return PLCTAG_STATUS_OK;
// }



int build_read_request_unconnected(ab_tag_p tag, int byte_offset)
{
    eip_cip_uc_req* cip;
    uint8_t* data;
    uint8_t* embed_start, *embed_end;
    ab_request_p req = NULL;
    int rc = PLCTAG_STATUS_OK;
    uint8_t read_cmd = AB_EIP_CMD_CIP_READ_FRAG;

    pdebug(DEBUG_INFO, "Starting.");

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    /* point the request struct at the buffer */
    cip = (eip_cip_uc_req*)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_cip_uc_req);

    /*
     * set up the embedded CIP read packet
     * The format is:
     *
     * uint8_t cmd
     * LLA formatted name
     * uint16_t # of elements to read
     */

    embed_start = data;

    /* set up the CIP Read request */
    if(tag->plc_type == AB_PLC_OMRON_NJNX) {
        read_cmd = AB_EIP_CMD_CIP_READ;
    } else {
        read_cmd = AB_EIP_CMD_CIP_READ_FRAG;
    }

    *data = read_cmd;
    data++;

    /* copy the tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* add the count of elements to read. */
    /* FIXME BUG - this may not work on some processors! */
    *((uint16_le*)data) = h2le16((uint16_t)(tag->elem_count));
    data += sizeof(uint16_le);

    /* add the byte offset for this request */
    if(read_cmd == AB_EIP_CMD_CIP_READ_FRAG) {
        *((uint32_le*)data) = h2le32((uint32_t)byte_offset);
        data += sizeof(uint32_le);
    }

    /* mark the end of the embedded packet */
    embed_end = data;

    /* Now copy in the routing information for the embedded message */
    /*
     * routing information.  Format:
     *
     * uint8_t path_size in 16-bit words
     * uint8_t reserved/pad (zero)
     * uint8_t[...] path (padded to even number of bytes)
     */
    if(tag->session->conn_path_size > 0) {
        *data = (tag->session->conn_path_size) / 2; /* in 16-bit words */
        data++;
        *data = 0; /* reserved/pad */
        data++;
        mem_copy(data, tag->session->conn_path, tag->session->conn_path_size);
        data += tag->session->conn_path_size;
    }

    /* now we go back and fill in the fields of the static part */

    /* encap fields */
    cip->encap_command = h2le16(AB_EIP_UNCONNECTED_SEND); /* ALWAYS 0x0070 Unconnected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    cip->cpf_nai_item_type = h2le16(AB_EIP_ITEM_NAI); /* ALWAYS 0 */
    cip->cpf_nai_item_length = h2le16(0);             /* ALWAYS 0 */
    cip->cpf_udi_item_type = h2le16(AB_EIP_ITEM_UDI); /* ALWAYS 0x00B2 - Unconnected Data Item */
    cip->cpf_udi_item_length = h2le16((uint16_t)(data - (uint8_t*)(&cip->cm_service_code))); /* REQ: fill in with length of remaining data. */

    /* CM Service Request - Connection Manager */
    cip->cm_service_code = AB_EIP_CMD_UNCONNECTED_SEND; /* 0x52 Unconnected Send */
    cip->cm_req_path_size = 2;                          /* 2, size in 16-bit words of path, next field */
    cip->cm_req_path[0] = 0x20;                         /* class */
    cip->cm_req_path[1] = 0x06;                         /* Connection Manager */
    cip->cm_req_path[2] = 0x24;                         /* instance */
    cip->cm_req_path[3] = 0x01;                         /* instance 1 */

    /* Unconnected send needs timeout information */
    cip->secs_per_tick = AB_EIP_SECS_PER_TICK; /* seconds per tick */
    cip->timeout_ticks = AB_EIP_TIMEOUT_TICKS; /* timeout = src_secs_per_tick * src_timeout_ticks */

    /* size of embedded packet */
    cip->uc_cmd_length = h2le16((uint16_t)(embed_end - embed_start));

    /* set the size of the request */
    req->request_size = (int)(data - (req->data));

    /* allow packing if the tag allows it. */
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


int build_write_bit_request_connected(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_req* cip = NULL;
    uint8_t* data = NULL;
    ab_request_p req = NULL;
    int i;

    pdebug(DEBUG_INFO, "Starting.");

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    rc = calculate_write_data_per_packet(tag);
    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to calculate valid write data per packet!.  rc=%s", plc_tag_decode_error(rc));
        return rc;
    }

    if(tag->write_data_per_packet < (tag->size * 2) + 2) {  /* 2 masks plus a count word. */
        pdebug(DEBUG_ERROR,"Insufficient space to write bit masks!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    cip = (eip_cip_co_req*)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_cip_co_req);

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
    *data = (uint8_t)(tag->elem_size & 0xFF); data++;
    *data = (uint8_t)((tag->elem_size >> 8) & 0xFF); data++;

    /* write the OR mask */
    for(i=0; i < tag->elem_size; i++) {
        if((tag->bit/8) == i) {
            uint8_t mask = (uint8_t)(1 << (tag->bit % 8));

            /* if the bit is set, then we want to mask it on. */
            if(tag->data[tag->bit / 8] & mask) {
                *data = mask;
            } else {
                *data = (uint8_t)0;
            }

            pdebug(DEBUG_DETAIL, "adding OR mask byte %d: %x", i, *data);

            data++;
        } else {
            /* this is not the data we care about. */
            *data = (uint8_t)0;

            pdebug(DEBUG_DETAIL, "adding OR mask byte %d: %x", i, *data);

            data++;
        }
    }

    /* write the AND mask */
    for(i=0; i < tag->elem_size; i++) {
        if((tag->bit / 8) == i) {
            uint8_t mask = (uint8_t)(1 << (tag->bit % 8));

            /* if the bit is set, then we want to _not_ mask it off. */
            if(tag->data[tag->bit / 8] & mask) {
                *data = (uint8_t)0xFF;
            } else {
                *data = (uint8_t)(~mask);
            }

            pdebug(DEBUG_DETAIL, "adding OR mask byte %d: %x", i, *data);

            data++;
        } else {
            /* this is not the data we care about. */
            *data = (uint8_t)0xFF;

            pdebug(DEBUG_DETAIL, "adding OR mask byte %d: %x", i, *data);

            data++;
        }
    }

    /* let the rest of the system know that the write is complete after this. */
    tag->offset = tag->size;

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




int build_write_bit_request_unconnected(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    eip_cip_uc_req* cip = NULL;
    uint8_t* data = NULL;
    uint8_t *embed_start = NULL;
    uint8_t *embed_end = NULL;
    ab_request_p req = NULL;
    int i = 0;

    pdebug(DEBUG_INFO, "Starting.");

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    rc = calculate_write_data_per_packet(tag);
    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to calculate valid write data per packet!.  rc=%s", plc_tag_decode_error(rc));
        return rc;
    }

    if(tag->write_data_per_packet < (tag->size * 2) + 2) {  /* 2 masks plus a count word. */
        pdebug(DEBUG_ERROR,"Insufficient space to write bit masks!");
        return PLCTAG_ERR_TOO_SMALL;
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
    *data = (uint8_t)(tag->elem_size & 0xFF); data++;
    *data = (uint8_t)((tag->elem_size >> 8) & 0xFF); data++;

    /* write the OR mask */
    for(i=0; i < tag->elem_size; i++) {
        if((tag->bit/8) == i) {
            uint8_t mask = (uint8_t)(1 << (tag->bit % 8));

            /* if the bit is set, then we want to mask it on. */
            if(tag->data[tag->bit / 8] & mask) {
                *data = mask;
            } else {
                *data = (uint8_t)0;
            }

            pdebug(DEBUG_DETAIL, "adding OR mask byte %d: %x", i, *data);

            data++;
        } else {
            /* this is not the data we care about. */
            *data = (uint8_t)0;

            pdebug(DEBUG_DETAIL, "adding OR mask byte %d: %x", i, *data);

            data++;
        }
    }

    /* write the AND mask */
    for(i=0; i < tag->elem_size; i++) {
        if((tag->bit / 8) == i) {
            uint8_t mask = (uint8_t)(1 << (tag->bit % 8));

            /* if the bit is set, then we want to _not_ mask it off. */
            if(tag->data[tag->bit / 8] & mask) {
                *data = (uint8_t)0xFF;
            } else {
                *data = (uint8_t)(~mask);
            }

            pdebug(DEBUG_DETAIL, "adding OR mask byte %d: %x", i, *data);

            data++;
        } else {
            /* this is not the data we care about. */
            *data = (uint8_t)0xFF;

            pdebug(DEBUG_DETAIL, "adding OR mask byte %d: %x", i, *data);

            data++;
        }
    }

    /* let the rest of the system know that the write is complete after this. */
    tag->offset = tag->size;

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
    data++;
    mem_copy(data, tag->session->conn_path, tag->session->conn_path_size);
    data += tag->session->conn_path_size;

    /* now fill in the rest of the structure. */

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







int build_write_request_connected(ab_tag_p tag, int byte_offset)
{
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_req* cip = NULL;
    uint8_t* data = NULL;
    ab_request_p req = NULL;
    int multiple_requests = 0;
    int write_size = 0;

    pdebug(DEBUG_INFO, "Starting.");

    if(tag->is_bit) {
        return build_write_bit_request_connected(tag);
    }

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    rc = calculate_write_data_per_packet(tag);
    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to calculate valid write data per packet!.  rc=%s", plc_tag_decode_error(rc));
        return rc;
    }

    if(tag->write_data_per_packet < tag->size) {
        multiple_requests = 1;
    }

    if(multiple_requests && tag->plc_type == AB_PLC_OMRON_NJNX) {
        pdebug(DEBUG_WARN, "Tag too large for unfragmented request on Omron PLC!");
        return PLCTAG_ERR_TOO_LARGE;
    }

    cip = (eip_cip_co_req*)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_cip_co_req);

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
    *data = (multiple_requests) ? AB_EIP_CMD_CIP_WRITE_FRAG : AB_EIP_CMD_CIP_WRITE;
    data++;

    /* copy the tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* copy encoded type info */
    if (tag->encoded_type_info_size) {
        mem_copy(data, tag->encoded_type_info, tag->encoded_type_info_size);
        data += tag->encoded_type_info_size;
    } else {
        pdebug(DEBUG_WARN,"Data type unsupported!");
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* copy the item count, little endian */
    *((uint16_le*)data) = h2le16((uint16_t)(tag->elem_count));
    data += sizeof(uint16_le);

    if (multiple_requests) {
        /* put in the byte offset */
        *((uint32_le*)data) = h2le32((uint32_t)(byte_offset));
        data += sizeof(uint32_le);
    }

    /* how much data to write? */
    write_size = tag->size - tag->offset;

    if(write_size > tag->write_data_per_packet) {
        write_size = tag->write_data_per_packet;
    }

    /* now copy the data to write */
    mem_copy(data, tag->data + tag->offset, write_size);
    data += write_size;
    tag->offset += write_size;

    /* need to pad data to multiple of 16-bits */
    if (write_size & 0x01) {
        *data = 0;
        data++;
    }

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




int build_write_request_unconnected(ab_tag_p tag, int byte_offset)
{
    int rc = PLCTAG_STATUS_OK;
    eip_cip_uc_req* cip = NULL;
    uint8_t* data = NULL;
    uint8_t *embed_start = NULL;
    uint8_t *embed_end = NULL;
    ab_request_p req = NULL;
    int multiple_requests = 0;
    int write_size = 0;

    pdebug(DEBUG_INFO, "Starting.");

    if(tag->is_bit) {
        return build_write_bit_request_unconnected(tag);
    }

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    rc = calculate_write_data_per_packet(tag);
    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to calculate valid write data per packet!.  rc=%s", plc_tag_decode_error(rc));
        return rc;
    }

    if(tag->write_data_per_packet < tag->size) {
        multiple_requests = 1;
    }

    if(multiple_requests && tag->plc_type == AB_PLC_OMRON_NJNX) {
        pdebug(DEBUG_WARN, "Tag too large for unfragmented request on Omron PLC!");
        return PLCTAG_ERR_TOO_LARGE;
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
     * set up the CIP Read request type.
     * Different if more than one request.
     *
     * This handles a bug where attempting fragmented requests
     * does not appear to work with a single boolean.
     */
    *data = (multiple_requests) ? AB_EIP_CMD_CIP_WRITE_FRAG : AB_EIP_CMD_CIP_WRITE;
    data++;

    /* copy the tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* copy encoded type info */
    if (tag->encoded_type_info_size) {
        mem_copy(data, tag->encoded_type_info, tag->encoded_type_info_size);
        data += tag->encoded_type_info_size;
    } else {
        pdebug(DEBUG_WARN,"Data type unsupported!");
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* copy the item count, little endian */
    *((uint16_le*)data) = h2le16((uint16_t)(tag->elem_count));
    data += sizeof(uint16_le);

    if (multiple_requests) {
        /* put in the byte offset */
        *((uint32_le*)data) = h2le32((uint32_t)byte_offset);
        data += sizeof(uint32_le);
    }

    /* how much data to write? */
    write_size = tag->size - tag->offset;

    if(write_size > tag->write_data_per_packet) {
        write_size = tag->write_data_per_packet;
    }

    /* now copy the data to write */
    mem_copy(data, tag->data + tag->offset, write_size);
    data += write_size;
    tag->offset += write_size;

    /* need to pad data to multiple of 16-bits */
    if (write_size & 0x01) {
        *data = 0;
        data++;
    }

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
    data++;
    mem_copy(data, tag->session->conn_path, tag->session->conn_path_size);
    data += tag->session->conn_path_size;

    /* now fill in the rest of the structure. */

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
 * check_read_status_connected
 *
 * This routine checks for any outstanding requests and copies in data
 * that has arrived.  At the end of the request, it will clean up the request
 * buffers.  This is not thread-safe!  It should be called with the tag mutex
 * locked!
 */

static int check_read_status_connected(ab_tag_p tag)
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
            tag->size = tag->elem_count * tag->elem_size;

            break;
        }
    }

    if(rc != PLCTAG_STATUS_OK) {
        if(rc_is_error(rc)) {
            /* the request is dead, from session side. */
            tag->read_in_progress = 0;
            tag->offset = 0;

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

        /*
         * FIXME
         *
         * It probably should not be necessary to check for both as setting the type to anything other
         * than fragmented is error-prone.
         */

        if (cip_resp->reply_service != (AB_EIP_CMD_CIP_READ_FRAG | AB_EIP_CMD_CIP_OK)
            && cip_resp->reply_service != (AB_EIP_CMD_CIP_READ | AB_EIP_CMD_CIP_OK) ) {
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
        payload_size = (data_end - data);
        if(payload_size > 0) {
            /* the first byte of the response is a type byte. */
            pdebug(DEBUG_DETAIL, "type byte = %d (%x)", (int)*data, (int)*data);

            /* handle the data type part.  This can be long. */

            /* check for a simple/base type */
            if ((*data) >= AB_CIP_DATA_BIT && (*data) <= AB_CIP_DATA_STRINGI) {
                /* copy the type info for later. */
                if (tag->encoded_type_info_size == 0) {
                    tag->encoded_type_info_size = 2;
                    mem_copy(tag->encoded_type_info, data, tag->encoded_type_info_size);
                }

                /* skip the type byte and zero length byte */
                data += 2;
            } else if ((*data) == AB_CIP_DATA_ABREV_STRUCT || (*data) == AB_CIP_DATA_ABREV_ARRAY ||
                       (*data) == AB_CIP_DATA_FULL_STRUCT || (*data) == AB_CIP_DATA_FULL_ARRAY) {
                /* this is an aggregate type of some sort, the type info is variable length */
                int type_length = *(data + 1) + 2;  /*
                                                       * MAGIC
                                                       * add 2 to get the total length including
                                                       * the type byte and the length byte.
                                                       */

                /* check for extra long types */
                if (type_length > MAX_TAG_TYPE_INFO) {
                    pdebug(DEBUG_WARN, "Read data type info is too long (%d)!", type_length);
                    rc = PLCTAG_ERR_TOO_LARGE;
                    break;
                }

                /* copy the type info for later. */
                if (tag->encoded_type_info_size == 0) {
                    tag->encoded_type_info_size = type_length;
                    mem_copy(tag->encoded_type_info, data, tag->encoded_type_info_size);
                }

                data += type_length;
            } else {
                pdebug(DEBUG_WARN, "Unsupported data type returned, type byte=%d", *data);
                rc = PLCTAG_ERR_UNSUPPORTED;
                break;
            }

            /* check payload size now that we have bumped past the data type info. */
            payload_size = (data_end - data);

            /* copy the data into the tag and realloc if we need more space. */
            if(payload_size + tag->offset > tag->size) {
                tag->size = (int)payload_size + tag->offset;
                tag->elem_size = tag->size / tag->elem_count;

                pdebug(DEBUG_DETAIL, "Increasing tag buffer size to %d bytes.", tag->size);

                tag->data = (uint8_t*)mem_realloc(tag->data, tag->size);
                if(!tag->data) {
                    pdebug(DEBUG_WARN, "Unable to reallocate tag data memory!");
                    rc = PLCTAG_ERR_NO_MEM;
                    break;
                }
            }

            pdebug(DEBUG_INFO, "Got %d bytes of data", (int)payload_size);

            /*
             * copy the data, but only if this is not
             * a pre-read for a subsequent write!  We do not
             * want to overwrite the data the upstream has
             * put into the tag's data buffer.
             */
            if (!tag->pre_write_read) {
                mem_copy(tag->data + tag->offset, data, (int)(payload_size));
            }

            /* bump the byte offset */
            tag->offset += (int)(payload_size);
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
        /* this particular read is done. */
        tag->read_in_progress = 0;

        /* skip if we are doing a pre-write read. */
        if (!tag->pre_write_read && partial_data) {
            /* call read start again to get the next piece */
            pdebug(DEBUG_DETAIL, "calling tag_read_start() to get the next chunk.");
            rc = tag_read_start(tag);
        } else {
            /* done! */
            tag->first_read = 0;
            tag->offset = 0;

            /* if this is a pre-read for a write, then pass off to the write routine */
            if (tag->pre_write_read) {
                pdebug(DEBUG_DETAIL, "Restarting write call now.");
                tag->pre_write_read = 0;
                rc = tag_write_start(tag);
            }
        }
    }

    /* this is not an else clause because the above if could result in bad rc. */
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        /* error ! */
        pdebug(DEBUG_WARN, "Error received!");

        /* clean up everything. */
        ab_tag_abort(tag);
    }

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}



// /*
//  * check_read_tag_list_status_connected
//  *
//  * This routine checks for any outstanding tag list requests.  It will
//  * terminate when there is no data in the response and the error is not "more data".
//  *
//  * This is not thread-safe!  It should be called with the tag mutex
//  * locked!
//  */

// static int check_read_tag_list_status_connected(ab_tag_p tag)
// {
//     int rc = PLCTAG_STATUS_OK;
//     eip_cip_co_resp* cip_resp;
//     uint8_t* data;
//     uint8_t* data_end;
//     int partial_data = 0;

//     static int symbol_index=0;


//     pdebug(DEBUG_SPEW, "Starting.");

//     if(!tag) {
//         pdebug(DEBUG_ERROR,"Null tag pointer passed!");
//         return PLCTAG_ERR_NULL_PTR;
//     }

//     if (!tag->req) {
//         tag->read_in_progress = 0;
//         tag->offset = 0;

//         pdebug(DEBUG_WARN,"Read in progress, but no request in flight!");

//         return PLCTAG_ERR_READ;
//     }

//     /* request can be used by two threads at once. */
//     spin_block(&tag->req->lock) {
//         if(!tag->req->resp_received) {
//             rc = PLCTAG_STATUS_PENDING;
//             break;
//         }

//         /* check to see if it was an abort on the session side. */
//         if(tag->req->status != PLCTAG_STATUS_OK) {
//             rc = tag->req->status;
//             tag->req->abort_request = 1;

//             pdebug(DEBUG_WARN,"Session reported failure of request: %s.", plc_tag_decode_error(rc));

//             tag->read_in_progress = 0;
//             tag->offset = 0;
//             tag->next_id = 0;
//             tag->size = tag->elem_count * tag->elem_size;

//             break;
//         }
//     }

//     if(rc != PLCTAG_STATUS_OK) {
//         if(rc_is_error(rc)) {
//             /* the request is dead, from session side. */
//             tag->req = rc_dec(tag->req);
//         }

//         return rc;
//     }

//     /* the request is ours exclusively. */

//     /* point to the data */
//     cip_resp = (eip_cip_co_resp*)(tag->req->data);

//     /* point to the start of the data */
//     data = (tag->req->data) + sizeof(eip_cip_co_resp);

//     /* point the end of the data */
//     data_end = (tag->req->data + le2h16(cip_resp->encap_length) + sizeof(eip_encap));

//     /* check the status */
//     do {
//         ptrdiff_t payload_size = (data_end - data);

//         if (le2h16(cip_resp->encap_command) != AB_EIP_CONNECTED_SEND) {
//             pdebug(DEBUG_WARN, "Unexpected EIP packet type received: %d!", cip_resp->encap_command);
//             rc = PLCTAG_ERR_BAD_DATA;
//             break;
//         }

//         if (le2h32(cip_resp->encap_status) != AB_EIP_OK) {
//             pdebug(DEBUG_WARN, "EIP command failed, response code: %d", le2h32(cip_resp->encap_status));
//             rc = PLCTAG_ERR_REMOTE_ERR;
//             break;
//         }

//         if (cip_resp->reply_service != (AB_EIP_CMD_CIP_LIST_TAGS | AB_EIP_CMD_CIP_OK) ) {
//             pdebug(DEBUG_WARN, "CIP response reply service unexpected: %d", cip_resp->reply_service);
//             rc = PLCTAG_ERR_BAD_DATA;
//             break;
//         }

//         if (cip_resp->status != AB_CIP_STATUS_OK && cip_resp->status != AB_CIP_STATUS_FRAG) {
//             pdebug(DEBUG_WARN, "CIP read failed with status: 0x%x %s", cip_resp->status, decode_cip_error_short((uint8_t *)&cip_resp->status));
//             pdebug(DEBUG_INFO, decode_cip_error_long((uint8_t *)&cip_resp->status));
//             rc = decode_cip_error_code((uint8_t *)&cip_resp->status);
//             break;
//         }

//         /* check to see if this is a partial response. */
//         partial_data = (cip_resp->status == AB_CIP_STATUS_FRAG);

//         /*
//          * check to see if there is any data to process.  If this is a packed
//          * response, there might not be.
//          */
//         if(payload_size > 0) {
//             uint8_t *current_entry_data = data;

//             /* copy the data into the tag and realloc if we need more space. */

//             if(payload_size + tag->offset > tag->size) {
//                 tag->elem_count = tag->size = (int)payload_size + tag->offset;

//                 pdebug(DEBUG_DETAIL, "Increasing tag buffer size to %d bytes.", tag->size);

//                 tag->data = (uint8_t*)mem_realloc(tag->data, tag->size);
//                 if(!tag->data) {
//                     pdebug(DEBUG_WARN, "Unable to reallocate tag data memory!");
//                     rc = PLCTAG_ERR_NO_MEM;
//                     break;
//                 }
//             }

//             /* copy the data into the tag's data buffer. */
//             mem_copy(tag->data + tag->offset, data, (int)payload_size);

//             tag->offset += (int)payload_size;

//             pdebug(DEBUG_DETAIL, "current offset %d", tag->offset);

//             /* scan through the data to get the next ID to use. */
//             while((data_end - current_entry_data) > 0) {
//                 tag_list_entry *current_entry = (tag_list_entry*)current_entry_data;

//                 /* first element is the symbol instance ID */
//                 tag->next_id = (uint16_t)(le2h32(current_entry->instance_id) + 1);

//                 pdebug(DEBUG_DETAIL, "Next ID: %d", tag->next_id);

//                 /* skip past to the next instance. */
//                 current_entry_data += (sizeof(*current_entry) + le2h16(current_entry->string_len));

//                 symbol_index++;
//             }
//         } else {
//             pdebug(DEBUG_DETAIL, "Response returned no data and no error.");
//         }

//         /* set the return code */
//         rc = PLCTAG_STATUS_OK;
//     } while(0);

//     /* clean up the request */
//     tag->req->abort_request = 1;
//     tag->req = rc_dec(tag->req);

//     /* are we actually done? */
//     if (rc == PLCTAG_STATUS_OK) {
//         /* this read is done. */
//         tag->read_in_progress = 0;

//         /* keep going if we are not done yet. */
//         if (partial_data) {
//             /* call read start again to get the next piece */
//             pdebug(DEBUG_DETAIL, "calling tag_read_start() to get the next chunk.");
//             rc = tag_read_start(tag);
//         } else {
//             /* done! */
//             pdebug(DEBUG_DETAIL, "Done reading tag list data!");

//             pdebug(DEBUG_DETAIL, "total symbols: %d", symbol_index);

//             tag->elem_count = tag->offset;

//             tag->first_read = 0;
//             tag->offset = 0;
//             tag->next_id = 0;
//         }
//     }

//     /* this is not an else clause because the above if could result in bad rc. */
//     if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
//         /* error ! */
//         pdebug(DEBUG_WARN, "Error received: %s!", plc_tag_decode_error(rc));

//         tag->offset = 0;
//         tag->next_id = 0;

//         /* clean up everything. */
//         ab_tag_abort(tag);
//     }

//     pdebug(DEBUG_SPEW, "Done.");

//     return rc;
// }





static int check_read_status_unconnected(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    eip_cip_uc_resp* cip_resp;
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
    cip_resp = (eip_cip_uc_resp*)(tag->req->data);

    /* point to the start of the data */
    data = (tag->req->data) + sizeof(eip_cip_uc_resp);

    /* point the end of the data */
    data_end = (tag->req->data + le2h16(cip_resp->encap_length) + sizeof(eip_encap));

    /* check the status */
    do {
        ptrdiff_t payload_size = (data_end - data);

        if (le2h16(cip_resp->encap_command) != AB_EIP_UNCONNECTED_SEND) {
            pdebug(DEBUG_WARN, "Unexpected EIP packet type received: %d!", cip_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if (le2h32(cip_resp->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "EIP command failed, response code: %d", le2h32(cip_resp->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        /*
         * FIXME
         *
         * It probably should not be necessary to check for both as setting the type to anything other
         * than fragmented is error-prone.
         */

        if (cip_resp->reply_service != (AB_EIP_CMD_CIP_READ_FRAG | AB_EIP_CMD_CIP_OK)
            && cip_resp->reply_service != (AB_EIP_CMD_CIP_READ | AB_EIP_CMD_CIP_OK) ) {
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

        /* the first byte of the response is a type byte. */
        pdebug(DEBUG_DETAIL, "type byte = %d (%x)", (int)*data, (int)*data);

        /* copy the type data. */

        /* check for a simple/base type */

        if ((*data) >= AB_CIP_DATA_BIT && (*data) <= AB_CIP_DATA_STRINGI) {
            /* copy the type info for later. */
            if (tag->encoded_type_info_size == 0) {
                tag->encoded_type_info_size = 2;
                mem_copy(tag->encoded_type_info, data, tag->encoded_type_info_size);
            }

            /* skip the type byte and zero length byte */
            data += 2;
        } else if ((*data) == AB_CIP_DATA_ABREV_STRUCT || (*data) == AB_CIP_DATA_ABREV_ARRAY ||
                   (*data) == AB_CIP_DATA_FULL_STRUCT || (*data) == AB_CIP_DATA_FULL_ARRAY) {
            /* this is an aggregate type of some sort, the type info is variable length */
            int type_length =
                *(data + 1) + 2;  /*
                                   * MAGIC
                                   * add 2 to get the total length including
                                   * the type byte and the length byte.
                                   */

            /* check for extra long types */
            if (type_length > MAX_TAG_TYPE_INFO) {
                pdebug(DEBUG_WARN, "Read data type info is too long (%d)!", type_length);
                rc = PLCTAG_ERR_TOO_LARGE;
                break;
            }

            /* copy the type info for later. */
            if (tag->encoded_type_info_size == 0) {
                tag->encoded_type_info_size = type_length;
                mem_copy(tag->encoded_type_info, data, tag->encoded_type_info_size);
            }

            data += type_length;
        } else {
            pdebug(DEBUG_WARN, "Unsupported data type returned, type byte=%d", *data);
            rc = PLCTAG_ERR_UNSUPPORTED;
            break;
        }

        /* check payload size now that we have bumped past the data type info. */
        payload_size = (data_end - data);

        /* copy the data into the tag and realloc if we need more space. */
        if(payload_size + tag->offset > tag->size) {
            tag->size = (int)payload_size + tag->offset;
            tag->elem_size = tag->size / tag->elem_count;

            pdebug(DEBUG_DETAIL, "Increasing tag buffer size to %d bytes.", tag->size);

            tag->data = (uint8_t*)mem_realloc(tag->data, tag->size);
            if(!tag->data) {
                pdebug(DEBUG_WARN, "Unable to reallocate tag data memory!");
                rc = PLCTAG_ERR_NO_MEM;
                break;
            }
        }

        pdebug(DEBUG_INFO, "Got %d bytes of data", (int)payload_size);


        /*
         * copy the data, but only if this is not
         * a pre-read for a subsequent write!  We do not
         * want to overwrite the data the upstream has
         * put into the tag's data buffer.
         */
        if (!tag->pre_write_read) {
            mem_copy(tag->data + tag->offset, data, (int)payload_size);
        }

        /* bump the byte offset */
        tag->offset += (int)payload_size;

        /* set the return code */
        rc = PLCTAG_STATUS_OK;
    } while(0);


    /* clean up the request */
    tag->req->abort_request = 1;
    tag->req = rc_dec(tag->req);

    /* are we actually done? */
    if (rc == PLCTAG_STATUS_OK) {
        /* this read is done. */
        tag->read_in_progress = 0;

        /* skip if we are doing a pre-write read. */
        if (!tag->pre_write_read && partial_data && tag->offset < tag->size) {
            /* call read start again to get the next piece */
            pdebug(DEBUG_DETAIL, "calling tag_read_start() to get the next chunk.");
            rc = tag_read_start(tag);
        } else {
            /* done! */
            tag->first_read = 0;
            tag->offset = 0;

            /* if this is a pre-read for a write, then pass off to the write routine */
            if (tag->pre_write_read) {
                pdebug(DEBUG_DETAIL, "Restarting write call now.");

                tag->pre_write_read = 0;
                rc = tag_write_start(tag);
            }
        }
    }

    /* this is not an else clause because the above if could result in bad rc. */
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        /* error ! */
        pdebug(DEBUG_WARN, "Error received!");

        /* clean up everything. */
        ab_tag_abort(tag);
    }

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}



/*
 * check_write_status_connected
 *
 * This routine must be called with the tag mutex locked.  It checks the current
 * status of a write operation.  If the write is done, it triggers the clean up.
 */

static int check_write_status_connected(ab_tag_p tag)
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

        if (cip_resp->reply_service != (AB_EIP_CMD_CIP_WRITE_FRAG | AB_EIP_CMD_CIP_OK)
            && cip_resp->reply_service != (AB_EIP_CMD_CIP_WRITE | AB_EIP_CMD_CIP_OK)
            && cip_resp->reply_service != (AB_EIP_CMD_CIP_RMW | AB_EIP_CMD_CIP_OK)) {
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
    } while(0);

    /* clean up the request. */
    tag->req->abort_request = 1;
    tag->req = rc_dec(tag->req);

    /* write is done in one way or another. */
    tag->write_in_progress = 0;

    if(rc == PLCTAG_STATUS_OK) {
        if(tag->offset < tag->size) {

            pdebug(DEBUG_DETAIL, "Write not complete, triggering next round.");
            rc = tag_write_start(tag);
        } else {
            /* only clear this if we are done. */
            tag->offset = 0;
        }
    } else {
        pdebug(DEBUG_WARN,"Write failed!");

        tag->offset = 0;
    }

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}





static int check_write_status_unconnected(ab_tag_p tag)
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

    /* request is exclusively ours. */

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

        if (cip_resp->reply_service != (AB_EIP_CMD_CIP_WRITE_FRAG | AB_EIP_CMD_CIP_OK)
            && cip_resp->reply_service != (AB_EIP_CMD_CIP_WRITE | AB_EIP_CMD_CIP_OK)
            && cip_resp->reply_service != (AB_EIP_CMD_CIP_RMW | AB_EIP_CMD_CIP_OK)) {
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
    } while(0);

    /* clean up the request. */
    tag->req->abort_request = 1;
    tag->req = rc_dec(tag->req);

    /* write is done in one way or another */
    tag->write_in_progress = 0;

    if(rc == PLCTAG_STATUS_OK) {
        if(tag->offset < tag->size) {

            pdebug(DEBUG_DETAIL, "Write not complete, triggering next round.");
            rc = tag_write_start(tag);
        } else {
            /* only clear this if we are done. */
            tag->offset = 0;
        }
    } else {
        pdebug(DEBUG_WARN,"Write failed!");
        tag->offset = 0;
    }

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}




int calculate_write_data_per_packet(ab_tag_p tag)
{
    int overhead = 0;
    int data_per_packet = 0;
    int max_payload_size = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* if we are here, then we have all the type data etc. */
    if(tag->use_connected_msg) {
        pdebug(DEBUG_DETAIL,"Connected tag.");
        max_payload_size = session_get_max_payload(tag->session);
        overhead =  1                               /* service request, one byte */
                    + tag->encoded_name_size        /* full encoded name */
                    + tag->encoded_type_info_size   /* encoded type size */
                    + 2                             /* element count, 16-bit int */
                    + 4                             /* byte offset, 32-bit int */
                    + 8;                            /* MAGIC fudge factor */
    } else {
        pdebug(DEBUG_DETAIL,"Unconnected tag.");
        max_payload_size = session_get_max_payload(tag->session);
        overhead =  1                               /* service request, one byte */
                    + tag->encoded_name_size        /* full encoded name */
                    + tag->encoded_type_info_size   /* encoded type size */
                    + tag->session->conn_path_size + 2       /* encoded device path size plus two bytes for length and padding */
                    + 2                             /* element count, 16-bit int */
                    + 4                             /* byte offset, 32-bit int */
                    + 8;                            /* MAGIC fudge factor */
    }

    data_per_packet = max_payload_size - overhead;

    pdebug(DEBUG_DETAIL,"Write packet maximum size is %d, write overhead is %d, and write data per packet is %d.", max_payload_size, overhead, data_per_packet);

    if (data_per_packet <= 0) {
        pdebug(DEBUG_WARN,
               "Unable to send request.  Packet overhead, %d bytes, is too large for packet, %d bytes!",
               overhead,
               max_payload_size);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* we want a multiple of 8 bytes */
    data_per_packet &= 0xFFFFF8;

    tag->write_data_per_packet = data_per_packet;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}



// int setup_tag_listing(ab_tag_p tag, const char *name)
// {
//     char **tag_parts = NULL;

//     pdebug(DEBUG_DETAIL, "Starting.");

//     /* Check for a match with just "@tags" for a controller tag listing. */
//     if(str_cmp_i(name, "@tags") == 0) {
//         /* controller tag listing. */
//         pdebug(DEBUG_DETAIL, "Tag is a controller tag listing request.");

//         /* fall through to the last part to set up the tag. */
//     } else if(str_length(name) >= str_length("PROGRAM:x.@tags")) {
//         tag_parts = str_split(name, ".");

//         /* check to make sure that we have at least one part. */
//         if(!tag_parts) {
//             pdebug(DEBUG_INFO, "Tag is not a tag listing request.");
//             return PLCTAG_ERR_NOT_FOUND;
//         }

//         /* check that we have exactly two parts. */
//         if(tag_parts[0] != NULL && tag_parts[1] != NULL && tag_parts[2] == NULL) {
//             /* we have exactly two parts. Make sure the last part is "@tags" */
//             if(str_cmp_i(tag_parts[1], "@tags") != 0) {
//                 mem_free(tag_parts);
//                 pdebug(DEBUG_INFO, "Tag is not a tag listing request.");
//                 return PLCTAG_ERR_NOT_FOUND;
//             }

//             if(str_length(tag_parts[0]) <= str_length("PROGRAM:x")) {
//                 mem_free(tag_parts);
//                 pdebug(DEBUG_INFO, "Tag is not a tag listing request.");
//                 return PLCTAG_ERR_NOT_FOUND;
//             }

//             /* make sure the first part is "PROGRAM:" */
//             if((tag_parts[0][0]) != 'P' && (tag_parts[0][0]) != 'p') {
//                 mem_free(tag_parts);
//                 pdebug(DEBUG_INFO, "Tag %s is not a tag listing request.", name);
//                 return PLCTAG_ERR_NOT_FOUND;

//             }

//             if((tag_parts[0][1]) != 'R' && (tag_parts[0][1]) != 'r') {
//                 mem_free(tag_parts);
//                 pdebug(DEBUG_INFO, "Tag %s is not a tag listing request.", name);
//                 return PLCTAG_ERR_NOT_FOUND;
//             }

//             if((tag_parts[0][2]) != 'O' && (tag_parts[0][2]) != 'o') {
//                 mem_free(tag_parts);
//                 pdebug(DEBUG_INFO, "Tag %s is not a tag listing request.", name);
//                 return PLCTAG_ERR_NOT_FOUND;
//             }

//             if((tag_parts[0][3]) != 'G' && (tag_parts[0][3]) != 'g') {
//                 mem_free(tag_parts);
//                 pdebug(DEBUG_INFO, "Tag %s is not a tag listing request.", name);
//                 return PLCTAG_ERR_NOT_FOUND;
//             }

//             if((tag_parts[0][4]) != 'R' && (tag_parts[0][4]) != 'r') {
//                 mem_free(tag_parts);
//                 pdebug(DEBUG_INFO, "Tag %s is not a tag listing request.", name);
//                 return PLCTAG_ERR_NOT_FOUND;
//             }

//             if((tag_parts[0][5]) != 'A' && (tag_parts[0][5]) != 'a') {
//                 mem_free(tag_parts);
//                 pdebug(DEBUG_INFO, "Tag %s is not a tag listing request.", name);
//                 return PLCTAG_ERR_NOT_FOUND;
//             }

//             if((tag_parts[0][6]) != 'M' && (tag_parts[0][6]) != 'm') {
//                 mem_free(tag_parts);
//                 pdebug(DEBUG_INFO, "Tag %s is not a tag listing request.", name);
//                 return PLCTAG_ERR_NOT_FOUND;
//             }

//             if((tag_parts[0][7]) != ':') {
//                 mem_free(tag_parts);
//                 pdebug(DEBUG_INFO, "Tag %s is not a tag listing request.", name);
//                 return PLCTAG_ERR_NOT_FOUND;
//             }

//             /* we have a program tag request! */
//             if(cip_encode_tag_name(tag, tag_parts[0]) != PLCTAG_STATUS_OK) {
//                 mem_free(tag_parts);
//                 pdebug(DEBUG_WARN, "Tag program listing, %s, is not able to be encoded!", name);
//                 return PLCTAG_ERR_BAD_PARAM;
//             }
//         } else {
//             mem_free(tag_parts);
//             pdebug(DEBUG_INFO, "Tag is not a tag listing request.");
//             return PLCTAG_ERR_NOT_FOUND;
//         }
//     } else {
//         pdebug(DEBUG_INFO, "Tag is not a tag listing request.");
//         return PLCTAG_ERR_NOT_FOUND;
//     }

//     if(tag_parts) {
//         mem_free(tag_parts);
//     }

//     tag->tag_list = 1;
//     tag->elem_type = AB_TYPE_TAG_ENTRY;
//     tag->elem_count = 1;  /* place holder */
//     tag->elem_size = 1;

//     pdebug(DEBUG_DETAIL, "Done.");

//     return PLCTAG_STATUS_OK;
// }
