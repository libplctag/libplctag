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

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include "cip.h"
#include "eip.h"
#include "pccc.h"
#include "plc.h"
#include "slice.h"
#include "utils.h"


/* tag commands */
const uint8_t CIP_MULTI[] = { 0x0A, 0x02, 0x20, 0x02, 0x24, 0x01 };
const uint8_t CIP_READ[] = { 0x4C };
const uint8_t CIP_WRITE[] = { 0x4D };
const uint8_t CIP_RMW[] = { 0x4E, 0x02, 0x20, 0x02, 0x24, 0x01 };
const uint8_t CIP_READ_FRAG[] = { 0x52 };
const uint8_t CIP_WRITE_FRAG[] = { 0x53 };


/* non-tag commands */
//4b 02 20 67 24 01 07 3d f3 45 43 50 21
const uint8_t CIP_PCCC_EXECUTE[] = { 0x4B, 0x02, 0x20, 0x67, 0x24, 0x01, 0x07, 0x3d, 0xf3, 0x45, 0x43, 0x50, 0x21 };
const uint8_t CIP_FORWARD_CLOSE[] = { 0x4E, 0x02, 0x20, 0x06, 0x24, 0x01 };
const uint8_t CIP_FORWARD_OPEN[] = { 0x54, 0x02, 0x20, 0x06, 0x24, 0x01 };
const uint8_t CIP_LIST_TAGS[] = { 0x55, 0x02, 0x20, 0x02, 0x24, 0x01 };
const uint8_t CIP_FORWARD_OPEN_EX[] = { 0x5B, 0x02, 0x20, 0x06, 0x24, 0x01 };

/* path to match. */
// uint8_t LOGIX_CONN_PATH[] = { 0x03, 0x00, 0x00, 0x20, 0x02, 0x24, 0x01 };
// uint8_t MICRO800_CONN_PATH[] = { 0x02, 0x20, 0x02, 0x24, 0x01 };

#define CIP_DONE               ((uint8_t)0x80)

#define CIP_SYMBOLIC_SEGMENT_MARKER ((uint8_t)0x91)

/* CIP Errors */

#define CIP_OK                  ((uint8_t)0x00)
#define CIP_ERR_0x01            ((uint8_t)0x01)
#define CIP_ERR_FRAG            ((uint8_t)0x06)
#define CIP_ERR_UNSUPPORTED     ((uint8_t)0x08)
#define CIP_ERR_EXTENDED        ((uint8_t)0xff)

#define CIP_ERR_EX_TOO_LONG     ((uint16_t)0x2105)

typedef struct {
    uint8_t service_code;   /* why is the operation code _before_ the path? */
    uint8_t path_size;      /* size in 16-bit words of the path */
    slice_s path;           /* store this in a slice to avoid copying */
} cip_header_s;

static slice_s handle_forward_open(slice_s input, slice_s output, plc_s *plc);
static slice_s handle_forward_close(slice_s input, slice_s output, plc_s *plc);
static slice_s handle_read_request(slice_s input, slice_s output, plc_s *plc);
static slice_s handle_write_request(slice_s input, slice_s output, plc_s *plc);

static bool process_tag_segment(plc_s *plc, slice_s input, tag_def_s **tag, size_t *start_read_offset);
static slice_s make_cip_error(slice_s output, uint8_t cip_cmd, uint8_t cip_err, bool extend, uint16_t extended_error);
static bool match_path(slice_s input, bool need_pad, uint8_t *path, uint8_t path_len);

slice_s cip_dispatch_request(slice_s input, slice_s output, plc_s *plc)
{
    info("Got packet:");
    slice_dump(input);

    /* match the prefix and dispatch. */
    if(slice_match_bytes(input, CIP_READ, sizeof(CIP_READ))) {
        return handle_read_request(input, output, plc);
    } else if(slice_match_bytes(input, CIP_READ_FRAG, sizeof(CIP_READ_FRAG))) {
        return handle_read_request(input, output, plc);
    } else if(slice_match_bytes(input, CIP_WRITE, sizeof(CIP_WRITE))) {
        return handle_write_request(input, output, plc);
    } else if(slice_match_bytes(input, CIP_WRITE_FRAG, sizeof(CIP_WRITE_FRAG))) {
        return handle_write_request(input, output, plc);
    } else if(slice_match_bytes(input, CIP_FORWARD_OPEN, sizeof(CIP_FORWARD_OPEN))) {
        return handle_forward_open(input, output, plc);
    } else if(slice_match_bytes(input, CIP_FORWARD_OPEN_EX, sizeof(CIP_FORWARD_OPEN_EX))) {
        return handle_forward_open(input, output, plc);
    } else if(slice_match_bytes(input, CIP_FORWARD_CLOSE, sizeof(CIP_FORWARD_CLOSE))) {
        return handle_forward_close(input, output, plc);
    } else if(slice_match_bytes(input, CIP_PCCC_EXECUTE, sizeof(CIP_PCCC_EXECUTE))) {
        return dispatch_pccc_request(input, output, plc);
    } else {
            return make_cip_error(output, (uint8_t)(slice_get_uint8(input, 0) | (uint8_t)CIP_DONE), (uint8_t)CIP_ERR_UNSUPPORTED, false, (uint16_t)0);
    }
}


/* a handy structure to hold all the parameters we need to receive in a Forward Open request. */
typedef struct {
    uint8_t secs_per_tick;                  /* seconds per tick */
    uint8_t timeout_ticks;                  /* timeout = srd_secs_per_tick * src_timeout_ticks */
    uint32_t server_conn_id;                /* 0, returned by server in reply. */
    uint32_t client_conn_id;                /* sent by client. */
    uint16_t conn_serial_number;            /* client connection ID/serial number */
    uint16_t orig_vendor_id;                /* client unique vendor ID */
    uint32_t orig_serial_number;            /* client unique serial number */
    uint8_t conn_timeout_multiplier;        /* timeout = mult * RPI */
    uint8_t reserved[3];                    /* reserved, set to 0 */
    uint32_t client_to_server_rpi;          /* us to target RPI - Request Packet Interval in microseconds */
    uint32_t client_to_server_conn_params;  /* some sort of identifier of what kind of PLC we are??? */
    uint32_t server_to_client_rpi;          /* target to us RPI, in microseconds */
    uint32_t server_to_client_conn_params;       /* some sort of identifier of what kind of PLC the target is ??? */
    uint8_t transport_class;                /* ALWAYS 0xA3, server transport, class 3, application trigger */
    slice_s path;                           /* connection path. */
} forward_open_s;

/* the minimal Forward Open with no path */
#define CIP_FORWARD_OPEN_MIN_SIZE   (46)


slice_s handle_forward_open(slice_s input, slice_s output, plc_s *plc)
{
    slice_s conn_path;
    size_t offset = 0;
    uint8_t fo_cmd = slice_get_uint8(input, 0);
    forward_open_s fo_req = {0};

    info("Checking Forward Open request:");
    slice_dump(input);

    /* minimum length check */
    if(slice_len(input) < CIP_FORWARD_OPEN_MIN_SIZE) {
        /* FIXME - send back the right error. */
        info("Size of request too small!");
        return make_cip_error(output, (uint8_t)(slice_get_uint8(input, 0) | (uint8_t)CIP_DONE), (uint8_t)CIP_ERR_UNSUPPORTED, false, (uint16_t)0);
    }

    /* get the data. */
    offset = sizeof(CIP_FORWARD_OPEN); /* step past the path to the CM */
    fo_req.secs_per_tick = slice_get_uint8(input, offset); offset++;
    fo_req.timeout_ticks = slice_get_uint8(input, offset); offset++;
    fo_req.server_conn_id = slice_get_uint32_le(input, offset); offset += 4;
    fo_req.client_conn_id = slice_get_uint32_le(input, offset); offset += 4;
    fo_req.conn_serial_number = slice_get_uint16_le(input, offset); offset += 2;
    fo_req.orig_vendor_id = slice_get_uint16_le(input, offset); offset += 2;
    fo_req.orig_serial_number = slice_get_uint32_le(input, offset); offset += 4;
    fo_req.conn_timeout_multiplier = slice_get_uint8(input, offset); offset += 4; /* byte plus 3-bytes of padding. */
    fo_req.client_to_server_rpi = slice_get_uint32_le(input, offset); offset += 4;
    if(fo_cmd == CIP_FORWARD_OPEN[0]) {
        /* old command uses 16-bit value. */
        fo_req.client_to_server_conn_params = slice_get_uint16_le(input, offset); offset += 2;
    } else {
        /* new command has 32-bit field here. */
        fo_req.client_to_server_conn_params = slice_get_uint32_le(input, offset); offset += 4;
    }
    fo_req.server_to_client_rpi = slice_get_uint32_le(input, offset); offset += 4;
    if(fo_cmd == CIP_FORWARD_OPEN[0]) {
        /* old command uses 16-bit value. */
        fo_req.server_to_client_conn_params = slice_get_uint16_le(input, offset); offset += 2;
    } else {
        /* new command has 32-bit field here. */
        fo_req.server_to_client_conn_params = slice_get_uint32_le(input, offset); offset += 4;
    }
    fo_req.transport_class = slice_get_uint8(input, offset); offset++;

    /* check the remaining length */
    if(offset >= slice_len(input)) {
        /* FIXME - send back the right error. */
        info("Forward open request size, %d, too small.   Should be greater than %d!", slice_len(input), offset);
        return make_cip_error(output, (uint8_t)(slice_get_uint8(input, 0) | CIP_DONE), (uint8_t)CIP_ERR_UNSUPPORTED, false, (uint16_t)0);
    }

    /* build the path to match. */
    conn_path = slice_from_slice(input, offset, slice_len(input));

    info("path slice:");
    slice_dump(conn_path);

    if(!match_path(conn_path, ((offset & 0x01) ? false : true), &plc->path[0], plc->path_len)) {
        /* FIXME - send back the right error. */
        info("Forward open request path did not match the path for this PLC!");
        return make_cip_error(output, (uint8_t)(slice_get_uint8(input, 0) | CIP_DONE), (uint8_t)CIP_ERR_UNSUPPORTED, false, (uint16_t)0);
    }

    /* check to see how many refusals we should do. */
    if(plc->reject_fo_count > 0) {
        plc->reject_fo_count--;
        info("Forward open request being bounced for debugging. %d to go.", plc->reject_fo_count);
        return make_cip_error(output,
                             (uint8_t)(slice_get_uint8(input, 0) | CIP_DONE),
                             (uint8_t)CIP_ERR_0x01,
                             true,
                             (uint16_t)0x100);
    }

    /* all good if we got here. */
    plc->client_connection_id = fo_req.client_conn_id;
    plc->client_connection_serial_number = fo_req.conn_serial_number;
    plc->client_vendor_id = fo_req.orig_vendor_id;
    plc->client_serial_number = fo_req.orig_serial_number;
    plc->client_to_server_rpi = fo_req.client_to_server_rpi;
    plc->server_to_client_rpi = fo_req.server_to_client_rpi;
    plc->server_connection_id = (uint32_t)rand();
    plc->server_connection_seq = (uint16_t)rand();

    /* store the allowed packet sizes. */
    plc->client_to_server_max_packet = fo_req.client_to_server_conn_params &
                               ((fo_cmd == CIP_FORWARD_OPEN[0]) ? 0x1FF : 0x0FFF);
    plc->server_to_client_max_packet = fo_req.server_to_client_conn_params &
                               ((fo_cmd == CIP_FORWARD_OPEN[0]) ? 0x1FF : 0x0FFF);

    /* FIXME - check that the packet sizes are valid 508 or 4002 */

    /* now process the FO and respond. */
    offset = 0;
    slice_set_uint8(output, offset, (uint8_t)(slice_get_uint8(input, 0) | CIP_DONE)); offset++;
    slice_set_uint8(output, offset, 0); offset++; /* padding/reserved. */
    slice_set_uint8(output, offset, 0); offset++; /* no error. */
    slice_set_uint8(output, offset, 0); offset++; /* no extra error fields. */

    slice_set_uint32_le(output, offset, plc->server_connection_id); offset += 4;
    slice_set_uint32_le(output, offset, plc->client_connection_id); offset += 4;
    slice_set_uint16_le(output, offset, plc->client_connection_serial_number); offset += 2;
    slice_set_uint16_le(output, offset, plc->client_vendor_id); offset += 2;
    slice_set_uint32_le(output, offset, plc->client_serial_number); offset += 4;
    slice_set_uint32_le(output, offset, plc->client_to_server_rpi); offset += 4;
    slice_set_uint32_le(output, offset, plc->server_to_client_rpi); offset += 4;

    /* not sure what these do... */
    slice_set_uint8(output, offset, 0); offset++;
    slice_set_uint8(output, offset, 0); offset++;

    return slice_from_slice(output, 0, offset);
}


/* Forward Close request. */
typedef struct {
    uint8_t secs_per_tick;          /* seconds per tick */
    uint8_t timeout_ticks;          /* timeout = srd_secs_per_tick * src_timeout_ticks */
    uint16_t client_connection_serial_number; /* our connection ID/serial number */
    uint16_t client_vendor_id;      /* our unique vendor ID */
    uint32_t client_serial_number;  /* our unique serial number */
    slice_s path;                   /* path to PLC */
} forward_close_s;

/* the minimal Forward Open with no path */
#define CIP_FORWARD_CLOSE_MIN_SIZE   (16)


slice_s handle_forward_close(slice_s input, slice_s output, plc_s *plc)
{
    slice_s conn_path;
    size_t offset = 0;
    forward_close_s fc_req = {0};

    info("Checking Forward Close request:");
    slice_dump(input);

    /* minimum length check */
    if(slice_len(input) < CIP_FORWARD_CLOSE_MIN_SIZE) {
        /* FIXME - send back the right error. */
        return make_cip_error(output, (uint8_t)(slice_get_uint8(input, 0) | CIP_DONE), (uint8_t)CIP_ERR_UNSUPPORTED, false, (uint16_t)0);
    }

    /* get the data. */
    offset = sizeof(CIP_FORWARD_CLOSE); /* step past the path to the CM */
    fc_req.secs_per_tick = slice_get_uint8(input, offset); offset++;
    fc_req.timeout_ticks = slice_get_uint8(input, offset); offset++;
    fc_req.client_connection_serial_number = slice_get_uint16_le(input, offset); offset += 2;
    fc_req.client_vendor_id = slice_get_uint16_le(input, offset); offset += 2;
    fc_req.client_serial_number = slice_get_uint32_le(input, offset); offset += 4;

    /* check the remaining length */
    if(offset >= slice_len(input)) {
        /* FIXME - send back the right error. */
        info("Forward close request size, %d, too small.   Should be greater than %d!", slice_len(input), offset);
        return make_cip_error(output, slice_get_uint8(input, 0) | CIP_DONE, CIP_ERR_UNSUPPORTED, false, 0);
    }

    /*
     * why does Rockwell do this?   The path here is _NOT_ a byte-for-byte copy of the path
     * that was used to open the connection.  This one is padded with a zero byte after the path
     * length.
     */

    /* build the path to match. */
    conn_path = slice_from_slice(input, offset, slice_len(input));

    if(!match_path(conn_path, ((offset & 0x01) ? false : true), plc->path, plc->path_len)) {
        info("path does not match stored path!");
        return make_cip_error(output, slice_get_uint8(input, 0) | CIP_DONE, CIP_ERR_UNSUPPORTED, false, 0);
    }

    /* Check the values we got. */
    if(plc->client_connection_serial_number != fc_req.client_connection_serial_number) {
        /* FIXME - send back the right error. */
        info("Forward close connection serial number, %x, did not match the connection serial number originally passed, %x!", fc_req.client_connection_serial_number, plc->client_connection_serial_number);
        return make_cip_error(output, slice_get_uint8(input, 0) | CIP_DONE, CIP_ERR_UNSUPPORTED, false, 0);
    }
    if(plc->client_vendor_id != fc_req.client_vendor_id) {
        /* FIXME - send back the right error. */
        info("Forward close client vendor ID, %x, did not match the client vendor ID originally passed, %x!", fc_req.client_vendor_id, plc->client_vendor_id);
        return make_cip_error(output, slice_get_uint8(input, 0) | CIP_DONE, CIP_ERR_UNSUPPORTED, false, 0);
    }
    if(plc->client_serial_number != fc_req.client_serial_number) {
        /* FIXME - send back the right error. */
        info("Forward close client serial number, %x, did not match the client serial number originally passed, %x!", fc_req.client_serial_number, plc->client_serial_number);
        return make_cip_error(output, slice_get_uint8(input, 0) | CIP_DONE, CIP_ERR_UNSUPPORTED, false, 0);
    }

    /* now process the FClose and respond. */
    offset = 0;
    slice_set_uint8(output, offset, slice_get_uint8(input, 0) | CIP_DONE); offset++;
    slice_set_uint8(output, offset, 0); offset++; /* padding/reserved. */
    slice_set_uint8(output, offset, 0); offset++; /* no error. */
    slice_set_uint8(output, offset, 0); offset++; /* no extra error fields. */

    slice_set_uint16_le(output, offset, plc->client_connection_serial_number); offset += 2;
    slice_set_uint16_le(output, offset, plc->client_vendor_id); offset += 2;
    slice_set_uint32_le(output, offset, plc->client_serial_number); offset += 4;

    /* not sure what these do... */
    slice_set_uint8(output, offset, 0); offset++;
    slice_set_uint8(output, offset, 0); offset++;

    return slice_from_slice(output, 0, offset);
}


/*
 * A read request comes in with a symbolic segment first, then zero to three numeric segments.
 */

#define CIP_READ_MIN_SIZE (6)
#define CIP_READ_FRAG_MIN_SIZE (10)

slice_s handle_read_request(slice_s input, slice_s output, plc_s *plc)
{
    uint8_t read_cmd = slice_get_uint8(input, 0);  /*get the type. */
    uint8_t tag_segment_size = 0;
    uint16_t element_count = 0;
    uint32_t byte_offset = 0;
    size_t read_start_offset = 0;
    size_t offset = 0;
    tag_def_s *tag = NULL;
    size_t tag_data_length = 0;
    size_t total_request_size = 0;
    size_t remaining_size = 0;
    size_t packet_capacity = 0;
    bool need_frag = false;
    size_t amount_to_copy = 0;

    /* Omron does not support fragmented read. */
    if(plc->plc_type == PLC_OMRON && read_cmd == CIP_READ_FRAG[0]) {
        info("Omron PLCs do not support fragmented read!");
        return make_cip_error(output, read_cmd | CIP_DONE, CIP_ERR_UNSUPPORTED, false, 0);
    }

    if(slice_len(input) < (read_cmd == CIP_READ[0] ? CIP_READ_MIN_SIZE : CIP_READ_FRAG_MIN_SIZE)) {
        info("Insufficient data in the CIP read request!");
        return make_cip_error(output, read_cmd | CIP_DONE, CIP_ERR_UNSUPPORTED, false, 0);
    }

    offset = 1;
    tag_segment_size = slice_get_uint8(input, offset); offset++;

    /* check that we have enough space. */
    if((slice_len(input) + (read_cmd == CIP_READ[0] ? 2 : 6) - 2) < (tag_segment_size * 2)) {
        info("Request does not have enough space for element count and byte offset!");
        return make_cip_error(output, read_cmd | CIP_DONE, CIP_ERR_UNSUPPORTED, false, 0);
    }

    if(!process_tag_segment(plc, slice_from_slice(input, offset, (size_t)(tag_segment_size * 2)), &tag, &read_start_offset)) {
        return make_cip_error(output, read_cmd | CIP_DONE, CIP_ERR_UNSUPPORTED, false, 0);
    }

    /* step past the tag segment. */
    offset += (size_t)(tag_segment_size * 2);

    element_count = slice_get_uint16_le(input, offset); offset += 2;

    if(plc->plc_type == PLC_OMRON) {
        if(element_count != 1) {
            info("Omron PLC requires element count to be 1, found %d!", element_count);
            return make_cip_error(output, read_cmd | CIP_DONE, CIP_ERR_UNSUPPORTED, false, 0);
        } else {
            /* all good, now fake it with an element count that is the full tag. */
            element_count = (uint16_t)tag->elem_count;
        }
    }

    if(read_cmd == CIP_READ_FRAG[0]) {
        byte_offset = slice_get_uint32_le(input, offset); offset += 4;
    }

    /* double check the size of the request. */
    if(offset != slice_len(input)) {
        info("Request size does not match CIP request size!");
        return make_cip_error(output, read_cmd | CIP_DONE, CIP_ERR_UNSUPPORTED, false, 0);
    }

    /* check the offset bounds. */
    tag_data_length = (size_t)(tag->elem_count * tag->elem_size);

    info("tag_data_length = %d", tag_data_length);

    /* get the amount requested. */
    total_request_size = (size_t)(element_count * tag->elem_size);

    info("total_request_size = %d", total_request_size);

    /* check the amount */
    if(read_start_offset + total_request_size > tag_data_length) {
        info("request asks for too much data!");
        return make_cip_error(output, read_cmd | CIP_DONE, CIP_ERR_EXTENDED, true, CIP_ERR_EX_TOO_LONG);
    }

    /* check to make sure that the offset passed is within the bounds. */
    if(read_start_offset + byte_offset > tag_data_length) {
        info("request offset is past the end of the tag!");
        return make_cip_error(output, read_cmd | CIP_DONE, CIP_ERR_EXTENDED, true, CIP_ERR_EX_TOO_LONG);
    }

    /* do we need to fragment the result? */
    remaining_size = total_request_size - byte_offset;
    packet_capacity = slice_len(output) - 6; /* MAGIC - CIP header plus data type bytes is 6 bytes. */

    info("packet_capacity = %d", packet_capacity);

    if(remaining_size > packet_capacity) {
        need_frag = true;
    } else {
        need_frag = false;
    }

    info("need_frag = %s", need_frag ? "true" : "false");

    /* start making the response. */
    offset = 0;
    slice_set_uint8(output, offset, read_cmd | CIP_DONE); offset++;
    slice_set_uint8(output, offset, 0); offset++; /* padding/reserved. */
    slice_set_uint8(output, offset, (need_frag ? CIP_ERR_FRAG : CIP_OK)); offset++; /* no error. */
    slice_set_uint8(output, offset, 0); offset++; /* no extra error fields. */

    /* copy the data type. */
    slice_set_uint16_le(output, offset, tag->tag_type); offset += 2;

    /* how much data to copy? */
    amount_to_copy = (remaining_size < packet_capacity ? remaining_size : packet_capacity);
    if(amount_to_copy > 8) {
        /* align to 8-byte chunks */
        amount_to_copy &= 0xFFFFC;
    }

    info("amount_to_copy = %d", amount_to_copy);
    info("copy start location = %d", offset);
    info("output space = %d", slice_len(output) - offset);

    /* FIXME - use memcpy */
    for(size_t i=0; i < amount_to_copy; i++) {
        slice_set_uint8(output, offset + i, tag->data[byte_offset + i]);
    }

    offset += amount_to_copy;

    return slice_from_slice(output, 0, offset);
}




#define CIP_WRITE_MIN_SIZE (6)
#define CIP_WRITE_FRAG_MIN_SIZE (10)

slice_s handle_write_request(slice_s input, slice_s output, plc_s *plc)
{
    uint8_t write_cmd = slice_get_uint8(input, 0);  /*get the type. */
    uint8_t tag_segment_size = 0;
    uint32_t byte_offset = 0;
    size_t write_start_offset = 0;
    size_t offset = 0;
    tag_def_s *tag = NULL;
    size_t tag_data_length = 0;
    size_t total_request_size = 0;
    bool need_frag = false;
    uint16_t write_data_type = 0;
    uint16_t write_element_count = 0;

    if(slice_len(input) < (write_cmd == CIP_WRITE[0] ? CIP_WRITE_MIN_SIZE : CIP_WRITE_FRAG_MIN_SIZE)) {
        info("Insufficient data in the CIP write request!");
        return make_cip_error(output, write_cmd | CIP_DONE, CIP_ERR_UNSUPPORTED, false, 0);
    }

    offset = 1;
    tag_segment_size = slice_get_uint8(input, offset); offset++;

    /* check that we have enough space. */
    if((slice_len(input) + (write_cmd == CIP_WRITE[0] ? 2 : 6) - 2) < (tag_segment_size * 2)) {
        info("Request does not have enough space for element count and byte offset!");
        return make_cip_error(output, write_cmd | CIP_DONE, CIP_ERR_UNSUPPORTED, false, 0);
    }

    if(!process_tag_segment(plc, slice_from_slice(input, offset, (size_t)(tag_segment_size * 2)), &tag, &write_start_offset)) {
        return make_cip_error(output, write_cmd | CIP_DONE, CIP_ERR_UNSUPPORTED, false, 0);
    }

    /* step past the tag segment. */
    offset += (size_t)(tag_segment_size * 2);

    /* get the tag data type and compare. */
    write_data_type = slice_get_uint16_le(input, offset); offset += 2;

    /* check that the data types match. */
    if(tag->tag_type != write_data_type) {
        info("tag data type %02x does not match the data type in the write request %02x", tag->tag_type, write_data_type);
        return make_cip_error(output, write_cmd | CIP_DONE, CIP_ERR_UNSUPPORTED, false, 0);
    }

    /* get the number of elements to write. */
    write_element_count = slice_get_uint16_le(input, offset); offset += 2;

    /* check the number of elements */
    if(write_element_count > tag->elem_count) {
        info("request tries to write too many elements!");
        return make_cip_error(output, write_cmd | CIP_DONE, CIP_ERR_EXTENDED, true, CIP_ERR_EX_TOO_LONG);
    }

    if(write_cmd == CIP_WRITE_FRAG[0]) {
        byte_offset = slice_get_uint32_le(input, offset); offset += 4;
    }

    info("byte_offset = %d", byte_offset);

    /* check the offset bounds. */
    tag_data_length = (size_t)(tag->elem_count * tag->elem_size);

    info("tag_data_length = %d", tag_data_length);

    /* get the write amount requested. */
    total_request_size = slice_len(input) - offset;

    info("total_request_size = %d", total_request_size);

    /* check the amount */
    if(byte_offset + total_request_size > tag_data_length) {
        info("request tries to write too much data!");
        return make_cip_error(output, write_cmd | CIP_DONE, CIP_ERR_EXTENDED, true, CIP_ERR_EX_TOO_LONG);
    }

    /* copy the data. */
    info("byte_offset = %d", byte_offset);
    info("offset = %d", offset);
    info("total_request_size = %d", total_request_size);
    memcpy(&tag->data[byte_offset], slice_get_bytes(input, offset), total_request_size);

    /* start making the response. */
    offset = 0;
    slice_set_uint8(output, offset, write_cmd | CIP_DONE); offset++;
    slice_set_uint8(output, offset, 0); offset++; /* padding/reserved. */
    slice_set_uint8(output, offset, (need_frag ? CIP_ERR_FRAG : CIP_OK)); offset++; /* no error. */
    slice_set_uint8(output, offset, 0); offset++; /* no extra error fields. */

    return slice_from_slice(output, 0, offset);
}





/*
 * we should see:
 *  0x91 <name len> <name bytes> (<numeric segment>){0-3}
 *
 * find the tag name, then check the numeric segments, if any, against the
 * tag dimensions.
 */

bool process_tag_segment(plc_s *plc, slice_s input, tag_def_s **tag, size_t *start_read_offset)
{
    size_t offset = 0;
    uint8_t symbolic_marker = slice_get_uint8(input, offset); offset++;
    uint8_t name_len = 0;
    slice_s tag_name;
    size_t dimensions[3] = { 0, 0, 0};
    size_t dimension_index = 0;

    if(symbolic_marker != CIP_SYMBOLIC_SEGMENT_MARKER)  {
        info("Expected symbolic segment but found %x!", symbolic_marker);
        return false;
    }

    /* get and check the length of the symbolic name part. */
    name_len = slice_get_uint8(input, offset); offset++;
    if(name_len >= slice_len(input)) {
        info("Insufficient space in symbolic segment for name.   Needed %d bytes but only had %d bytes!", name_len, slice_len(input)-1);
        return false;
    }

    /* bump the offset.   Must be 16-bit aligned, so pad if needed. */
    offset += (size_t)(name_len + ((name_len & 0x01) ? 1 : 0));

    /* try to find the tag. */
    tag_name = slice_from_slice(input, 2, name_len);
    *tag = plc->tags;

    while(*tag) {
        if(slice_match_string(tag_name, (*tag)->name)) {
            info("Found tag %s", (*tag)->name);
            break;
        }

        (*tag) = (*tag)->next_tag;
    }

    if(*tag) {
        slice_s numeric_segments = slice_from_slice(input, offset, slice_len(input));

        dimension_index = 0;

        info("Numeric segment(s):");
        slice_dump(numeric_segments);

        while(slice_len(numeric_segments) > 0) {
            uint8_t segment_type = slice_get_uint8(numeric_segments, 0);

            if(dimension_index >= 3) {
                info("More numeric segments than expected!   Remaining request:");
                slice_dump(numeric_segments);
                return false;
            }

            switch(segment_type) {
                case 0x28: /* single byte value. */
                    dimensions[dimension_index] = (size_t)slice_get_uint8(numeric_segments, 1);
                    dimension_index++;
                    numeric_segments = slice_from_slice(numeric_segments, 2, slice_len(numeric_segments));
                    break;

                case 0x29: /* two byte value */
                    dimensions[dimension_index] = (size_t)slice_get_uint16_le(numeric_segments, 2);
                    dimension_index++;
                    numeric_segments = slice_from_slice(numeric_segments, 4, slice_len(numeric_segments));
                    break;

                case 0x2A: /* four byte value */
                    dimensions[dimension_index] = (size_t)slice_get_uint32_le(numeric_segments, 2);
                    dimension_index++;
                    numeric_segments = slice_from_slice(numeric_segments, 6, slice_len(numeric_segments));
                    break;

                default:
                    info("Unexpected numeric segment marker %x!", segment_type);
                    return false;
                    break;
            }
        }

        /* calculate the element offset. */
        if(dimension_index > 0) {
            size_t element_offset = 0;

            if(dimension_index != (*tag)->num_dimensions) {
                info("Required %d numeric segments, but only found %d!", (*tag)->num_dimensions, dimension_index);
                return false;
            }

            /* check in bounds. */
            for(size_t i=0; i < dimension_index; i++) {
                if(dimensions[i] >= (*tag)->dimensions[i]) {
                    info("Dimension %d is out of bounds, must be 0 <= %d < %d", (int)i, dimensions[i], (*tag)->dimensions[i]);
                    return false;
                }
            }

            /* calculate the offset. */
            element_offset = (size_t)(dimensions[0] * ((*tag)->dimensions[1] * (*tag)->dimensions[2]) +
                                      dimensions[1] *  (*tag)->dimensions[2] +
                                      dimensions[2]);

            *start_read_offset = (size_t)((*tag)->elem_size * element_offset);
        } else {
            *start_read_offset = 0;
        }
    } else {
        info("Tag %.*s not found!", slice_len(tag_name), (const char *)(tag_name.data));
        return false;
    }



    return true;
}

/* match a path.   This is tricky, thanks, Rockwell. */
bool match_path(slice_s input, bool need_pad, uint8_t *path, uint8_t path_len)
{
    size_t input_len = slice_len(input);
    size_t input_path_len = 0;
    size_t path_start = 0;

    info("Starting with request path:");
    slice_dump(input);
    info("and stored path:");
    slice_dump(slice_make(path, (ssize_t)path_len));

    if(input_len < path_len) {
        info("path does not match lengths.   Input length %zu, path length %zu.", input_len, path_len);
        return false;
    }

    /* the first byte of the path input is the length byte in 16-bit words */
    input_path_len = (size_t)slice_get_uint8(input, 0);

    /* check it against the passed path length */
    if((input_path_len * 2) != path_len) {
        info("path is wrong length.   Got %zu but expected %zu!", input_path_len*2, path_len);
        return false;
    }

    /* where does the path start? */
    if(need_pad) {
        path_start = 2;
    } else {
        path_start = 1;
    }

    info("Comparing slice:");
    slice_dump(slice_from_slice(input, path_start, slice_len(input)));
    info("with slice:");
    slice_dump(slice_make(path, (ssize_t)path_len));

    return slice_match_bytes(slice_from_slice(input, path_start, slice_len(input)), path, path_len);
}



slice_s make_cip_error(slice_s output, uint8_t cip_cmd, uint8_t cip_err, bool extend, uint16_t extended_error)
{
    size_t result_size = 0;

    slice_set_uint8(output, 0, cip_cmd | CIP_DONE);
    slice_set_uint8(output, 1, 0); /* reserved, must be zero. */
    slice_set_uint8(output, 2, cip_err);

    if(extend) {
        slice_set_uint8(output, 3, 2); /* two bytes of extended status. */
        slice_set_uint16_le(output, 4, extended_error);
        result_size = 6;
    } else {
        slice_set_uint8(output, 3, 0); /* no additional bytes of sub-error. */
        result_size = 4;
    }

    return slice_from_slice(output, 0, result_size);
}

