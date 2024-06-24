/***************************************************************************
 *   Copyright (C) 2024 by Kyle Hayes                                      *
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
#include <omron/defs.h>
#include <omron/omron_common.h>
#include <omron/cip.h>
#include <omron/tag.h>
#include <omron/conn.h>
#include <omron/eip_cip.h>  /* for the Logix decode types. */
#include <omron/omron_listing_tag.h>
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
//    uint8_t request_service;    /* OMRON_EIP_CMD_CIP_LIST_TAGS=0x55 */
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



/* listing tag functions. */
static int listing_tag_read_start(omron_tag_p tag);
static int listing_tag_tickler(omron_tag_p tag);
//static int listing_tag_write_start(omron_tag_p tag);
static int listing_tag_check_read_status_connected(omron_tag_p tag);
static int listing_tag_build_read_request_connected(omron_tag_p tag);


/* define the vtable for listing tag type. */
static struct tag_vtable_t listing_tag_vtable = {
    (tag_vtable_func)omron_tag_abort, /* shared */
    (tag_vtable_func)listing_tag_read_start,
    (tag_vtable_func)omron_tag_status, /* shared */
    (tag_vtable_func)listing_tag_tickler,
    (tag_vtable_func)NULL, /* write */
    (tag_vtable_func)NULL, /* wake_plc */

    /* attribute accessors */
    omron_get_int_attrib,
    omron_set_int_attrib,

    omron_get_byte_array_attrib
};



tag_byte_order_t omron_tag_listing_byte_order = {
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




/******************************************************************
 ******************* tag listing functions ************************
 ******************************************************************/




/*
 * Handle tag listing tag set up.
 *
 * There are two main cases here: 1) a bare tag listing, 2) a program tag listing.
 * We know that we got here because the string "@tags" was in the name.
 */

int omron_setup_tag_listing_tag(omron_tag_p tag, const char *name)
{
    int rc = PLCTAG_STATUS_OK;
    char **tag_parts = NULL;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* FIXME - Omron tag listing does _NOT_ work like this. Rewrite for Omron. */
    return PLCTAG_ERR_UNSUPPORTED;

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
                if(cip.encode_tag_name(tag, tag_parts[0]) != PLCTAG_STATUS_OK) {
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
        tag->elem_type = OMRON_TYPE_TAG_ENTRY;
        tag->elem_count = 1;
        tag->elem_size = 1;

        tag->byte_order = &omron_tag_listing_byte_order;

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

int listing_tag_read_start(omron_tag_p tag)
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



int listing_tag_tickler(omron_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW,"Starting.");

    if (tag->read_in_progress) {
        if(tag->elem_type == OMRON_TYPE_TAG_RAW) {
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

static int listing_tag_check_read_status_connected(omron_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_resp* cip_resp;
    uint8_t* data;
    uint8_t* data_end;
    int partial_data = 0;
    omron_request_p request = NULL;

    static int symbol_index=0;


    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_ERROR,"Null tag pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* guard against the request being deleted out from underneath us. */
    request = rc_inc(tag->req);
    rc = omron_check_read_reqest_status(tag, request);
    if(rc != PLCTAG_STATUS_OK)  {
        pdebug(DEBUG_DETAIL, "Read request status is not OK.");
        rc_dec(request);
        return rc;
    }

    /* the request reference is still valid. */

    /* point to the data */
    cip_resp = (eip_cip_co_resp*)(request->data);

    /* point to the start of the data */
    data = (request->data) + sizeof(eip_cip_co_resp);

    /* point the end of the data */
    data_end = (request->data + le2h16(cip_resp->encap_length) + sizeof(eip_encap));

    /* check the status */
    do {
        ptrdiff_t payload_size = (data_end - data);

        if (le2h16(cip_resp->encap_command) != OMRON_EIP_CONNECTED_SEND) {
            pdebug(DEBUG_WARN, "Unexpected EIP packet type received: %d!", cip_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if (le2h32(cip_resp->encap_status) != OMRON_EIP_OK) {
            pdebug(DEBUG_WARN, "EIP command failed, response code: %d", le2h32(cip_resp->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if (cip_resp->reply_service != (OMRON_EIP_CMD_CIP_LIST_TAGS | OMRON_EIP_CMD_CIP_OK) ) {
            pdebug(DEBUG_WARN, "CIP response reply service unexpected: %d", cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if (cip_resp->status != OMRON_CIP_STATUS_OK && cip_resp->status != OMRON_CIP_STATUS_FRAG) {
            pdebug(DEBUG_WARN, "CIP read failed with status: 0x%x %s", cip_resp->status, cip.decode_cip_error_short((uint8_t *)&cip_resp->status));
            pdebug(DEBUG_INFO, cip.decode_cip_error_long((uint8_t *)&cip_resp->status));
            rc = cip.decode_cip_error_code((uint8_t *)&cip_resp->status);
            break;
        }

        /* check to see if this is a partial response. */
        partial_data = (cip_resp->status == OMRON_CIP_STATUS_FRAG);

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
    request->abort_request = 1;
    tag->req = rc_dec(request);

    /*
     * huh?  Yes, we do it a second time because we already had
     * a reference and got another at the top of this function.
     * So we need to remove it twice.   Once for the capture above,
     * and once for the original reference.
     */

    rc_dec(request);

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
        omron_tag_abort(tag);
    }

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}




int listing_tag_build_read_request_connected(omron_tag_p tag)
{
    eip_cip_co_req* cip = NULL;
    //tag_list_req *list_req = NULL;
    omron_request_p req = NULL;
    int rc = PLCTAG_STATUS_OK;
    uint8_t *data_start = NULL;
    uint8_t *data = NULL;
    uint16_le tmp_u16 = UINT16_LE_INIT(0);

    pdebug(DEBUG_INFO, "Starting.");

    /* get a request buffer */
    rc = conn_create_request(tag->conn, tag->tag_id, &req);
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
        uint8_t request_service;    OMRON_EIP_CMD_CIP_LIST_TAGS=0x55
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

    *data = OMRON_EIP_CMD_CIP_LIST_TAGS;
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
    cip->encap_command = h2le16(OMRON_EIP_CONNECTED_SEND); /* ALWAYS 0x0070 Connected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                 /* ALWAYS 2 */
    cip->cpf_cai_item_type = h2le16(OMRON_EIP_ITEM_CAI);/* ALWAYS 0x00A1 connected address item */
    cip->cpf_cai_item_length = h2le16(4);            /* ALWAYS 4, size of connection ID*/
    cip->cpf_cdi_item_type = h2le16(OMRON_EIP_ITEM_CDI);/* ALWAYS 0x00B1 - connected Data Item */
    cip->cpf_cdi_item_length = h2le16((uint16_t)((int)(data - data_start) + (int)sizeof(cip->cpf_conn_seq_num)));

    /* set the size of the request */
    req->request_size = (int)((int)sizeof(*cip) + (int)(data - data_start));

    req->allow_packing = tag->allow_packing;

    /* add the request to the conn's list. */
    rc = conn_add_request(tag->conn, req);

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to add request to conn! rc=%d", rc);
        tag->req = rc_dec(req);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_INFO, "Done");

    return PLCTAG_STATUS_OK;
}
