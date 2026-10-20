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

#include "cip.h"
#include "omron.h"
#include "eip.h"
#include "fault.h"
#include "pccc.h"
#include "plc.h"
#include "utils.h"
#include "log.h"
#include <libplctag/api/libplctag.h>
#include <utils/byte_slice.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <utils/random_utils.h>


/* tag and object constants */

/*
 * Encoded type size is per-tag now (tag_def_s.type_info_size): two bytes for an atomic type,
 * four for a structure such as the Logix STRING UDT.
 */
#define CIP_MIN_ATOMIC_ELEMENT_SIZE \
    ((size_t)8) /* size to use if element size of tag is big.  Prevents splitting of atomic values. */

#define CIP_TAG_MAX_INDEXES ((uint32_t)3) /* should double check for OMRON */

#define MAX_SUB_PACKETS ((uint16_t)1000) /* maximum number of sub-packets in a multi-service request */

#define CIP_MINIMAL_RESPONSE_SIZE ((int32_t)6) /* four bytes for header plus 2 for optional extended status. */



static bool parse_tag_path(byte_slice_t path, plc_s *plc, tag_def_s **tag, uint32_t *num_indexes, uint32_t *indexes);
static bool calculate_request_start_and_end_offsets(tag_def_s *tag, uint32_t num_indexes, uint32_t *indexes,
                                                    uint16_t request_element_count, size_t *request_start_byte_offset,
                                                    size_t *request_end_byte_offset);


/*
 * Service codes repeat across classes -- 0x4E is Forward Close on the Connection Manager and
 * Read-Modify-Write on a tag, 0x52 is Unconnected Send and Read Tag Fragmented -- but never
 * within one.
 */
typedef struct {
    uint8_t service;
    bool unwrapped_only; /* the Connection Manager services */
    byte_slice_t (*handler)(cip_request_t *req, byte_slice_t out, plc_s *plc);
} cip_service_t;

typedef struct {
    uint16_t class_id;
    const cip_service_t *services;
    size_t service_count;
} cip_class_t;

static byte_slice_t identity_get_attributes_all(cip_request_t *req, byte_slice_t out, plc_s *plc);
static byte_slice_t forward_open(cip_request_t *req, byte_slice_t out, plc_s *plc);
static byte_slice_t forward_close(cip_request_t *req, byte_slice_t out, plc_s *plc);
static byte_slice_t unconnected_send(cip_request_t *req, byte_slice_t out, plc_s *plc);
static byte_slice_t pccc_execute(cip_request_t *req, byte_slice_t out, plc_s *plc);
static byte_slice_t omron_read_write(cip_request_t *req, byte_slice_t out, plc_s *plc);
static byte_slice_t multiple_service(cip_request_t *req, byte_slice_t out, plc_s *plc);
static byte_slice_t read_tag(cip_request_t *req, byte_slice_t out, plc_s *plc);
static byte_slice_t read_tag_data(cip_request_t *req, byte_slice_t out, plc_s *plc, tag_def_s *tag, uint32_t num_indexes,
                                  uint32_t *indexes, uint16_t element_count, uint32_t fragment_offset);
static byte_slice_t write_tag(cip_request_t *req, byte_slice_t out, plc_s *plc);
static byte_slice_t read_modify_write(cip_request_t *req, byte_slice_t out, plc_s *plc);
static void record_latency(tag_def_s *tag, int64_t start_us);

static const cip_service_t identity_services[] = {
    {CIP_SRV_GET_ATTRIBUTES_ALL, false, identity_get_attributes_all},
};

static const cip_service_t message_router_services[] = {
    {CIP_SRV_MULTI, false, multiple_service},
};

static const cip_service_t connection_manager_services[] = {
    {CIP_SRV_FORWARD_CLOSE, true, forward_close},
    {CIP_SRV_UNCONNECTED_SEND, true, unconnected_send},
    {CIP_SRV_FORWARD_OPEN, true, forward_open},
    {CIP_SRV_FORWARD_OPEN_EX, true, forward_open},
};

static const cip_service_t pccc_services[] = {
    {CIP_SRV_PCCC_EXECUTE, false, pccc_execute},
};

static const cip_service_t symbol_services[] = {
    {CIP_SRV_READ_NAMED_TAG, false, read_tag},
    {CIP_SRV_WRITE_NAMED_TAG, false, write_tag},
    {CIP_SRV_RMW, false, read_modify_write},
    {CIP_SRV_READ_NAMED_TAG_FRAG, false, read_tag},
    {CIP_SRV_WRITE_NAMED_TAG_FRAG, false, write_tag},
};

/* Omron variables add Get_Attributes_All and the simple data segment forms of read and write. */
static const cip_service_t omron_symbol_services[] = {
    {CIP_SRV_GET_ATTRIBUTES_ALL, false, omron_variable_get_attributes_all},
    {CIP_SRV_READ_NAMED_TAG, false, omron_read_write},
    {CIP_SRV_WRITE_NAMED_TAG, false, omron_read_write},
    {CIP_SRV_RMW, false, read_modify_write},
    {CIP_SRV_READ_NAMED_TAG_FRAG, false, read_tag},
    {CIP_SRV_WRITE_NAMED_TAG_FRAG, false, write_tag},
};

static const cip_service_t omron_tag_name_server_services[] = {
    {CIP_SRV_OMRON_GET_INSTANCE_LIST, false, omron_get_instance_list},
};

static const cip_service_t omron_variable_type_services[] = {
    {CIP_SRV_GET_ATTRIBUTES_ALL, false, omron_variable_type_get_attributes_all},
};

#define CIP_CLASS_ENTRY(id, services) {(id), (services), sizeof(services) / sizeof((services)[0])}

static const cip_class_t standard_classes[] = {
    CIP_CLASS_ENTRY(CIP_CLASS_IDENTITY, identity_services),
    CIP_CLASS_ENTRY(CIP_CLASS_MESSAGE_ROUTER, message_router_services),
    CIP_CLASS_ENTRY(CIP_CLASS_CONNECTION_MANAGER, connection_manager_services),
    CIP_CLASS_ENTRY(CIP_CLASS_PCCC, pccc_services),
    CIP_CLASS_ENTRY(CIP_CLASS_SYMBOL, symbol_services),
};

static const cip_class_t omron_classes[] = {
    CIP_CLASS_ENTRY(CIP_CLASS_IDENTITY, identity_services),
    CIP_CLASS_ENTRY(CIP_CLASS_MESSAGE_ROUTER, message_router_services),
    CIP_CLASS_ENTRY(CIP_CLASS_CONNECTION_MANAGER, connection_manager_services),
    CIP_CLASS_ENTRY(CIP_CLASS_PCCC, pccc_services),
    CIP_CLASS_ENTRY(CIP_CLASS_SYMBOL, omron_symbol_services),
    CIP_CLASS_ENTRY(CIP_CLASS_OMRON_TAG_NAME_SERVER, omron_tag_name_server_services),
    CIP_CLASS_ENTRY(CIP_CLASS_OMRON_VARIABLE_TYPE, omron_variable_type_services),
};

static uint8_t read_request(byte_slice_t in, cip_request_t *req);
static uint8_t find_service(plc_s *plc, cip_request_t *req, const cip_service_t **service);


byte_slice_t cip_dispatch_request(byte_slice_t in, byte_slice_t out, plc_s *plc, bool unwrapped) {
    cip_request_t req = {0};
    const cip_service_t *service = NULL;
    uint8_t status = CIP_OK;

    log_info("Got packet:");
    log_info_byte_slice(in);

    req.unwrapped = unwrapped;

    if((status = read_request(in, &req)) == CIP_OK) { status = find_service(plc, &req, &service); }

    if(status != CIP_OK) { return cip_write_reply_header(out, req.service, status, false, 0); }

    log_info("CIP service %02x on class %02x instance %u.", req.service, req.class_id, req.instance);

    return service->handler(&req, out, plc);
}


/* the service, path and payload, and the class and instance the path addresses. */
static uint8_t read_request(byte_slice_t in, cip_request_t *req) {
    uint8_t path_words = 0;
    uint8_t segment = 0;
    uint8_t id8 = 0;
    uint16_t id16 = 0;
    byte_slice_t path = {0};

    in = byte_slice_read_u8(in, &req->service);
    in = byte_slice_read_u8(in, &path_words);
    req->payload = byte_slice_take(in, (int32_t)path_words * 2, &req->path);

    if(byte_slice_has_error(req->path) || path_words == 0) {
        log_info("Unable to parse CIP request!");
        return CIP_ERR_INVALID_PARAM;
    }

    path = byte_slice_read_u8(req->path, &segment);

    switch(segment) {
        case CIP_SYMBOLIC_SEGMENT_MARKER: req->class_id = CIP_CLASS_SYMBOL; return CIP_OK;

        case CIP_SEG_CLASS_8:
            path = byte_slice_read_u8(path, &id8);
            req->class_id = id8;
            break;

        case CIP_SEG_CLASS_16:
            path = byte_slice_read_u16_le(byte_slice_skip(path, 1), &req->class_id);
            break;

        default: log_info("Unsupported first path segment %02x!", segment); return CIP_ERR_PATH_SEGMENT;
    }

    /* the instance is optional. */
    if(byte_slice_len(path) >= 2) {
        byte_slice_t rest = byte_slice_read_u8(path, &segment);

        if(segment == CIP_SEG_INSTANCE_8) {
            path = byte_slice_read_u8(rest, &id8);
            req->instance = id8;
        } else if(segment == CIP_SEG_INSTANCE_16) {
            path = byte_slice_read_u16_le(byte_slice_skip(rest, 1), &id16);
            req->instance = id16;
        }
    }

    /* ponytail: a second instance or an attribute after the class/instance is legal CIP; none is supported. */
    if(byte_slice_has_error(byte_slice_end(path))) {
        log_info("Truncated logical segment, or unsupported segments after the class and instance, in the CIP path!");
        return CIP_ERR_PATH_SEGMENT;
    }

    req->path = path;

    return CIP_OK;
}


static uint8_t find_service(plc_s *plc, cip_request_t *req, const cip_service_t **service) {
    const cip_class_t *classes = standard_classes;
    size_t class_count = sizeof(standard_classes) / sizeof(standard_classes[0]);

    if(plc->plc_type == PLC_OMRON) {
        classes = omron_classes;
        class_count = sizeof(omron_classes) / sizeof(omron_classes[0]);
    }

    for(size_t i = 0; i < class_count; i++) {
        if(classes[i].class_id != req->class_id) { continue; }

        for(size_t j = 0; j < classes[i].service_count; j++) {
            if(classes[i].services[j].service != req->service) { continue; }

            if(classes[i].services[j].unwrapped_only && !req->unwrapped) {
                log_info("Service %02x on class %02x is only accepted straight in SendRRData!", req->service, req->class_id);
                return CIP_ERR_UNSUPPORTED;
            }

            *service = &classes[i].services[j];
            return CIP_OK;
        }

        log_info("Class %02x does not support service %02x!", req->class_id, req->service);
        return CIP_ERR_UNSUPPORTED;
    }

    log_info("No class %02x here!", req->class_id);
    return CIP_ERR_PATH_DEST_UNKNOWN;
}


/*
 * Unconnected Send carries another request, which is dispatched again as unwrapped = false, so
 * it cannot nest.  The payload is priority/tick, timeout ticks, the message length, the message,
 * a pad byte if the length is odd, the route path size in words, a reserved byte and the path.
 */
static byte_slice_t unconnected_send(cip_request_t *req, byte_slice_t out, plc_s *plc) {
    uint16_t message_len = 0;
    uint8_t route_words = 0;
    byte_slice_t message = {0};
    byte_slice_t in = byte_slice_skip(req->payload, 2);

    in = byte_slice_read_u16_le(in, &message_len);
    in = byte_slice_take(in, message_len, &message);

    if(byte_slice_has_error(in)) {
        log_info("CIP service payload too short for embedded service length");
        return cip_write_reply_header(out, req->service, CIP_ERR_INSUFFICIENT_DATA, false, 0);
    }

    /* A ControlLogix router rejects a request whose layout does not add up with 0x01/0x0205, so this does too. */
    in = byte_slice_skip(in, message_len & 1);
    in = byte_slice_read_u8(in, &route_words);
    in = byte_slice_skip(in, 1 + (int32_t)route_words * 2);

    if(byte_slice_has_error(byte_slice_end(in))) {
        log_info("Unconnected Send route path does not follow the embedded message where it should!");
        return cip_write_reply_header(out, req->service, CIP_ERR_EXT_ERR, true, CIP_ERR_EX_UC_SEND_PARAM);
    }

    log_info("Unconnected Send: embedded CIP service length %u", message_len);

    return cip_dispatch_request(message, out, plc, false);
}


static byte_slice_t pccc_execute(cip_request_t *req, byte_slice_t out, plc_s *plc) {
    return pccc_dispatch_request(req->service, req->payload, out, plc);
}


/* an Omron read or write with a Simple Data Segment after the name; anything else is the plain form. */
static byte_slice_t omron_read_write(cip_request_t *req, byte_slice_t out, plc_s *plc) {
    tag_def_s *tag = NULL;
    uint8_t segment = 0;
    byte_slice_t rest = cip_find_symbol_tag(req->path, plc, &tag);

    byte_slice_read_u8(rest, &segment); /* zero if nothing follows the name */

    if(tag && segment == OMRON_SIMPLE_DATA_SEGMENT) { return omron_segment_access(req, tag, rest, out); }

    return req->service == CIP_SRV_READ_NAMED_TAG ? read_tag(req, out, plc) : write_tag(req, out, plc);
}


uint8_t cip_input_status(byte_slice_t failed) {
    return byte_slice_get_error(failed) == PLCTAG_ERR_TOO_LARGE ? CIP_ERR_TOO_MUCH_DATA : CIP_ERR_INSUFFICIENT_DATA;
}


/* a CIP reply header: the service with the reply bit, the status and optional extended status. */
byte_slice_t cip_write_reply_header(byte_slice_t out, uint8_t service, uint8_t status, bool extend, uint16_t extended_status) {
    out = byte_slice_write_u8(out, (uint8_t)(service | CIP_DONE));
    out = byte_slice_write_u8(out, 0); /* reserved */
    out = byte_slice_write_u8(out, status);

    if(!extend) { return byte_slice_write_u8(out, 0); }

    out = byte_slice_write_u8(out, 1); /* one word of extended status */
    return byte_slice_write_u16_le(out, extended_status);
}


/*
 * The Identity object's attributes, one set per emulated PLC.
 *
 * A real device reports these out of flash.  Here they only have to be plausible, distinct
 * and stable: distinct so a test can tell one simulated PLC from another, stable so the
 * expected values can be written down.  The ControlLogix, PLC/5 and MicroLogix entries copy
 * the 1756-L81E, the PLC-5/30 behind a 1785-ENET and the 1763-L16BWA on the bench, serial
 * numbers aside, so a classifier tested here is tested on what those PLCs send.  Note the
 * MicroLogix reports itself as a communications adapter, not a PLC, and the PLC/5 name is the
 * ENET module's and ends in a space.
 */
typedef struct {
    uint16_t vendor_id;
    uint16_t device_type;
    uint16_t product_code;
    uint8_t revision_major;
    uint8_t revision_minor;
    uint16_t status;
    uint32_t serial_number;
    const char *product_name;
} cip_identity_s;

#define CIP_VENDOR_ROCKWELL ((uint16_t)0x0001)
#define CIP_VENDOR_OMRON ((uint16_t)0x002F)
#define CIP_DEVICE_TYPE_PLC ((uint16_t)0x000E)
#define CIP_DEVICE_TYPE_COMM_ADAPTER ((uint16_t)0x000C)
#define CIP_IDENTITY_STATUS_OWNED ((uint16_t)0x0060)

/* indexed by plc_type_t. */
static const cip_identity_s cip_identities[] = {
    [PLC_CONTROL_LOGIX] = {CIP_VENDOR_ROCKWELL, CIP_DEVICE_TYPE_PLC, 0x00A4, 31, 11, 0x3060, 0x00C01756,
                           "1756-L81E/B"},
    [PLC_MICRO800] = {CIP_VENDOR_ROCKWELL, CIP_DEVICE_TYPE_PLC, 0x00C8, 12, 11, CIP_IDENTITY_STATUS_OWNED, 0x00C02080,
                      "2080-LC50-24QWB"},
    [PLC_OMRON] = {CIP_VENDOR_OMRON, CIP_DEVICE_TYPE_PLC, 0x008E, 1, 3, CIP_IDENTITY_STATUS_OWNED, 0x00C00102,
                   "NX102-9000"},
    [PLC_PLC5] = {CIP_VENDOR_ROCKWELL, CIP_DEVICE_TYPE_PLC, 0x0012, 3, 11, CIP_IDENTITY_STATUS_OWNED, 0x00C01785,
                  "PLC-5/30 C/K - 1785-ENET 2.17 "},
    [PLC_SLC] = {CIP_VENDOR_ROCKWELL, CIP_DEVICE_TYPE_PLC, 0x0039, 10, 0, CIP_IDENTITY_STATUS_OWNED, 0x00C01747, "1747-L553"},
    [PLC_MICROLOGIX] = {CIP_VENDOR_ROCKWELL, CIP_DEVICE_TYPE_COMM_ADAPTER, 0x00B9, 2, 12, 0x0064, 0x00C01763,
                        "1763-L16BWA B/12.00"},
    /* a ControlLogix reached through the PCCC mapping is still a ControlLogix. */
    [PLC_LGX_PCCC] = {CIP_VENDOR_ROCKWELL, CIP_DEVICE_TYPE_PLC, 0x00A4, 31, 11, 0x3060, 0x00C01756,
                      "1756-L81E/B"},
};


/*
 * Handle Get_Attributes_All (service 0x01) on the Identity object.
 *
 * This is what an @identity tag asks for.  With no path the request arrives unrouted and is
 * answered by whatever owns the gateway address; with a path it arrives wrapped in an
 * Unconnected Send, which the caller has already unwrapped.  Both reach here, and the reply
 * is the same either way -- the framing is the connection's business, not this object's.
 */
static byte_slice_t identity_get_attributes_all(cip_request_t *req, byte_slice_t out, plc_s *plc) {
    byte_slice_t rest = {0};

    if(req->instance != CIP_IDENTITY_INSTANCE) {
        log_info("Get_Attributes_All is only supported on the Identity object, class 1 instance 1!");
        return cip_write_reply_header(out, req->service, CIP_ERR_PATH_DEST_UNKNOWN, false, 0);
    }

    rest = cip_encode_identity(cip_write_reply_header(out, req->service, CIP_OK, false, 0), plc);

    if(byte_slice_has_error(rest)) { return cip_write_reply_header(out, req->service, CIP_ERR_INSUFFICIENT_DATA, false, 0); }

    return rest;
}


byte_slice_t cip_encode_identity(byte_slice_t out, plc_s *plc) {
    const cip_identity_s *identity = NULL;
    size_t name_len = 0;

    if((size_t)plc->plc_type >= (sizeof(cip_identities) / sizeof(cip_identities[0]))) {
        log_info("No identity is defined for PLC type %d!", (int)plc->plc_type);
        return byte_slice_fail(out, PLCTAG_ERR_NOT_FOUND);
    }

    identity = &cip_identities[plc->plc_type];
    name_len = strlen(identity->product_name);

    out = byte_slice_write_u16_le(out, identity->vendor_id);
    out = byte_slice_write_u16_le(out, identity->device_type);
    out = byte_slice_write_u16_le(out, identity->product_code);
    out = byte_slice_write_u8(out, identity->revision_major);
    out = byte_slice_write_u8(out, identity->revision_minor);
    out = byte_slice_write_u16_le(out, identity->status);
    out = byte_slice_write_u32_le(out, identity->serial_number);

    /* the product name is a SHORT_STRING: a one byte count, then that many bytes, no padding. */
    out = byte_slice_write_u8(out, (uint8_t)name_len);
    out = byte_slice_write_bytes(out, (const uint8_t *)identity->product_name, (int32_t)name_len);

    log_info("Identity: vendor %u, product code %u, revision %u.%u, serial %08x, name \"%s\".",
             (unsigned int)identity->vendor_id, (unsigned int)identity->product_code, (unsigned int)identity->revision_major,
             (unsigned int)identity->revision_minor, (unsigned int)identity->serial_number, identity->product_name);

    return out;
}


/*
 * Multiple Service Packet (0x0A) on Message Router instance 1.  The payload is a count, then
 * an offset per request measured from the count word, then the requests; the reply has the
 * same shape.  Each sub-reply leaves room for a minimal reply per request still to come.
 */
static byte_slice_t multiple_service(cip_request_t *req, byte_slice_t out, plc_s *plc) {
    uint16_t count = 0;
    uint16_t start = 0;
    uint16_t end = 0;
    uint8_t status = CIP_OK;
    byte_slice_t offsets = {0};
    byte_slice_t header = {0};
    byte_slice_t reply_offsets = {0};
    byte_slice_t replies = {0};
    int32_t payload_len = byte_slice_len(req->payload);

    if(req->instance != 1) {
        log_info("Multiple Service requested of Message Router instance %u!", req->instance);
        return cip_write_reply_header(out, req->service, CIP_ERR_PATH_DEST_UNKNOWN, false, 0);
    }

    offsets = byte_slice_read_u16_le(req->payload, &count);
    byte_slice_cut(offsets, (int32_t)count * 2, &offsets, NULL);

    if(byte_slice_has_error(offsets)) {
        log_info("Multiple Service payload is too short for its offset table!");
        return cip_write_reply_header(out, req->service, CIP_ERR_INSUFFICIENT_DATA, false, 0);
    }

    /* the simulator suite greps for this line to see that the client packed its requests */
    log_info("Multi-service request contains %u services", count);

    if(count == 0 || count > MAX_SUB_PACKETS) {
        log_info("Multiple Service count %u is out of range!", count);
        return cip_write_reply_header(out, req->service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    replies = byte_slice_take(out, 6 + (int32_t)count * 2, &header);
    reply_offsets = byte_slice_skip(header, 6);

    if(byte_slice_len(replies) < (int32_t)count * CIP_MINIMAL_RESPONSE_SIZE) {
        log_info("No room for the Multiple Service reply!");
        return cip_write_reply_header(out, req->service, CIP_ERR_INSUFFICIENT_DATA, false, 0);
    }

    offsets = byte_slice_read_u16_le(offsets, &start);

    for(uint16_t i = 0; i < count; i++) {
        byte_slice_t request = {0};
        byte_slice_t room = {0};
        byte_slice_t rest = {0};
        byte_slice_t reply = {0};

        end = (uint16_t)payload_len;

        if(i + 1 < count) { offsets = byte_slice_read_u16_le(offsets, &end); }

        if(start >= end || end > payload_len) {
            log_info("Multiple Service request %u spans %u to %u of %d bytes!", i, start, end, payload_len);
            return cip_write_reply_header(out, req->service, CIP_ERR_INVALID_PARAM, false, 0);
        }

        byte_slice_cut(byte_slice_skip(req->payload, start), end - start, &request, NULL);
        byte_slice_cut(replies, byte_slice_len(replies) - (int32_t)(count - i - 1) * CIP_MINIMAL_RESPONSE_SIZE, &room, NULL);

        rest = cip_dispatch_request(request, room, plc, false);

        /* ponytail: no room for even an error reply gives an empty sub-reply, as before */
        reply = byte_slice_has_error(rest) ? byte_slice_make(byte_slice_get_data(room), 0) : byte_slice_used(room, rest);

        if(byte_slice_len(reply) > 2 && byte_slice_get_data(reply)[2] != CIP_OK) { status = CIP_ERR_EMBEDDED_SERVICE; }

        reply_offsets = byte_slice_write_u16_le(reply_offsets, (uint16_t)(byte_slice_get_data(replies) - byte_slice_get_data(header) - 4));
        replies = byte_slice_skip(replies, byte_slice_len(reply));
        start = end;
    }

    header = cip_write_reply_header(header, req->service, status, false, 0);
    byte_slice_write_u16_le(header, count);

    return replies;
}


#define CIP_CONNECTED_FRAMING_SIZE ((uint32_t)(24 + 20)) /* EIP header, then CPF framing of a connected send */

static byte_slice_t write_size_rejection(byte_slice_t out, uint8_t service, uint16_t supported_size);


/*
 * Forward Open (0x54) and Large Forward Open (0x5B).  They differ only in the two connection
 * parameter fields, 16 bits wide in the first and 32 in the second.
 */
static byte_slice_t forward_open(cip_request_t *req, byte_slice_t out, plc_s *plc) {
    bool large = (req->service == CIP_SRV_FORWARD_OPEN_EX);
    int32_t params_size = large ? 4 : 2;
    uint32_t conn_size_mask = large ? 0x0FFF : 0x1FF;
    uint32_t client_conn_id = 0;
    uint16_t conn_serial_number = 0;
    uint16_t vendor_id = 0;
    uint32_t serial_number = 0;
    uint32_t client_to_server_rpi = 0;
    uint64_t client_to_server_params = 0;
    uint32_t server_to_client_rpi = 0;
    uint64_t server_to_client_params = 0;
    uint8_t path_words = 0;
    byte_slice_t path = {0};
    byte_slice_t in = req->payload;

    log_info("Processing %s request.", large ? "Large Forward Open" : "Forward Open");
    log_info_byte_slice(in);

    if(req->instance != 1) {
        log_info("Forward Open requested of Connection Manager instance %u!", req->instance);
        return cip_write_reply_header(out, req->service, CIP_ERR_UNSUPPORTED, false, 0);
    }

    in = byte_slice_skip(in, 2 + 4); /* priority/tick, timeout ticks, and the connection ID the server picks */
    in = byte_slice_read_u32_le(in, &client_conn_id);
    in = byte_slice_read_u16_le(in, &conn_serial_number);
    in = byte_slice_read_u16_le(in, &vendor_id);
    in = byte_slice_read_u32_le(in, &serial_number);
    in = byte_slice_skip(in, 4); /* timeout multiplier and three reserved bytes */
    in = byte_slice_read_u32_le(in, &client_to_server_rpi);
    in = byte_slice_read_uint(in, params_size, false, &client_to_server_params);
    in = byte_slice_read_u32_le(in, &server_to_client_rpi);
    in = byte_slice_read_uint(in, params_size, false, &server_to_client_params);
    in = byte_slice_skip(in, 1); /* transport class and trigger */

    if(byte_slice_has_error(in)) {
        log_info("Not enough Forward Open service data!");
        return cip_write_reply_header(out, req->service, CIP_ERR_INSUFFICIENT_DATA, false, 0);
    }

    in = byte_slice_read_u8(in, &path_words);
    in = byte_slice_take(in, (int32_t)path_words * 2, &path);

    if(byte_slice_has_error(in) || path_words == 0) {
        log_info("Unable to extract the connection path from the Forward Open request!");
        return cip_write_reply_header(out, req->service, CIP_ERR_PATH_SEGMENT, false, 0);
    }

    if(byte_slice_len(in) != 0) {
        log_info("Forward Open request has %d bytes after the path!", byte_slice_len(in));
        return cip_write_reply_header(out, req->service, CIP_ERR_TOO_MUCH_DATA, false, 0);
    }

    if(byte_slice_compare(path, byte_slice_make(plc->path, plc->path_len), INT32_MAX) != 0) {
        log_info("Forward Open path did not match the path for this PLC!");
        log_info_byte_slice(path);
        return cip_write_reply_header(out, req->service, CIP_ERR_PATH_DEST_UNKNOWN, false, 0);
    }

    /*
     * The debugging refusals.  Their counters are shared (via pointer) across every
     * connection's copy of the plc struct, so each is a single atomic op rather than a
     * check-then-decrement: two connections racing in with a count at 1 must not both see
     * ">0" and both bounce.  atomic_dec_int32 returns the value from before the decrement.
     * ponytail: once exhausted a counter keeps decrementing into negative territory on every
     * later Forward Open; harmless since nothing else reads it, add a CAS-at-zero floor if
     * that ever needs to change.
     */
    {
        int32_t reject_size_remaining = atomic_dec_int32(plc->reject_size_count);

        if(reject_size_remaining > 0) {
            log_info("Rejecting the connection size for debugging, offering %u bytes. %d to go.", plc->reject_size_supported,
                     reject_size_remaining - 1);
            return write_size_rejection(out, req->service, plc->reject_size_supported);
        }
    }

    {
        int32_t offline_remaining = atomic_dec_int32(plc->link_offline_count);

        if(offline_remaining > 0) {
            log_info("Forward open refused, network link offline, for debugging. %d to go.", offline_remaining - 1);
            return cip_write_reply_header(out, req->service, CIP_ERR_EXT_ERR, true, CIP_ERR_EX_LINK_OFFLINE);
        }
    }

    {
        int32_t reject_remaining = atomic_dec_int32(plc->dup_conn_id_tries);

        if(reject_remaining > 0) {
            log_info("Forward open request being bounced for debugging. %d to go.", reject_remaining - 1);
            return cip_write_reply_header(out, req->service, CIP_ERR_EXT_ERR, true, CIP_ERR_EX_DUPLICATE_CONN);
        }
    }

    plc->client_connection_id = client_conn_id;
    plc->client_connection_serial_number = conn_serial_number;
    plc->client_vendor_id = vendor_id;
    plc->client_serial_number = serial_number;
    plc->client_to_server_rpi = client_to_server_rpi;
    plc->server_to_client_rpi = server_to_client_rpi;
    plc->server_connection_id = (uint32_t)(random_u64(UINT32_MAX) + 1);

    /*
     * Store the allowed packet sizes.  The connection size is the connected data -- the 2-byte
     * sequence count and the CIP message -- but the limit here covers the whole EIP packet, so
     * add the 24-byte EIP header and the 20 bytes of interface handle, timeout and CPF items.
     * ponytail: sizes are not checked against what a real PLC allows (508 or 4002).
     */
    plc->client_to_server_max_packet = CIP_CONNECTED_FRAMING_SIZE + ((uint32_t)client_to_server_params & conn_size_mask);
    plc->server_to_client_max_packet = CIP_CONNECTED_FRAMING_SIZE + ((uint32_t)server_to_client_params & conn_size_mask);

    out = cip_write_reply_header(out, req->service, CIP_OK, false, 0);
    out = byte_slice_write_u32_le(out, plc->server_connection_id);
    out = byte_slice_write_u32_le(out, client_conn_id);
    out = byte_slice_write_u16_le(out, conn_serial_number);
    out = byte_slice_write_u16_le(out, vendor_id);
    out = byte_slice_write_u32_le(out, serial_number);
    out = byte_slice_write_u32_le(out, client_to_server_rpi);
    out = byte_slice_write_u32_le(out, server_to_client_rpi);
    out = byte_slice_write_u8(out, 0);  /* application reply size */
    return byte_slice_write_u8(out, 0); /* reserved */
}


/*
 * A Forward Open rejection that says "that connection size is not supported, but this one is".
 *
 * The layout matters to the client: general status 0x01 marks an extended error, the size
 * byte counts 16-bit words of extended status, and the client reads the extended status and
 * the supported size from the four bytes that follow it.  Real PLCs use this to negotiate a
 * client down to a size they can actually serve.
 */
static byte_slice_t write_size_rejection(byte_slice_t out, uint8_t service, uint16_t supported_size) {
    out = byte_slice_write_u8(out, (uint8_t)(service | CIP_DONE));
    out = byte_slice_write_u8(out, 0); /* reserved */
    out = byte_slice_write_u8(out, CIP_ERR_EXT_ERR);
    out = byte_slice_write_u8(out, 2); /* two words of extended status */
    out = byte_slice_write_u16_le(out, CIP_ERR_EX_INVALID_CONN_SIZE);
    return byte_slice_write_u16_le(out, supported_size);
}


/*
 * Forward Close: priority/tick, timeout ticks, the connection serial number, vendor ID and
 * originator serial number, then the connection path.  Rockwell pads the path size with a zero
 * byte here, so it is not a byte-for-byte copy of the Forward Open path.
 */
static byte_slice_t forward_close(cip_request_t *req, byte_slice_t out, plc_s *plc) {
    uint16_t conn_serial_number = 0;
    uint16_t vendor_id = 0;
    uint32_t serial_number = 0;
    uint8_t path_words = 0;
    byte_slice_t path = {0};
    byte_slice_t in = byte_slice_skip(req->payload, 2);

    if(req->instance != 1) {
        log_info("Forward Close requested of Connection Manager instance %u!", req->instance);
        return cip_write_reply_header(out, req->service, CIP_ERR_UNSUPPORTED, false, 0);
    }

    in = byte_slice_read_u16_le(in, &conn_serial_number);
    in = byte_slice_read_u16_le(in, &vendor_id);
    in = byte_slice_read_u32_le(in, &serial_number);
    in = byte_slice_read_u8(in, &path_words);
    in = byte_slice_skip(in, 1);
    in = byte_slice_take(in, (int32_t)path_words * 2, &path);

    if(byte_slice_has_error(in) || path_words == 0) {
        log_info("Forward Close request is too short!");
        return cip_write_reply_header(out, req->service, CIP_ERR_INSUFFICIENT_DATA, false, 0);
    }

    if(byte_slice_len(in) != 0) {
        log_info("Forward Close request has %d bytes after the path!", byte_slice_len(in));
        return cip_write_reply_header(out, req->service, CIP_ERR_TOO_MUCH_DATA, false, 0);
    }

    if(byte_slice_compare(path, byte_slice_make(plc->path, plc->path_len), INT32_MAX) != 0) {
        log_info("Forward Close path did not match the path for this PLC!");
        log_info_byte_slice(path);
        return cip_write_reply_header(out, req->service, CIP_ERR_PATH_DEST_UNKNOWN, false, 0);
    }

    if(conn_serial_number != plc->client_connection_serial_number || vendor_id != plc->client_vendor_id
       || serial_number != plc->client_serial_number) {
        log_info("Forward Close connection %x/%x/%x does not match the open one, %x/%x/%x!", conn_serial_number, vendor_id,
                 serial_number, plc->client_connection_serial_number, plc->client_vendor_id, plc->client_serial_number);
        return cip_write_reply_header(out, req->service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    out = cip_write_reply_header(out, req->service, CIP_OK, false, 0);
    out = byte_slice_write_u16_le(out, conn_serial_number);
    out = byte_slice_write_u16_le(out, vendor_id);
    out = byte_slice_write_u32_le(out, serial_number);
    out = byte_slice_write_u8(out, 0); /* application reply size */
    return byte_slice_write_u8(out, 0); /* reserved */
}


/*
 * Read Tag (0x4C) takes an element count; Read Tag Fragmented (0x52) adds a byte offset.  The
 * reply is the header, the tag's type info and as much data as fits, in whole elements, with
 * status 0x06 if more remains.
 */
static byte_slice_t read_tag(cip_request_t *req, byte_slice_t out, plc_s *plc) {
    int64_t start_us = util_time_us();
    byte_slice_t in = req->payload;
    tag_def_s *tag = NULL;
    uint32_t num_indexes = CIP_TAG_MAX_INDEXES;
    uint32_t indexes[CIP_TAG_MAX_INDEXES] = {0};
    uint16_t element_count = 0;
    uint32_t fragment_offset = 0;
    byte_slice_t rest = {0};

    log_info("Processing Read Named Tag%s request.", req->service == CIP_SRV_READ_NAMED_TAG_FRAG ? " Fragmented" : "");

    /* OMRON only supports un-fragmented reads. */
    if(plc->plc_type == PLC_OMRON && req->service != CIP_SRV_READ_NAMED_TAG) {
        log_info("Omron PLCs do not support fragmented read CIP service!");
        return cip_write_reply_header(out, req->service, CIP_ERR_UNSUPPORTED, false, 0);
    }

    in = byte_slice_read_u16_le(in, &element_count);
    if(req->service == CIP_SRV_READ_NAMED_TAG_FRAG) { in = byte_slice_read_u32_le(in, &fragment_offset); }

    if(byte_slice_has_error(in)) {
        log_info("Insufficient data in the CIP read request payload!");
        return cip_write_reply_header(out, req->service, CIP_ERR_INSUFFICIENT_DATA, false, 0);
    }

    if(!parse_tag_path(req->path, plc, &tag, &num_indexes, &(indexes[0]))) {
        log_info("Unable to parse tag path:");
        log_info_byte_slice(req->path);
        return cip_write_reply_header(out, req->service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    if(byte_slice_len(in) != 0) {
        log_info("Extra data in the CIP read request payload!");
        log_info_byte_slice(req->payload);
        return cip_write_reply_header(out, req->service, CIP_ERR_TOO_MUCH_DATA, false, 0);
    }

    /* the offsets of a variable-length tag depend on its data, which a write can resize. */
    critical_block(tag->data_mutex) {
        rest = read_tag_data(req, out, plc, tag, num_indexes, indexes, element_count, fragment_offset);
    }

    if(!byte_slice_has_error(rest)) { record_latency(tag, start_us); }

    return rest;
}


/* the rest of read_tag, under the tag's data lock. */
static byte_slice_t read_tag_data(cip_request_t *req, byte_slice_t out, plc_s *plc, tag_def_s *tag, uint32_t num_indexes,
                                  uint32_t *indexes, uint16_t element_count, uint32_t fragment_offset) {
    size_t start = 0;
    size_t end = 0;
    size_t min_element_size = 0;
    size_t copy_size = 0;
    byte_slice_t header = {0};
    byte_slice_t data = {0};
    bool partial = false;
    bool endless_fragmentation = false;

    /* get the starting offset of the request, checks against tag size. */
    if(!calculate_request_start_and_end_offsets(tag, num_indexes, indexes, element_count, &start, &end)) {
        log_info("Unable to calculate the starting offset of the read request!");
        return cip_write_reply_header(out, req->service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    /*
     * Fault injection: answer every read with a partial-transfer status and a full payload, no
     * matter how much the client has already taken.  See FAULT_ENDLESS_FRAG.
     *
     * A fragmenting client keeps raising its offset, which would run off the end of the tag and
     * be rejected below, so ignore the offset.  The content is not the point here; the endless
     * stream is.  An Omron client never sends an offset at all, so for that one this only has
     * to force the status.
     */
    endless_fragmentation = fault_fires(plc, FAULT_ENDLESS_FRAG);

    if(!endless_fragmentation) { start += fragment_offset; }

    if(start > end) {
        log_info("Invalid byte offsets in the read request!");
        return cip_write_reply_header(out, req->service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    /*
     * An Omron never trims or fragments a reply.  If all that was asked for does not fit, it
     * refuses the read, and the client must ask only for what fits.  The fault injection still
     * forces a partial status, because that is what it is for.
     */
    if(plc->plc_type == PLC_OMRON && !endless_fragmentation
       && (size_t)byte_slice_len(out) < CIP_RESPONSE_HEADER_SIZE + tag->type_info_size + (end - start)) {
        log_info("Omron read does not fit in one reply.");
        return cip_write_reply_header(out, req->service, CIP_ERR_REPLY_TOO_LARGE, false, 0);
    }

    /* the smallest piece that does not split an atomic value; for strings, an empty string. */
    if(tag->variable_elements) {
        min_element_size = tag->count_word_bytes;
    } else if(tag->elem_size > CIP_MIN_ATOMIC_ELEMENT_SIZE) {
        min_element_size = CIP_MIN_ATOMIC_ELEMENT_SIZE;
    } else {
        min_element_size = tag->elem_size;
    }

    if((size_t)byte_slice_len(out) < CIP_RESPONSE_HEADER_SIZE + tag->type_info_size + min_element_size) {
        /* Rockwell sends a partial-transfer status with no data; an Omron refuses the read. */
        log_info("Insufficient space in the output buffer for the response!");
        return cip_write_reply_header(out, req->service, plc->plc_type == PLC_OMRON ? CIP_ERR_REPLY_TOO_LARGE : CIP_ERR_FRAG,
                                  false, 0);
    }

    /* debugging: answer with a partial-transfer status and no data at all.  See the note on
     * plc_s.empty_frag_count -- this is the shape a real PLC produces when packed requests
     * leave no room, and it makes no forward progress. */
    {
        int32_t empty_frag_remaining = atomic_dec_int32(plc->empty_frag_count);

        if(empty_frag_remaining > 0) {
            log_info("Returning an empty fragment response for debugging. %d to go.", empty_frag_remaining - 1);
            return cip_write_reply_header(out, req->service, CIP_ERR_FRAG, false, 0);
        }
    }

    data = byte_slice_take(out, (int32_t)CIP_RESPONSE_HEADER_SIZE, &header);
    data = byte_slice_write_bytes(data, tag->type_info, (int32_t)tag->type_info_size);

    /* the largest number of whole elements that fits, but no more than was asked for. */
    copy_size = ((size_t)byte_slice_len(data) / min_element_size) * min_element_size;
    if(copy_size > end - start) { copy_size = end - start; }

    data = byte_slice_write_bytes(data, tag->data + start, (int32_t)copy_size);

    partial = (start + copy_size < end);
    if(partial) { log_info("Need to fragment read request."); }

    cip_write_reply_header(header, req->service, (partial || endless_fragmentation) ? CIP_ERR_FRAG : CIP_OK, false, 0);

    return data;
}


/* ponytail: separate atomics, so a concurrent update can be lost; fine for statistics. */
static void record_latency(tag_def_s *tag, int64_t start_us) {
    int64_t latency = util_time_us() - start_us;
    int64_t current_min = atomic_load_int64(&tag->min_latency_us);
    int64_t current_max = atomic_load_int64(&tag->max_latency_us);

    atomic_inc_int32(&tag->request_count);

    if(current_min == 0 || latency < current_min) { atomic_store_int64(&tag->min_latency_us, latency); }

    if(latency > current_max) { atomic_store_int64(&tag->max_latency_us, latency); }

    atomic_store_int64(&tag->total_latency_us, atomic_load_int64(&tag->total_latency_us) + latency);
}


/*
 * Write Tag (0x4D): the tag's type info, the element count and the data.  Write Tag Fragmented
 * (0x53) puts a byte offset before the data.  The reply is a bare header.
 */
static byte_slice_t write_tag(cip_request_t *req, byte_slice_t out, plc_s *plc) {
    int64_t start_us = util_time_us();
    byte_slice_t in = req->payload;
    byte_slice_t type_info = {0};
    tag_def_s *tag = NULL;
    uint32_t num_indexes = CIP_TAG_MAX_INDEXES;
    uint32_t indexes[CIP_TAG_MAX_INDEXES] = {0};
    uint16_t element_count = 0;
    uint32_t fragment_offset = 0;
    size_t start = 0;
    size_t end = 0;
    int32_t data_len = 0;
    uint8_t status = CIP_OK;

    log_info("Processing Write Named Tag%s request.", req->service == CIP_SRV_WRITE_NAMED_TAG_FRAG ? " Fragmented" : "");

    if(plc->plc_type == PLC_OMRON && req->service != CIP_SRV_WRITE_NAMED_TAG) {
        log_info("Omron PLCs do not support fragmented write CIP service!");
        return cip_write_reply_header(out, req->service, CIP_ERR_UNSUPPORTED, false, 0);
    }

    if(!parse_tag_path(req->path, plc, &tag, &num_indexes, &(indexes[0]))) {
        log_info("Unable to parse tag path:");
        log_info_byte_slice(req->path);
        return cip_write_reply_header(out, req->service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    /*
     * The encoded type is as wide as the tag's own encoding: two bytes for an atomic type, four
     * for a structure (the 0xA0 marker, a length and the handle).
     */
    in = byte_slice_take(in, (int32_t)tag->type_info_size, &type_info);
    in = byte_slice_read_u16_le(in, &element_count);
    if(req->service == CIP_SRV_WRITE_NAMED_TAG_FRAG) { in = byte_slice_read_u32_le(in, &fragment_offset); }
    data_len = byte_slice_len(in);

    if(byte_slice_has_error(in) || data_len == 0) {
        log_info("Insufficient data in the CIP write request payload!");
        return cip_write_reply_header(out, req->service, CIP_ERR_INSUFFICIENT_DATA, false, 0);
    }

    if(byte_slice_compare(type_info, byte_slice_make(tag->type_info, (int32_t)tag->type_info_size), INT32_MAX) != 0) {
        log_info("Request element type does not match tag type!");
        return cip_write_reply_header(out, req->service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    if(tag->variable_elements) {
        /*
         * The elements have no stride, so a write cannot be placed by multiplying and the new
         * content is free to be a different length than what is stored.  The client sends the
         * whole packed array, so its payload simply becomes the tag's new extent.
         *
         * Writing part of such an array would mean shifting everything after it, which no
         * client here does -- the Omron and Micro800 paths never fragment a write -- so reject
         * that rather than implement it.
         */
        if(fragment_offset != 0 || (num_indexes > 0 && indexes[0] != 0)) {
            log_info("A partial write to a variable-length element tag is not supported!");
            return cip_write_reply_header(out, req->service, CIP_ERR_INVALID_PARAM, false, 0);
        }

        if(element_count != tag->elem_count) {
            log_info("A write to a variable-length element tag must cover all %zu elements, not %u.", tag->elem_count,
                     element_count);
            return cip_write_reply_header(out, req->service, CIP_ERR_INVALID_PARAM, false, 0);
        }
    } else {
        if(!calculate_request_start_and_end_offsets(tag, num_indexes, indexes, element_count, &start, &end)) {
            log_info("Unable to calculate the starting or ending offset of the write request!");
            return cip_write_reply_header(out, req->service, CIP_ERR_INVALID_PARAM, false, 0);
        }

        start += fragment_offset;

        if(start + (size_t)data_len > end) {
            log_info("Too much data in the write request!");
            return cip_write_reply_header(out, req->service, CIP_ERR_INVALID_PARAM, false, 0);
        }
    }

    /* resize and copy under one lock, so no reader sees the new size with the old data. */
    critical_block(tag->data_mutex) {
        if(tag->variable_elements) {
            uint8_t *new_data = realloc(tag->data, (size_t)data_len);

            if(!new_data) {
                status = CIP_ERR_INVALID_PARAM;
                break;
            }

            tag->data = new_data;
            tag->data_size = (size_t)data_len;
        }

        memcpy(tag->data + start, byte_slice_get_data(in), (size_t)data_len);
    }

    if(status != CIP_OK) {
        log_info("Unable to resize the tag data buffer for the write!");
        return cip_write_reply_header(out, req->service, status, false, 0);
    }

    record_latency(tag, start_us);

    return cip_write_reply_header(out, req->service, CIP_OK, false, 0);
}


/*
 * Handle Read-Modify-Write (service 0x4e on a tag path).
 *
 * This is how a client changes one bit of a word without reading it first: the PLC does the
 * read, the mask and the write itself, so nothing another client wrote to the rest of the
 * word in the meantime is lost.  A client that emulates it with a read and a whole-word write
 * has a window where exactly that happens.
 *
 * The request payload is
 *
 *   uint16_le  size of one mask in bytes
 *   uint8_t[]  OR mask   -- a set bit turns its bit on
 *   uint8_t[]  AND mask  -- a clear bit turns its bit off
 *
 * and the new value is (old & AND) | OR, applied one element wide at the addressed element.
 * The reply carries no data.
 *
 * Validating the mask width against the tag's element size is the point of this handler
 * existing: a client that got the width or the byte order wrong would otherwise be answered
 * by a server that happily masked the wrong bytes, and the simulator would agree with the
 * client while a real PLC rejected it.
 */
static byte_slice_t read_modify_write(cip_request_t *req, byte_slice_t out, plc_s *plc) {
    byte_slice_t in = req->payload;
    byte_slice_t or_mask = {0};
    byte_slice_t and_mask = {0};
    tag_def_s *tag = NULL;
    uint32_t num_indexes = CIP_TAG_MAX_INDEXES;
    uint32_t indexes[CIP_TAG_MAX_INDEXES] = {0};
    uint16_t mask_size = 0;
    size_t start = 0;
    size_t end = 0;
    uint8_t status = CIP_OK;

    log_info("Processing Read Modify Write request.");

    if(!parse_tag_path(req->path, plc, &tag, &num_indexes, &(indexes[0]))) {
        log_info("Unable to parse tag path:");
        log_info_byte_slice(req->path);
        return cip_write_reply_header(out, req->service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    /*
     * A variable-length element has no stride, so there is no fixed-width element to mask and
     * no way to find one by index.  A real controller has no such tag type on this service.
     */
    if(tag->variable_elements) {
        log_info("Read Modify Write is not supported on a variable-length element tag!");
        return cip_write_reply_header(out, req->service, CIP_ERR_UNSUPPORTED, false, 0);
    }

    in = byte_slice_read_u16_le(in, &mask_size);
    in = byte_slice_take(in, mask_size, &or_mask);
    in = byte_slice_take(in, mask_size, &and_mask);

    in = byte_slice_end(in);


    if(byte_slice_has_error(in)) {
        log_info("Read Modify Write payload does not end with exactly two %u byte masks!", mask_size);
        return cip_write_reply_header(out, req->service, cip_input_status(in), false, 0);
    }

    /* the masks cover exactly one element.  A real controller rejects any other width. */
    if((size_t)mask_size != tag->elem_size) {
        log_info("Read Modify Write mask is %u bytes but an element of this tag is %zu bytes!", mask_size, tag->elem_size);
        return cip_write_reply_header(out, req->service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    /* one element, at the index the path gave. */
    if(!calculate_request_start_and_end_offsets(tag, num_indexes, indexes, 1, &start, &end)) {
        log_info("Unable to calculate the offset of the Read Modify Write request!");
        return cip_write_reply_header(out, req->service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    critical_block(tag->data_mutex) {
        if(end > tag->data_size) {
            log_info("The addressed element runs past the end of the tag data!");
            status = CIP_ERR_INVALID_PARAM;
            break;
        }

        for(size_t i = 0; i < (size_t)mask_size; i++) {
            uint8_t or_bits = byte_slice_get_data(or_mask)[i];
            uint8_t and_bits = byte_slice_get_data(and_mask)[i];
            uint8_t old_byte = tag->data[start + i];
            uint8_t new_byte = (uint8_t)((old_byte & and_bits) | or_bits);

            log_info("RMW byte %zu: %02x & %02x | %02x = %02x", i, old_byte, and_bits, or_bits, new_byte);

            tag->data[start + i] = new_byte;
        }
    }

    /* the reply is a bare header; RMW returns no data. */
    return cip_write_reply_header(out, req->service, status, false, 0);
}


/* the tag, then zero or one numeric segment per dimension; a truncated segment is an error. */
static bool parse_tag_path(byte_slice_t path, plc_s *plc, tag_def_s **tag, uint32_t *num_indexes, uint32_t *indexes) {
    uint32_t max_indexes = *num_indexes; /* contains the max possible */

    path = cip_find_symbol_tag(path, plc, tag);
    if(!*tag) { return false; }

    *num_indexes = 0;

    while(byte_slice_len(path) > 0) {
        uint8_t segment_type = 0;
        uint8_t u8 = 0;
        uint16_t u16 = 0;

        /*
         * The caller's indexes[] array holds exactly max_indexes entries, so this must be checked
         * before the writes below and not after the increment: a remote client controls how many
         * numeric segments the path carries.
         */
        if(*num_indexes >= max_indexes) {
            log_info("More numeric segments than the expected maximum, %u!", max_indexes);
            return false;
        }

        path = byte_slice_read_u8(path, &segment_type);

        switch(segment_type) {
            case 0x28: /* one byte value */
                path = byte_slice_read_u8(path, &u8);
                indexes[*num_indexes] = u8;
                break;

            case 0x29: /* pad byte, two byte value */
                path = byte_slice_read_u16_le(byte_slice_skip(path, 1), &u16);
                indexes[*num_indexes] = u16;
                break;

            case 0x2A: /* pad byte, four byte value */
                path = byte_slice_read_u32_le(byte_slice_skip(path, 1), &indexes[*num_indexes]);
                break;

            default: log_info("Unexpected numeric segment marker %x!", segment_type); return false;
        }

        if(byte_slice_has_error(path)) {
            log_info("Truncated numeric segment %x!", segment_type);
            return false;
        }

        log_info("Numeric segment: %u", indexes[*num_indexes]);
        (*num_indexes)++;
    }

    /* the only valid number of indexes is zero or the number of dimensions in the tag. */
    if(*num_indexes != 0 && *num_indexes != (*tag)->num_dimensions) {
        log_info("Required zero or %zu numeric segments, but only found %zu!", (*tag)->num_dimensions, (size_t)*num_indexes);
        return false;
    }

    return true;
}


/* 0x91, the name length, the name and a pad byte if the length is odd.  *tag is NULL if not found. */
byte_slice_t cip_find_symbol_tag(byte_slice_t path, plc_s *plc, tag_def_s **tag) {
    uint8_t marker = 0;
    uint8_t name_len = 0;
    byte_slice_t name = {0};

    *tag = NULL;

    path = byte_slice_read_u8(path, &marker);
    path = byte_slice_read_u8(path, &name_len);
    path = byte_slice_take(path, name_len, &name);
    path = byte_slice_skip(path, name_len & 1);

    if(byte_slice_has_error(path) || marker != CIP_SYMBOLIC_SEGMENT_MARKER) {
        log_info("Path does not start with a complete symbolic segment!");
        return byte_slice_fail(path, PLCTAG_ERR_BAD_DATA);
    }

    for(tag_def_s *t = plc->tags; t; t = t->next_tag) {
        if(byte_slice_compare(name, byte_slice_make((uint8_t *)t->name, (int32_t)strlen(t->name)), INT32_MAX) == 0) {
            log_info("Found tag %s", t->name);
            *tag = t;
            return path;
        }
    }

    log_info("Tag %.*s not found!", (int)name_len, (const char *)byte_slice_get_data(name));
    return path;
}


/* we assume that the number of indexes matches that of the tag or is zero */

/*
 * Step over one packed CIP string: a two-byte count followed by exactly that many characters.
 * Returns false if the count runs off the end of the data, which is what a truncated or
 * corrupted buffer looks like.
 */
static bool step_over_string(tag_def_s *tag, size_t *offset) {
    size_t count = 0;
    size_t terminator = tag->elements_zero_terminated ? 1u : 0u;

    if(*offset + tag->count_word_bytes > tag->data_size) { return false; }

    /* the count word is one byte for a SHORT_STRING and two, little endian, for a STRING. */
    count = (size_t)tag->data[*offset];
    if(tag->count_word_bytes == 2) { count += ((size_t)tag->data[*offset + 1] << 8); }

    if(count + terminator > tag->data_size - *offset - tag->count_word_bytes) { return false; }

    *offset += tag->count_word_bytes + count + terminator;

    return true;
}


/*
 * Byte range of a request against a tag whose elements are packed at their own lengths.  There
 * is no stride to multiply by, so walk the count words instead.
 */
static bool calculate_variable_offsets(tag_def_s *tag, uint32_t num_indexes, uint32_t *indexes,
                                       uint16_t request_element_count, size_t *request_start_byte_offset,
                                       size_t *request_end_byte_offset) {
    size_t offset = 0;
    size_t element_offset = 0;

    /* only a single dimension makes sense for a packed array, and that is all this serves. */
    if(num_indexes > 1) {
        log_info("A variable-length element tag supports at most one index!");
        return false;
    }

    if(num_indexes == 1) {
        if(indexes[0] >= tag->elem_count) {
            log_info("Index %zu out of bounds for %zu elements.", (size_t)indexes[0], tag->elem_count);
            return false;
        }

        element_offset = indexes[0];
    }

    for(size_t i = 0; i < element_offset; i++) {
        if(!step_over_string(tag, &offset)) {
            log_info("Element %zu runs past the end of the tag data!", i);
            return false;
        }
    }

    *request_start_byte_offset = offset;

    if((size_t)request_element_count > tag->elem_count - element_offset) {
        log_info("Request for %u elements from element %zu exceeds the %zu in the tag.", request_element_count,
                 element_offset, tag->elem_count);
        return false;
    }

    for(size_t i = 0; i < (size_t)request_element_count; i++) {
        if(!step_over_string(tag, &offset)) {
            log_info("Element %zu runs past the end of the tag data!", element_offset + i);
            return false;
        }
    }

    *request_end_byte_offset = offset;

    log_info("Variable element request byte offsets: %zu to %zu.", *request_start_byte_offset, *request_end_byte_offset);

    return true;
}


bool calculate_request_start_and_end_offsets(tag_def_s *tag, uint32_t num_indexes, uint32_t *indexes,
                                             uint16_t request_element_count, size_t *request_start_byte_offset,
                                             size_t *request_end_byte_offset) {
    size_t total_elements = 1;
    size_t tag_size = 0;
    size_t element_offset = 0;

    if(tag->variable_elements) {
        return calculate_variable_offsets(tag, num_indexes, indexes, request_element_count, request_start_byte_offset,
                                          request_end_byte_offset);
    }

    /* Calculate the total number of elements in the tag */
    total_elements = 1;
    for(uint32_t i = 0; i < tag->num_dimensions; i++) { total_elements *= tag->dimensions[i]; }

    tag_size = total_elements * tag->elem_size;

    /* check index bounds */
    for(size_t index = 0; index < num_indexes; index++) {
        if(indexes[index] >= tag->dimensions[index]) {
            log_info("Index %zu out of bounds for dimension %zu.", (size_t)indexes[index], index);
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
            log_info("Too many indexes!");
            return false;
            break;
    }

    /* probably not needed, but just in case */
    if(element_offset >= total_elements) {
        log_info("Element offset %d exceeds total elements %d!", element_offset, total_elements);
        return false;
    }

    /*
     * Calculate the start and end byte offsets.
     * Note that these are absolute for the whole request.
     * Not for the fragment if there is one.
     */
    *request_start_byte_offset = element_offset * tag->elem_size;
    *request_end_byte_offset = *request_start_byte_offset + (request_element_count * tag->elem_size);

    log_info("Request start byte offset: %d", *request_start_byte_offset);
    log_info("Request end byte offset: %d", *request_end_byte_offset);

    /* Check if the start offset exceeds the total size of the tag */
    if(*request_start_byte_offset > tag_size) {
        log_info("Request start byte offset %d exceeds total tag size %zu", (size_t)*request_start_byte_offset, tag_size);
        return false;
    }

    /* Check if the end offset exceeds the total size of the tag */
    if(*request_end_byte_offset > tag_size) {
        log_info("Request end byte offset %d exceeds total tag size %zu", (size_t)*request_end_byte_offset, tag_size);
        return false;
    }

    return true;
}
