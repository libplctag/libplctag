/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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

#include "cip.h"
#include "eip.h"
#include "pccc.h"
#include "plc.h"
#include "slice.h"
#include "utils.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <utils/random_utils.h>


/* tag commands */
#define CIP_SRV_MULTI ((uint8_t)0x0a)
#define CIP_SRV_PCCC_EXECUTE ((uint8_t)0x4b)
#define CIP_SRV_READ_NAMED_TAG ((uint8_t)0x4c)
#define CIP_SRV_WRITE_NAMED_TAG ((uint8_t)0x4d)
#define CIP_SRV_FORWARD_CLOSE ((uint8_t)0x4e)
#define CIP_SRV_READ_NAMED_TAG_FRAG ((uint8_t)0x52)
#define CIP_SRV_WRITE_NAMED_TAG_FRAG ((uint8_t)0x53)
#define CIP_SRV_FORWARD_OPEN ((uint8_t)0x54)
#define CIP_SRV_INSTANCES_ATTRIBS ((uint8_t)0x55)
#define CIP_SRV_FORWARD_OPEN_EX ((uint8_t)0x5b)


/* non-tag commands */
// 4b 02 20 67 24 01 07 3d f3 45 43 50 21
//  const uint8_t CIP_PCCC_EXECUTE_OBJ[] = { 0x02, 0x20, 0x67, 0x24, 0x01 };
//  const uint8_t CIP_PCCC_PREFIX[] = { 0x07, 0x3d, 0xf3, 0x45, 0x43, 0x50, 0x21 };
//  const uint8_t CIP_FORWARD_CLOSE[] = { 0x4E, 0x02, 0x20, 0x06, 0x24, 0x01 };
const uint8_t CIP_OBJ_CONNECTION_MANAGER[] = {0x20, 0x06, 0x24, 0x01};
// const uint8_t CIP_LIST_TAGS[] = { 0x55, 0x02, 0x20, 0x02, 0x24, 0x01 };
// const uint8_t CIP_FORWARD_OPEN_EX[] = { 0x5B, 0x02, 0x20, 0x06, 0x24, 0x01 };

/* path to match. */
// uint8_t LOGIX_CONN_PATH[] = { 0x03, 0x00, 0x00, 0x20, 0x02, 0x24, 0x01 };
// uint8_t MICRO800_CONN_PATH[] = { 0x02, 0x20, 0x02, 0x24, 0x01 };

#define CIP_DONE ((uint8_t)0x80)

#define CIP_SYMBOLIC_SEGMENT_MARKER ((uint8_t)0x91)

/* CIP Errors */

#define CIP_OK ((uint8_t)0x00)
#define CIP_ERR_EXT_ERR ((uint8_t)0x01)
#define CIP_ERR_INVALID_PARAM ((uint8_t)0x03)
#define CIP_ERR_PATH_SEGMENT ((uint8_t)0x04)
#define CIP_ERR_PATH_DEST_UNKNOWN ((uint8_t)0x05)
#define CIP_ERR_FRAG ((uint8_t)0x06)
#define CIP_ERR_UNSUPPORTED ((uint8_t)0x08)
#define CIP_ERR_INSUFFICIENT_DATA ((uint8_t)0x13)
#define CIP_ERR_TOO_MUCH_DATA ((uint8_t)0x15)
#define CIP_ERR_EXTENDED ((uint8_t)0xff)

#define CIP_ERR_EX_DUPLICATE_CONN ((uint16_t)0x0100)
#define CIP_ERR_EX_INVALID_CONN_SIZE ((uint16_t)0x0109)
#define CIP_ERR_EX_TOO_LONG ((uint16_t)0x2105)

/* tag and object constants */
#define CIP_TAG_PATH_MIN ((size_t)(4)) /* 1 byte segment type, 1 byte length count, 1 byte tag name, 1 byte padding */
#define CIP_OBJ_PATH_MIN ((size_t)(4)) /* 1 byte class type, 1 byte class ID, 1 byte instance type, 1 byte instance ID */

#define CIP_RESPONSE_HEADER_SIZE ((size_t)4)
#define CIP_RESPONSE_HEADER_EXT_ERR_SIZE ((size_t)6)
#define CIP_RESPONSE_TYPE_INFO_SIZE ((size_t)2) /* FIXME - this should come from the tag */
#define CIP_MIN_ATOMIC_ELEMENT_SIZE \
    ((size_t)8) /* size to use if element size of tag is big.  Prevents splitting of atomic values. */

#define CIP_READ_PAYLOAD_MIN_SIZE ((size_t)2)      /* two bytes for the element count */
#define CIP_READ_FRAG_PAYLOAD_MIN_SIZE ((size_t)6) /* two bytes for element count, four for byte offset */

#define CIP_WRITE_PAYLOAD_MIN_SIZE ((size_t)5) /* two bytes for the element count, two bytes for type, 1 byte for data */
#define CIP_WRITE_FRAG_PAYLOAD_MIN_SIZE \
    ((size_t)9) /* two bytes for element count, four for byte offset, two bytes for type, 1 bytes for data */

#define CIP_TAG_MAX_INDEXES ((uint32_t)3) /* should double check for OMRON */


// typedef struct {
//     uint8_t service_code;   /* why is the operation code _before_ the path? */
//     uint8_t path_size;      /* size in 16-bit words of the path */
//     slice_s path;           /* store this in a slice to avoid copying */
// } cip_header_s;


static slice_s make_cip_error(slice_s output, uint8_t cip_cmd, uint8_t cip_err, bool extend, uint16_t extended_error);

static slice_s handle_forward_open(uint8_t cip_service, slice_s cip_service_path, slice_s cip_service_payload, slice_s output,
                                   plc_s *plc);
static slice_s handle_forward_close(uint8_t cip_service, slice_s cip_service_path, slice_s cip_service_payload, slice_s output,
                                    plc_s *plc);
static slice_s handle_read_request(uint8_t cip_service, slice_s cip_service_path, slice_s cip_service_payload, slice_s output,
                                   plc_s *plc);
static slice_s handle_write_request(uint8_t cip_service, slice_s cip_service_path, slice_s cip_service_payload, slice_s output,
                                    plc_s *plc);

static bool parse_cip_request(slice_s input, uint8_t *cip_service, slice_s *cip_service_path, slice_s *cip_service_payload);
static bool extract_cip_path(slice_s input, size_t *offset, bool padded, slice_s *output);
static bool parse_tag_path(slice_s tag_path, plc_s *plc, tag_def_s **tag, uint32_t *num_indexes, uint32_t *indexes);
static bool calculate_request_start_and_end_offsets(tag_def_s *tag, uint32_t num_indexes, uint32_t *indexes,
                                                    uint16_t request_element_count, size_t *request_start_byte_offset,
                                                    size_t *request_end_byte_offset);


slice_s cip_dispatch_request(slice_s input, slice_s output, plc_s *plc) {
    uint8_t cip_service = 0;
    slice_s cip_service_path = {0};
    slice_s cip_service_payload = {0};

    info("Got packet:");
    slice_dump(input);

    if(!parse_cip_request(input, &cip_service, &cip_service_path, &cip_service_payload)) {
        info("Unable to parse CIP request!");
        return make_cip_error(output, cip_service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    info("CIP Service: %02x", cip_service);
    info("CIP Path:");
    slice_dump(cip_service_path);
    info("CIP Payload:");
    slice_dump(cip_service_payload);

    switch(cip_service) {
        case CIP_SRV_FORWARD_OPEN:
        case CIP_SRV_FORWARD_OPEN_EX:
            return handle_forward_open(cip_service, cip_service_path, cip_service_payload, output, plc);
            break;

        case CIP_SRV_FORWARD_CLOSE:
            return handle_forward_close(cip_service, cip_service_path, cip_service_payload, output, plc);
            break;

        case CIP_SRV_READ_NAMED_TAG:
        case CIP_SRV_READ_NAMED_TAG_FRAG:
            return handle_read_request(cip_service, cip_service_path, cip_service_payload, output, plc);
            break;

        case CIP_SRV_WRITE_NAMED_TAG:
        case CIP_SRV_WRITE_NAMED_TAG_FRAG:
            return handle_write_request(cip_service, cip_service_path, cip_service_payload, output, plc);
            break;

        case CIP_SRV_PCCC_EXECUTE: return dispatch_pccc_request(input, output, plc); break;

        default: return make_cip_error(output, cip_service, CIP_ERR_UNSUPPORTED, false, 0); break;
    }
}


/* a handy structure to hold all the parameters we need to receive in a Forward Open request. */
typedef struct {
    uint8_t secs_per_tick;                 /* seconds per tick */
    uint8_t timeout_ticks;                 /* timeout = srd_secs_per_tick * src_timeout_ticks */
    uint32_t server_conn_id;               /* 0, returned by server in reply. */
    uint32_t client_conn_id;               /* sent by client. */
    uint16_t conn_serial_number;           /* client connection ID/serial number */
    uint16_t orig_vendor_id;               /* client unique vendor ID */
    uint32_t orig_serial_number;           /* client unique serial number */
    uint8_t conn_timeout_multiplier;       /* timeout = mult * RPI */
    uint8_t reserved[3];                   /* reserved, set to 0 */
    uint32_t client_to_server_rpi;         /* us to target RPI - Request Packet Interval in microseconds */
    uint32_t client_to_server_conn_params; /* some sort of identifier of what kind of PLC we are??? */
    uint32_t server_to_client_rpi;         /* target to us RPI, in microseconds */
    uint32_t server_to_client_conn_params; /* some sort of identifier of what kind of PLC the target is ??? */
    uint8_t transport_class;               /* ALWAYS 0xA3, server transport, class 3, application trigger */
    slice_s path;                          /* connection path. */
} forward_open_s;

/* the minimal Forward Open with no path */
#define CIP_FORWARD_OPEN_MIN_SIZE (42)
#define CIP_FORWARD_OPEN_EX_MIN_SIZE (46)


slice_s handle_forward_open(uint8_t cip_service, slice_s cip_service_path, slice_s cip_service_payload, slice_s output,
                            plc_s *plc) {
    slice_s path_payload = {0};
    slice_s conn_path_slice = {0};
    size_t offset = 0;
    forward_open_s fo_req = {0};

    if(cip_service == CIP_SRV_FORWARD_OPEN) {
        info("Processing Forward Open request.");
    } else {
        info("Processing Forward Open Extended request.");
    }

    if(!slice_match_data_exact(cip_service_path, CIP_OBJ_CONNECTION_MANAGER, sizeof(CIP_OBJ_CONNECTION_MANAGER))) {
        info("Forward Open service requested from wrong object!");
        slice_dump(cip_service_path);
        return make_cip_error(output, cip_service, CIP_ERR_UNSUPPORTED, false, 0);
    } else {
        info("Forward Open service requested of Connection Manager Object instance 1.");
    }

    /* start parsing */
    offset = 0;

    /* get the data. */
    fo_req.secs_per_tick = slice_get_uint8(cip_service_payload, offset);
    offset++;
    fo_req.timeout_ticks = slice_get_uint8(cip_service_payload, offset);
    offset++;
    fo_req.server_conn_id = slice_get_uint32_le(cip_service_payload, offset);
    offset += 4;
    fo_req.client_conn_id = slice_get_uint32_le(cip_service_payload, offset);
    offset += 4;
    fo_req.conn_serial_number = slice_get_uint16_le(cip_service_payload, offset);
    offset += 2;
    fo_req.orig_vendor_id = slice_get_uint16_le(cip_service_payload, offset);
    offset += 2;
    fo_req.orig_serial_number = slice_get_uint32_le(cip_service_payload, offset);
    offset += 4;
    fo_req.conn_timeout_multiplier = slice_get_uint8(cip_service_payload, offset);
    offset += 4; /* byte plus 3-bytes of padding. */
    fo_req.client_to_server_rpi = slice_get_uint32_le(cip_service_payload, offset);
    offset += 4;

    if(cip_service == CIP_SRV_FORWARD_OPEN) {
        /* old command uses 16-bit value. */
        fo_req.client_to_server_conn_params = slice_get_uint16_le(cip_service_payload, offset);
        offset += 2;
    } else {
        /* new command has 32-bit field here. */
        fo_req.client_to_server_conn_params = slice_get_uint32_le(cip_service_payload, offset);
        offset += 4;
    }

    fo_req.server_to_client_rpi = slice_get_uint32_le(cip_service_payload, offset);
    offset += 4;
    if(cip_service == CIP_SRV_FORWARD_OPEN) {
        /* old command uses 16-bit value. */
        fo_req.server_to_client_conn_params = slice_get_uint16_le(cip_service_payload, offset);
        offset += 2;
    } else {
        /* new command has 32-bit field here. */
        fo_req.server_to_client_conn_params = slice_get_uint32_le(cip_service_payload, offset);
        offset += 4;
    }

    fo_req.transport_class = slice_get_uint8(cip_service_payload, offset);
    offset++;

    /* did we run out of bounds? */
    if(offset > slice_len(cip_service_payload)) {
        info("Not enough Forward Open service data!");
        return make_cip_error(output, cip_service, CIP_ERR_INSUFFICIENT_DATA, false, 0);
    }

    /* Get the connection path */
    path_payload = slice_from_slice(cip_service_payload, offset, slice_len(cip_service_payload));

    info("Forward Open path payload:");
    slice_dump(path_payload);

    offset = 0;
    if(!extract_cip_path(path_payload, &offset, false, &conn_path_slice)) {
        info("Unable to extract the connection path from the Forward Open request!");
        return make_cip_error(output, cip_service, CIP_ERR_PATH_SEGMENT, false, 0);
    }

    info("Connection path slice:");
    slice_dump(conn_path_slice);

    if(!slice_match_data_exact(conn_path_slice, &(plc->path[0]), plc->path_len)) {
        slice_s plc_path = slice_make(&(plc->path[0]), (ssize_t)(size_t)(plc->path_len));

        info("Forward open request path did not match the path for this PLC!");
        info("FO path:");
        slice_dump(conn_path_slice);
        info("PLC path:");
        slice_dump(plc_path);

        return make_cip_error(output, cip_service, CIP_ERR_PATH_DEST_UNKNOWN, false, 0);
    }

    /* check to see how many refusals we should do. */
    if(plc->reject_fo_count > 0) {
        plc->reject_fo_count--;
        info("Forward open request being bounced for debugging. %d to go.", plc->reject_fo_count);
        return make_cip_error(output, cip_service, CIP_ERR_EXT_ERR, true, CIP_ERR_EX_DUPLICATE_CONN);
    }

    /* all good if we got here. */
    plc->client_connection_id = fo_req.client_conn_id;
    plc->client_connection_serial_number = fo_req.conn_serial_number;
    plc->client_vendor_id = fo_req.orig_vendor_id;
    plc->client_serial_number = fo_req.orig_serial_number;
    plc->client_to_server_rpi = fo_req.client_to_server_rpi;
    plc->server_to_client_rpi = fo_req.server_to_client_rpi;
    plc->server_connection_id = (uint32_t)(random_u64(UINT32_MAX) + 1);
    plc->server_connection_seq = (uint16_t)(random_u64(UINT16_MAX) + 1);

    /* store the allowed packet sizes. */
    plc->client_to_server_max_packet =
        fo_req.client_to_server_conn_params & ((cip_service == CIP_SRV_FORWARD_OPEN) ? 0x1FF : 0x0FFF);
    plc->server_to_client_max_packet =
        fo_req.server_to_client_conn_params & ((cip_service == CIP_SRV_FORWARD_OPEN) ? 0x1FF : 0x0FFF);

    /* FIXME - check that the packet sizes are valid 508 or 4002 */

    /* now process the FO and respond. */
    offset = 0;
    slice_set_uint8(output, offset, cip_service | CIP_DONE);
    offset++;
    slice_set_uint8(output, offset, 0);
    offset++; /* padding/reserved. */
    slice_set_uint8(output, offset, 0);
    offset++; /* no error. */
    slice_set_uint8(output, offset, 0);
    offset++; /* no extra error fields. */

    slice_set_uint32_le(output, offset, plc->server_connection_id);
    offset += 4;
    slice_set_uint32_le(output, offset, plc->client_connection_id);
    offset += 4;
    slice_set_uint16_le(output, offset, plc->client_connection_serial_number);
    offset += 2;
    slice_set_uint16_le(output, offset, plc->client_vendor_id);
    offset += 2;
    slice_set_uint32_le(output, offset, plc->client_serial_number);
    offset += 4;
    slice_set_uint32_le(output, offset, plc->client_to_server_rpi);
    offset += 4;
    slice_set_uint32_le(output, offset, plc->server_to_client_rpi);
    offset += 4;

    /* not sure what these do... */
    slice_set_uint8(output, offset, 0);
    offset++;
    slice_set_uint8(output, offset, 0);
    offset++;

    return slice_from_slice(output, 0, offset);
}


/* Forward Close request. */
typedef struct {
    uint8_t secs_per_tick;                    /* seconds per tick */
    uint8_t timeout_ticks;                    /* timeout = srd_secs_per_tick * src_timeout_ticks */
    uint16_t client_connection_serial_number; /* our connection ID/serial number */
    uint16_t client_vendor_id;                /* our unique vendor ID */
    uint32_t client_serial_number;            /* our unique serial number */
    slice_s path;                             /* path to PLC */
} forward_close_s;

/* the minimal Forward Open with no path */
#define CIP_FORWARD_CLOSE_MIN_SIZE (16)


slice_s handle_forward_close(uint8_t cip_service, slice_s cip_service_path, slice_s cip_service_payload, slice_s output,
                             plc_s *plc) {
    slice_s conn_path_slice;
    size_t offset = 0;
    forward_close_s fc_req = {0};

    if(!slice_match_data_exact(cip_service_path, CIP_OBJ_CONNECTION_MANAGER, sizeof(CIP_OBJ_CONNECTION_MANAGER))) {
        info("Forward Open service requested from wrong object!");
        slice_dump(cip_service_path);
        return make_cip_error(output, cip_service, CIP_ERR_UNSUPPORTED, false, 0);
    } else {
        info("Forward Close service requested of Connection Manager Object instance 1.");
    }

    /* minimum length check */
    if(slice_len(cip_service_payload) < CIP_FORWARD_CLOSE_MIN_SIZE) {
        /* FIXME - send back the right error. */
        return make_cip_error(output, cip_service, (uint8_t)CIP_ERR_UNSUPPORTED, false, (uint16_t)0);
    }

    /* get the data. */
    offset = 0;

    fc_req.secs_per_tick = slice_get_uint8(cip_service_payload, offset);
    offset++;
    fc_req.timeout_ticks = slice_get_uint8(cip_service_payload, offset);
    offset++;
    fc_req.client_connection_serial_number = slice_get_uint16_le(cip_service_payload, offset);
    offset += 2;
    fc_req.client_vendor_id = slice_get_uint16_le(cip_service_payload, offset);
    offset += 2;
    fc_req.client_serial_number = slice_get_uint32_le(cip_service_payload, offset);
    offset += 4;

    /* check the remaining length */
    if(offset >= slice_len(cip_service_payload)) {
        /* FIXME - send back the right error. */
        info("Forward close request size, %d, too small.   Should be greater than %d!", slice_len(cip_service_payload), offset);
        return make_cip_error(output, cip_service, CIP_ERR_INSUFFICIENT_DATA, false, 0);
    }

    /*
     * why does Rockwell do this?   The path here is _NOT_ a byte-for-byte copy of the path
     * that was used to open the connection.  This one is padded with a zero byte after the path
     * length.
     */

    if(!extract_cip_path(cip_service_payload, &offset, true, &conn_path_slice)) {
        info("Unable to extract the connection path from the Forward Close request!");
        return make_cip_error(output, cip_service, CIP_ERR_PATH_SEGMENT, false, 0);
    }

    info("Connection path slice:");
    slice_dump(conn_path_slice);

    if(!slice_match_data_exact(conn_path_slice, &(plc->path[0]), plc->path_len)) {
        slice_s plc_path = slice_make(&(plc->path[0]), (ssize_t)(size_t)(plc->path_len));

        info("Forward Cpen request path did not match the path for this PLC!");
        info("FC path:");
        slice_dump(conn_path_slice);
        info("PLC path:");
        slice_dump(plc_path);

        return make_cip_error(output, cip_service, CIP_ERR_PATH_DEST_UNKNOWN, false, 0);
    }

    /* Check the values we got. */
    if(plc->client_connection_serial_number != fc_req.client_connection_serial_number) {
        /* FIXME - send back the right error. */
        info("Forward close connection serial number, %x, did not match the connection serial number originally passed, %x!",
             fc_req.client_connection_serial_number, plc->client_connection_serial_number);
        return make_cip_error(output, cip_service, CIP_ERR_INVALID_PARAM, false, 0);
    }
    if(plc->client_vendor_id != fc_req.client_vendor_id) {
        /* FIXME - send back the right error. */
        info("Forward Close client vendor ID, %x, did not match the client vendor ID originally passed, %x!",
             fc_req.client_vendor_id, plc->client_vendor_id);
        return make_cip_error(output, cip_service, CIP_ERR_INVALID_PARAM, false, 0);
    }
    if(plc->client_serial_number != fc_req.client_serial_number) {
        /* FIXME - send back the right error. */
        info("Forward close client serial number, %x, did not match the client serial number originally passed, %x!",
             fc_req.client_serial_number, plc->client_serial_number);
        return make_cip_error(output, cip_service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    /* now process the FClose and respond. */
    offset = 0;
    slice_set_uint8(output, offset, cip_service | CIP_DONE);
    offset++;
    slice_set_uint8(output, offset, 0);
    offset++; /* padding/reserved. */
    slice_set_uint8(output, offset, 0);
    offset++; /* no error. */
    slice_set_uint8(output, offset, 0);
    offset++; /* no extra error fields. */

    slice_set_uint16_le(output, offset, plc->client_connection_serial_number);
    offset += 2;
    slice_set_uint16_le(output, offset, plc->client_vendor_id);
    offset += 2;
    slice_set_uint32_le(output, offset, plc->client_serial_number);
    offset += 4;

    /* not sure what these do... */
    slice_set_uint8(output, offset, 0);
    offset++;
    slice_set_uint8(output, offset, 0);
    offset++;

    return slice_from_slice(output, 0, offset);
}


slice_s handle_read_request(uint8_t cip_service, slice_s cip_service_path, slice_s cip_service_payload, slice_s output,
                            plc_s *plc) {
    size_t required_request_payload_size = 0;
    tag_def_s *tag = NULL;
    uint32_t num_indexes = CIP_TAG_MAX_INDEXES;
    uint32_t indexes[CIP_TAG_MAX_INDEXES] = {0};
    size_t parse_offset = 0;
    uint16_t request_element_count = 0;
    size_t request_fragment_start_byte_offset = 0;
    size_t request_start_byte_offset = 0;
    size_t request_end_byte_offset = 0;
    size_t min_data_element_size = 0;
    slice_s cip_response_header_slice = {0};
    slice_s cip_response_type_info_slice = {0};
    slice_s cip_response_payload_slice = {0};
    size_t copy_size = 0;
    uint8_t cip_err = CIP_OK;
    bool needs_fragmentation = false;

    /* what service are we handling? */
    if(cip_service == CIP_SRV_READ_NAMED_TAG) {
        info("Processing Read Named Tag request.");
        required_request_payload_size = CIP_READ_PAYLOAD_MIN_SIZE;
    } else {
        info("Processing Read Named Tag Fragmented request.");
        required_request_payload_size = CIP_READ_FRAG_PAYLOAD_MIN_SIZE;
    }

    /* OMRON only supports un-fragmented reads. */
    if(plc->plc_type == PLC_OMRON && cip_service != CIP_SRV_READ_NAMED_TAG) {
        info("Omron PLCs do not support fragmented read CIP service!");
        return make_cip_error(output, cip_service, CIP_ERR_UNSUPPORTED, false, 0);
    }

    /* check the payload size */
    if(slice_len(cip_service_payload) < required_request_payload_size) {
        info("Insufficient data in the CIP read request payload!");
        return make_cip_error(output, cip_service, CIP_ERR_INSUFFICIENT_DATA, false, 0);
    }

    /* try to get the tag and indexes from the tag path. */
    if(!parse_tag_path(cip_service_path, plc, &tag, &num_indexes, &(indexes[0]))) {
        info("Unable to parse tag path:");
        slice_dump(cip_service_path);
        return make_cip_error(output, cip_service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    /* get the element count and the optional request byte offset. */
    parse_offset = 0;
    request_element_count = slice_get_uint16_le(cip_service_payload, parse_offset);
    parse_offset += 2;

    /* optionally get the request byte offset */
    if(cip_service == CIP_SRV_READ_NAMED_TAG_FRAG) {
        request_fragment_start_byte_offset = slice_get_uint32_le(cip_service_payload, parse_offset);
        parse_offset += 4;
    }

    if(parse_offset != slice_len(cip_service_payload)) {
        info("Extra data in the CIP read request payload!");
        slice_dump(cip_service_payload);
        return make_cip_error(output, cip_service, CIP_ERR_TOO_MUCH_DATA, false, 0);
    }

    /* get the starting offset of the request, checks against tag size. */
    if(!calculate_request_start_and_end_offsets(tag, num_indexes, indexes, request_element_count, &request_start_byte_offset,
                                                &request_end_byte_offset)) {
        info("Unable to calculate the starting offset of the read request!");
        // FIXME - need other error.
        return make_cip_error(output, cip_service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    /* bump the start offset by the amount we may have already read. */
    request_start_byte_offset += request_fragment_start_byte_offset;

    /* check the byte offsets */
    if(request_start_byte_offset > request_end_byte_offset) {
        info("Invalid byte offsets in the read request!");
        return make_cip_error(output, cip_service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    /* what is the minimum amount of data we can send back without breaking an atomic base type? */
    // FIXME - does this actually work?
    if(tag->elem_size > CIP_MIN_ATOMIC_ELEMENT_SIZE) {
        min_data_element_size = CIP_MIN_ATOMIC_ELEMENT_SIZE;
    } else {
        min_data_element_size = tag->elem_size;
    }

    /* check the payload space.  */
    if(slice_len(output) < (CIP_RESPONSE_HEADER_SIZE + CIP_RESPONSE_TYPE_INFO_SIZE + min_data_element_size)) {
        info("Insufficient space in the output buffer for the response!");
        return make_cip_error(output, cip_service, CIP_ERR_FRAG, false, 0);
    }

    /* peel off space for the header and type info and payload. */
    cip_response_header_slice = slice_from_slice(output, 0, CIP_RESPONSE_HEADER_SIZE);
    cip_response_type_info_slice = slice_from_slice(output, CIP_RESPONSE_HEADER_SIZE, CIP_RESPONSE_TYPE_INFO_SIZE);
    cip_response_payload_slice = slice_from_slice(output, CIP_RESPONSE_HEADER_SIZE + CIP_RESPONSE_TYPE_INFO_SIZE,
                                                  slice_len(output) - (CIP_RESPONSE_HEADER_SIZE + CIP_RESPONSE_TYPE_INFO_SIZE));

    /* make sure we have enough space for at least one element. */
    if(slice_len(cip_response_payload_slice) < min_data_element_size) {
        info("Insufficient space in the output buffer for the response payload!");
        // FIXME - need other error.
        return make_cip_error(output, cip_service, CIP_ERR_EXTENDED, true, CIP_ERR_EX_TOO_LONG);
    }

    /* we have payload space so how much can we copy? */
    copy_size = slice_len(cip_response_payload_slice);

    /* find the largest multiple of the min_data_element_size count that fits. */
    copy_size = (copy_size / min_data_element_size) * min_data_element_size;

    /* make sure we don't copy too much. */
    if(copy_size > request_end_byte_offset - request_start_byte_offset) {
        copy_size = request_end_byte_offset - request_start_byte_offset;
    }

    /* copy the data into the response payload. */
    critical_block(tag->data_mutex) {
        if(!slice_copy_data_in(cip_response_payload_slice, tag->data + request_start_byte_offset, copy_size)) {
            info("Unable to copy the data into the response payload!");
            // FIXME - need other error.
            cip_err = CIP_ERR_INVALID_PARAM;
            break;
        }
    }

    if(cip_err != CIP_OK) { return make_cip_error(output, cip_service, cip_err, false, 0); }

    /* did we fragment? */
    if(request_start_byte_offset + copy_size < request_end_byte_offset) {
        info("Need to fragment read request.");
        needs_fragmentation = true;
    }

    /* fill in the CIP response header. */
    slice_set_uint8(cip_response_header_slice, 0, cip_service | CIP_DONE);
    slice_set_uint8(cip_response_header_slice, 1, 0);                                             /* reserved */
    slice_set_uint8(cip_response_header_slice, 2, (needs_fragmentation ? CIP_ERR_FRAG : CIP_OK)); /* status */
    slice_set_uint8(cip_response_header_slice, 3, 0);                                             /* no extended error */

    /* fill in the tag data type */
    slice_set_uint16_le(cip_response_type_info_slice, 0, tag->tag_type);

    /* return the slice used */
    return slice_from_slice(output, 0,
                            slice_len(cip_response_header_slice) + slice_len(cip_response_type_info_slice) + copy_size);
}


#define CIP_WRITE_MIN_SIZE (6)
#define CIP_WRITE_FRAG_MIN_SIZE (10)


slice_s handle_write_request(uint8_t cip_service, slice_s cip_service_path, slice_s cip_service_payload, slice_s output,
                             plc_s *plc) {
    size_t required_request_payload_size = 0;
    tag_def_s *tag = NULL;
    uint32_t num_indexes = CIP_TAG_MAX_INDEXES;
    uint32_t indexes[CIP_TAG_MAX_INDEXES] = {0};
    size_t parse_offset = 0;
    uint16_t request_element_type = 0;
    uint16_t request_element_count = 0;
    size_t request_fragment_start_byte_offset = 0;
    size_t request_start_byte_offset = 0;
    size_t request_end_byte_offset = 0;
    uint8_t cip_err = CIP_OK;
    slice_s write_request_payload_slice = {0};
    slice_s cip_response_header_slice = {0};

    /* what service are we handling? */
    if(cip_service == CIP_SRV_WRITE_NAMED_TAG) {
        info("Processing Write Named Tag request.");
        required_request_payload_size = CIP_WRITE_PAYLOAD_MIN_SIZE;
    } else {
        info("Processing Write Named Tag Fragmented request.");
        required_request_payload_size = CIP_WRITE_FRAG_PAYLOAD_MIN_SIZE;
    }

    /* OMRON only supports un-fragmented reads. */
    if(plc->plc_type == PLC_OMRON && cip_service != CIP_SRV_WRITE_NAMED_TAG) {
        info("Omron PLCs do not support fragmented write CIP service!");
        return make_cip_error(output, cip_service, CIP_ERR_UNSUPPORTED, false, 0);
    }

    /* we need the tag to do more calculations and checks */
    if(!parse_tag_path(cip_service_path, plc, &tag, &num_indexes, &(indexes[0]))) {
        info("Unable to parse tag path:");
        slice_dump(cip_service_path);
        return make_cip_error(output, cip_service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    /* are the number of indexes correct? */
    if(num_indexes > 0 && num_indexes != tag->num_dimensions) {
        info("Wrong number of indexes passed.   Must be zero or %zu indexes.", tag->num_dimensions);
        return make_cip_error(output, cip_service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    /* check the payload size */
    if(slice_len(cip_service_payload) < required_request_payload_size) {
        info("Insufficient data in the CIP read request payload!");
        return make_cip_error(output, cip_service, CIP_ERR_INSUFFICIENT_DATA, false, 0);
    }

    request_element_type = slice_get_uint16_le(cip_service_payload, parse_offset);
    parse_offset += 2;

    if(request_element_type != tag->tag_type) {
        info("Request element type does not match tag type!");
        return make_cip_error(output, cip_service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    /* get the element count and the optional request byte offset. */
    request_element_count = slice_get_uint16_le(cip_service_payload, parse_offset);
    parse_offset += 2;

    /* optionally get the request byte offset for this fragment */
    if(cip_service == CIP_SRV_WRITE_NAMED_TAG_FRAG) {
        request_fragment_start_byte_offset = (size_t)slice_get_uint32_le(cip_service_payload, parse_offset);
        parse_offset += 4;
    }

    /* get a slice for our payload data to write */
    write_request_payload_slice =
        slice_from_slice(cip_service_payload, parse_offset, slice_len(cip_service_payload) - parse_offset);

    /* TODO - check the amount of data to write and make sure it does not partially write a primitive/atomic value. */

    /* get the starting offset of the request, and checks tag size. */
    if(!calculate_request_start_and_end_offsets(tag, num_indexes, indexes, request_element_count, &request_start_byte_offset,
                                                &request_end_byte_offset)) {
        info("Unable to calculate the starting or ending offset of the write request!");
        // FIXME - need other error.
        return make_cip_error(output, cip_service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    /* what is the actual starting byte offset for this specific request which might be a fragment. */
    request_start_byte_offset += request_fragment_start_byte_offset;

    /* Make sure we are not trying to write too much. */
    if((slice_len(write_request_payload_slice) + request_start_byte_offset) > request_end_byte_offset) {
        info("Too much data in the write request!");
        return make_cip_error(output, cip_service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    critical_block(tag->data_mutex) {
        if(!slice_copy_data_out(tag->data + request_start_byte_offset, request_end_byte_offset - request_start_byte_offset,
                                write_request_payload_slice)) {
            info("Unable to copy the data into the tag!");
            cip_err = CIP_ERR_INVALID_PARAM;
            break;
        }
    }

    if(cip_err != CIP_OK) { return make_cip_error(output, cip_service, cip_err, false, 0); }

    /* peel off space for the header */
    cip_response_header_slice = slice_from_slice(output, 0, CIP_RESPONSE_HEADER_SIZE);

    /* fill in the CIP response header. */
    slice_set_uint8(cip_response_header_slice, 0, cip_service | CIP_DONE);
    slice_set_uint8(cip_response_header_slice, 1, 0);      /* reserved */
    slice_set_uint8(cip_response_header_slice, 2, CIP_OK); /* status */
    slice_set_uint8(cip_response_header_slice, 3, 0);      /* no extended error */

    /* peel off space for the header */
    cip_response_header_slice = slice_from_slice(output, 0, CIP_RESPONSE_HEADER_SIZE);

    /* fill in the CIP response header. */
    slice_set_uint8(cip_response_header_slice, 0, cip_service | CIP_DONE);
    slice_set_uint8(cip_response_header_slice, 1, 0); /* reserved */
    slice_set_uint8(cip_response_header_slice, 2, CIP_OK); /* status */
    slice_set_uint8(cip_response_header_slice, 3, 0); /* no extended error */

    /* return the remaining output space */
    return cip_response_header_slice;
}


slice_s make_cip_error(slice_s output, uint8_t cip_cmd, uint8_t cip_err, bool extend, uint16_t extended_error) {
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


#define CIP_MIN_TAG_PATH_SIZE 2

bool parse_tag_path(slice_s tag_path, plc_s *plc, tag_def_s **tag, uint32_t *num_indexes, uint32_t *indexes) {
    size_t offset = 0;
    uint8_t name_len = 0;
    uint8_t segment_marker = 0;
    slice_s tag_name_slice = {0};
    uint32_t max_indexes = *num_indexes; /* contains the max possible */

    /* Check if the tag path is long enough to contain the segment marker and name length */
    if(slice_len(tag_path) < CIP_MIN_TAG_PATH_SIZE) {
        info("Tag path is too short to contain segment marker and name length!");
        return false;
    }

    /* Get the segment marker */
    segment_marker = slice_get_uint8(tag_path, offset);
    offset++;
    if(segment_marker != CIP_SYMBOLIC_SEGMENT_MARKER) {
        info("Expected symbolic segment marker but found %x!", segment_marker);
        return false;
    }

    /* Get the name length */
    name_len = slice_get_uint8(tag_path, offset);
    offset++;
    if(name_len + offset > slice_len(tag_path)) {
        info("Name length %d exceeds remaining tag path length %d!", name_len, slice_len(tag_path) - offset);
        return false;
    }

    /* Extract the tag name slice */
    tag_name_slice = slice_from_slice(tag_path, offset, name_len);
    offset += name_len;

    /* Align to 16-bit boundary if necessary */
    if(offset % 2 != 0) { offset++; }

    /* find the tag */
    *tag = plc->tags;

    while(*tag) {
        if(slice_match_string_exact(tag_name_slice, (*tag)->name)) {
            info("Found tag %s", (*tag)->name);
            break;
        }

        (*tag) = (*tag)->next_tag;
    }

    if(!*tag) {
        info("Tag %.*s not found!", slice_len(tag_name_slice), (const char *)(tag_name_slice.data));
        return false;
    }

    /* Initialize the number of indexes to zero */
    *num_indexes = 0;

    /* Parse the numeric segments (indexes) */
    while(offset < slice_len(tag_path)) {
        uint8_t segment_type = slice_get_uint8(tag_path, offset);

        switch(segment_type) {
            case 0x28:        // Single byte value
                offset += 1;  // skip segment type
                indexes[*num_indexes] = (uint32_t)slice_get_uint8(tag_path, offset);
                info("Numeric segment: %u", indexes[*num_indexes]);
                offset += 1;
                break;

            case 0x29:        // Two byte value
                offset += 2;  // skip segment type and padding
                indexes[*num_indexes] = (uint32_t)slice_get_uint16_le(tag_path, offset);
                info("Numeric segment: %u", indexes[*num_indexes]);
                offset += 2;
                break;

            case 0x2A:        // Four byte value
                offset += 2;  // skip segment type and padding
                indexes[*num_indexes] = (uint32_t)slice_get_uint32_le(tag_path, offset);
                info("Numeric segment: %u", indexes[*num_indexes]);
                offset += 4;
                break;

            default:
                info("Unexpected numeric segment marker %x at position %zu!", segment_type, offset);
                return false;
                break;
        }

        (*num_indexes)++;

        if(*num_indexes > max_indexes) {
            info("More numeric segments, %zu, than expected, %zu!", *num_indexes, max_indexes);
            return false;
        }
    }

    /* the only valid number of indexes is zero or the number of dimensions in the tag. */
    if(*num_indexes != 0 && *num_indexes != (*tag)->num_dimensions) {
        info("Required zero or %zu numeric segments, but only found %zu!", (*tag)->num_dimensions, (size_t)*num_indexes);
        return false;
    }

    return true;
}


bool extract_cip_path(slice_s input, size_t *offset, bool padded, slice_s *output) {
    uint8_t path_len = 0;

    /* Check if the input slice is long enough to contain the path length and path */
    if(slice_len(input) < ((*offset) + (padded ? 2 : 1))) {
        info("CIP path is too short to contain path length and path!");
        return false;
    }

    /* Get the path length */
    path_len = slice_get_uint8(input, (*offset));
    (*offset)++;
    if(path_len == 0) {
        info("CIP path length is zero!");
        return false;
    }

    if(padded) { *offset += 1; /* skip the padding byte */ }

    /* Check if the input slice is long enough to contain the path */
    if(slice_len(input) < ((*offset) + (path_len * 2))) {
        info("CIP path is too short to contain the specified path length %u!", path_len * 2);
        return false;
    }

    /* return the slice with the path data. */
    *output = slice_from_slice(input, *offset, path_len * 2);

    /* update the offset to past the path. */
    *offset += path_len * 2;

    return true;
}


/* we assume that the number of indexes matches that of the tag or is zero */

bool calculate_request_start_and_end_offsets(tag_def_s *tag, uint32_t num_indexes, uint32_t *indexes,
                                             uint16_t request_element_count, size_t *request_start_byte_offset,
                                             size_t *request_end_byte_offset) {
    size_t total_elements = 1;
    size_t tag_size = 0;
    size_t element_offset = 0;

    /* Calculate the total number of elements in the tag */
    total_elements = 1;
    for(uint32_t i = 0; i < tag->num_dimensions; i++) { total_elements *= tag->dimensions[i]; }

    tag_size = total_elements * tag->elem_size;

    /* check index bounds */
    for(size_t index = 0; index < num_indexes; index++) {
        if(indexes[index] >= tag->dimensions[index]) {
            info("Index %zu out of bounds for dimension %zu.", (size_t)indexes[index], index);
            return false;
        }
    }

    /* calculate the linear element offset */
    switch(num_indexes) {
        case 0: element_offset = 0; break;

        case 1: element_offset = indexes[0]; break;

        case 2: element_offset = (indexes[0] * tag->dimensions[1]) + indexes[1]; break;

        case 3:
            element_offset =
                (indexes[0] * tag->dimensions[1] * tag->dimensions[2]) + (indexes[1] * tag->dimensions[2]) + indexes[2];
            break;

        default:
            info("Too many indexes!");
            return false;
            break;
    }

    /* probably not needed, but just in case */
    if(element_offset >= total_elements) {
        info("Element offset %d exceeds total elements %d!", element_offset, total_elements);
        return false;
    }

    /*
     * Calculate the start and end byte offsets.
     * Note that these are absolute for the whole request.
     * Not for the fragment if there is one.
     */
    *request_start_byte_offset = element_offset * tag->elem_size;
    *request_end_byte_offset = *request_start_byte_offset + (request_element_count * tag->elem_size);

    info("Request start byte offset: %d", *request_start_byte_offset);
    info("Request end byte offset: %d", *request_end_byte_offset);

    /* Check if the start offset exceeds the total size of the tag */
    if(*request_start_byte_offset > tag_size) {
        info("Request start byte offset %d exceeds total tag size %zu", (size_t)*request_start_byte_offset, tag_size);
        return false;
    }

    /* Check if the end offset exceeds the total size of the tag */
    if(*request_end_byte_offset > tag_size) {
        info("Request end byte offset %d exceeds total tag size %zu", (size_t)*request_end_byte_offset, tag_size);
        return false;
    }

    return true;
}

#define CIP_MIN_REQUEST_SIZE \
    4 /*  1 byte for service, 1 byte for service path length in word, 2 bytes for minimal service path. */

bool parse_cip_request(slice_s input, uint8_t *cip_service, slice_s *cip_service_path, slice_s *cip_service_payload) {
    size_t offset = 0;

    /* Check if the input slice is long enough to contain the service code and path size */
    if(slice_len(input) < CIP_MIN_REQUEST_SIZE) {
        info("CIP request is too short to contain service code and path size!");
        return false;
    }

    /* Get the service code */
    *cip_service = slice_get_uint8(input, offset);
    offset++;

    /* extract the service path slice */
    if(!extract_cip_path(input, &offset, false, cip_service_path)) {
        info("Unable to extract CIP service path!");
        return false;
    }

    /* Extract the service payload slice */
    *cip_service_payload = slice_from_slice(input, offset, slice_len(input) - offset);

    return true;
}
