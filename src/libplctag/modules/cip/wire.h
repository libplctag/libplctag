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


#pragma once

#include <stddef.h>

#include <platform.h>
#include <utils/byteorder.h>

/*
 * EtherNet/IP encapsulation, Common Packet Format and CIP wire structures.
 *
 * These are on-the-wire layouts, so every field is explicitly sized and little
 * endian and the whole struct is packed.  Nothing here is family specific: the
 * same bytes go to a ControlLogix and to an OMRON NJ/NX.  Do not add a field
 * that only one family uses.
 */


/*
 * The EtherNet/IP encapsulation header, the interface handle and router timeout that follow
 * it, and the unconnected Common Packet Format items.  Every framed message repeats some run
 * of these, so they are written once here and pasted in by name.
 *
 * These are field lists rather than nested structs on purpose: a nested struct would change
 * how the fields are spelled at every use and raises a question about how packing applies
 * through it, while a macro cannot change the layout at all.  The _Static_assert block at the
 * end of this file is what proves that.  It is the same idiom CIP_CONN_BASE_STRUCT uses in
 * conn.h.
 */

/* clang-format off */
#define CIP_EIP_ENCAP_FIELDS                                                            \
    uint16_le encap_command;        /* the EIP command */                               \
    uint16_le encap_length;         /* packet size in bytes, less this 24-byte header */ \
    uint32_le encap_session_handle; /* from session registration */                     \
    uint32_le encap_status;         /* always sent as zero */                           \
    uint64_le encap_sender_context; /* echoed back, so a reply can be matched to its request */ \
    uint32_le encap_options         /* reserved, zero */

/* Addressed to the CIP object interface, which for everything here is interface zero. */
#define CIP_EIP_INTERFACE_FIELDS                                                        \
    uint32_le interface_handle; /* ALWAYS 0 */                                          \
    uint16_le router_timeout    /* in seconds */

/* CPF carrying a null address item and an unconnected data item. */
#define CIP_CPF_UNCONNECTED_FIELDS                                                      \
    uint16_le cpf_item_count;      /* ALWAYS 2 */                                       \
    uint16_le cpf_nai_item_type;   /* ALWAYS 0, null address item */                    \
    uint16_le cpf_nai_item_length; /* ALWAYS 0 */                                       \
    uint16_le cpf_udi_item_type;   /* ALWAYS 0x00B2, unconnected data item */           \
    uint16_le cpf_udi_item_length  /* length of everything after this field */
/* clang-format on */


START_PACK typedef struct {
    uint8_t reply_service;    /* 0x?? CIP reply */
    uint8_t reserved;         /* 0x00 in reply */
    uint8_t status;           /* 0x00 for success */
    uint8_t num_status_words; /* number of 16-bit words in status */
} END_PACK cip_header;


START_PACK typedef struct {
    uint8_t service_code;        /* ALWAYS 0x0A Forward Open Request */
    uint8_t req_path_size;       /* ALWAYS 2, size in words of path, next field */
    uint8_t req_path[4];         /* ALWAYS 0x20,0x06,0x24,0x01 for CM, instance 1*/
    uint16_le request_count;     /* number of requests packed in this packet. */
    uint16_le request_offsets[]; /* request offsets from the count */
} END_PACK cip_multi_req_header;


START_PACK typedef struct {
    uint8_t reply_service;       /* 0x?? CIP reply */
    uint8_t reserved;            /* 0x00 in reply */
    uint8_t status;              /* 0x00 for success */
    uint8_t num_status_words;    /* number of 16-bit words in status */
    uint16_le request_count;     /* number of requests packed in this packet. */
    uint16_le request_offsets[]; /* request offsets from the count */
} END_PACK cip_multi_resp_header;


START_PACK typedef struct {
    CIP_EIP_ENCAP_FIELDS;
} END_PACK eip_encap;


START_PACK typedef struct {
    CIP_EIP_ENCAP_FIELDS;

    /* session registration request */
    uint16_le eip_version;
    uint16_le option_flags;
} END_PACK eip_session_reg_req;


START_PACK typedef struct {
    uint16_le cpf_udi_item_type;   /* ALWAYS 0x00B2 - Unconnected Data Item */
    uint16_le cpf_udi_item_length; /* REQ: fill in with length of remaining data. */
} END_PACK cpf_unconnected_data_item;


START_PACK typedef struct {
    uint16_le cpf_cdi_item_type;   /* ALWAYS 0x00B1, Connected Data Item type */
    uint16_le cpf_cdi_item_length; /* length in bytes of the rest of the packet */

    /* Connection sequence number */
    uint16_le cpf_conn_seq_num; /* connection sequence ID,*/
} END_PACK cpf_connected_data_item;


START_PACK typedef struct {
    CIP_EIP_ENCAP_FIELDS;

    CIP_EIP_INTERFACE_FIELDS;

    CIP_CPF_UNCONNECTED_FIELDS;
} END_PACK eip_cpf_uc_header;


START_PACK typedef struct {
    CIP_EIP_ENCAP_FIELDS;

    CIP_EIP_INTERFACE_FIELDS;

    CIP_CPF_UNCONNECTED_FIELDS;

    /* CM Service Request - Connection Manager */
    uint8_t cm_service_code;  /* ALWAYS 0x54 Forward Open Request */
    uint8_t cm_req_path_size; /* ALWAYS 2, size in words of path, next field */
    uint8_t cm_req_path[4];   /* ALWAYS 0x20,0x06,0x24,0x01 for CM, instance 1*/

    /* Forward Open Params */
    uint8_t secs_per_tick;              /* seconds per tick */
    uint8_t timeout_ticks;              /* timeout = srd_secs_per_tick * src_timeout_ticks */
    uint32_le orig_to_targ_conn_id;     /* 0, returned by target in reply. */
    uint32_le targ_to_orig_conn_id;     /* what is _our_ ID for this connection, use ab_connection ptr as id ? */
    uint16_le conn_serial_number;       /* our connection ID/serial number */
    uint16_le orig_vendor_id;           /* our unique vendor ID */
    uint32_le orig_serial_number;       /* our unique serial number */
    uint8_t conn_timeout_multiplier;    /* timeout = mult * RPI */
    uint8_t reserved[3];                /* reserved, set to 0 */
    uint32_le orig_to_targ_rpi;         /* us to target RPI - Request Packet Interval in microseconds */
    uint16_le orig_to_targ_conn_params; /* some sort of identifier of what kind of PLC we are??? */
    uint32_le targ_to_orig_rpi;         /* target to us RPI, in microseconds */
    uint16_le targ_to_orig_conn_params; /* some sort of identifier of what kind of PLC the target is ??? */
    uint8_t transport_class;            /* ALWAYS 0xA3, server transport, class 3, application trigger */
    uint8_t path_size;                  /* size of connection path in 16-bit words
                                         * connection path from MSG instruction.
                                         *
                                         * EG LGX with 1756-ENBT and CPU in slot 0 would be:
                                         * 0x01 - backplane port of 1756-ENBT
                                         * 0x00 - slot 0 for CPU
                                         * 0x20 - class
                                         * 0x02 - MR Message Router
                                         * 0x24 - instance
                                         * 0x01 - instance #1.
                                         */

    // uint8_t conn_path[ZLA_SIZE];    /* connection path as above */
} END_PACK eip_forward_open_request_t;


START_PACK typedef struct {
    CIP_EIP_ENCAP_FIELDS;

    CIP_EIP_INTERFACE_FIELDS;

    CIP_CPF_UNCONNECTED_FIELDS;

    /* CM Service Request - Connection Manager */
    uint8_t cm_service_code;  /* ALWAYS 0x5B Extended Forward Open Request */
    uint8_t cm_req_path_size; /* ALWAYS 2, size in words of path, next field */
    uint8_t cm_req_path[4];   /* ALWAYS 0x20,0x06,0x24,0x01 for CM, instance 1*/

    /* Forward Open Params */
    uint8_t secs_per_tick;                 /* seconds per tick */
    uint8_t timeout_ticks;                 /* timeout = srd_secs_per_tick * src_timeout_ticks */
    uint32_le orig_to_targ_conn_id;        /* 0, returned by target in reply. */
    uint32_le targ_to_orig_conn_id;        /* what is _our_ ID for this connection, use ab_connection ptr as id ? */
    uint16_le conn_serial_number;          /* our connection ID/serial number ?? */
    uint16_le orig_vendor_id;              /* our unique vendor ID */
    uint32_le orig_serial_number;          /* our unique serial number */
    uint8_t conn_timeout_multiplier;       /* timeout = mult * RPI */
    uint8_t reserved[3];                   /* reserved, set to 0 */
    uint32_le orig_to_targ_rpi;            /* us to target RPI - Request Packet Interval in microseconds */
    uint32_le orig_to_targ_conn_params_ex; /* some sort of identifier of what kind of PLC we are??? */
    uint32_le targ_to_orig_rpi;            /* target to us RPI, in microseconds */
    uint32_le targ_to_orig_conn_params_ex; /* some sort of identifier of what kind of PLC the target is ??? */
    uint8_t transport_class;               /* ALWAYS 0xA3, server transport, class 3, application trigger */
    uint8_t path_size;                     /* size of connection path in 16-bit words
                                            * connection path from MSG instruction.
                                            *
                                            * EG LGX with 1756-ENBT and CPU in slot 0 would be:
                                            * 0x01 - backplane port of 1756-ENBT
                                            * 0x00 - slot 0 for CPU
                                            * 0x20 - class
                                            * 0x02 - MR Message Router
                                            * 0x24 - instance
                                            * 0x01 - instance #1.
                                            */

    // uint8_t conn_path[ZLA_SIZE];    /* connection path as above */
} END_PACK eip_forward_open_request_ex_t;


START_PACK typedef struct {
    CIP_EIP_ENCAP_FIELDS;

    CIP_EIP_INTERFACE_FIELDS;

    CIP_CPF_UNCONNECTED_FIELDS;

    /* Forward Open Reply */
    uint8_t resp_service_code;      /* returned as 0xD4 or 0xDB */
    uint8_t reserved1;              /* returned as 0x00? */
    uint8_t general_status;         /* 0 on success */
    uint8_t status_size;            /* number of 16-bit words of extra status, 0 if success */
    uint32_le orig_to_targ_conn_id; /* target's connection ID for us, save this. */
    uint32_le targ_to_orig_conn_id; /* our connection ID back for reference */
    uint16_le conn_serial_number;   /* our connection ID/serial number from request */
    uint16_le orig_vendor_id;       /* our unique vendor ID from request*/
    uint32_le orig_serial_number;   /* our unique serial number from request*/
    uint32_le orig_to_targ_api;     /* Actual packet interval, microsecs */
    uint32_le targ_to_orig_api;     /* Actual packet interval, microsecs */
    uint8_t app_data_size;          /* size in 16-bit words of send_data at end */
    uint8_t reserved2;
    // uint8_t app_data[ZLA_SIZE];
} END_PACK eip_forward_open_response_t;


START_PACK typedef struct {
    CIP_EIP_ENCAP_FIELDS;

    CIP_EIP_INTERFACE_FIELDS;

    CIP_CPF_UNCONNECTED_FIELDS;

    /* CM Service Request - Connection Manager */
    uint8_t cm_service_code;  /* ALWAYS 0x4E Forward Close Request */
    uint8_t cm_req_path_size; /* ALWAYS 2, size in words of path, next field */
    uint8_t cm_req_path[4];   /* ALWAYS 0x20,0x06,0x24,0x01 for CM, instance 1*/

    /* Forward Open Params */
    uint8_t secs_per_tick;        /* seconds per tick */
    uint8_t timeout_ticks;        /* timeout = srd_secs_per_tick * src_timeout_ticks */
    uint16_le conn_serial_number; /* our connection ID/serial number */
    uint16_le orig_vendor_id;     /* our unique vendor ID */
    uint32_le orig_serial_number; /* our unique serial number */
    uint8_t path_size;            /* size of connection path in 16-bit words*/
    uint8_t reserved;             /* ALWAYS 0 */
    // uint8_t conn_path[ZLA_SIZE];
} END_PACK eip_forward_close_req_t;


START_PACK typedef struct {
    CIP_EIP_ENCAP_FIELDS;

    CIP_EIP_INTERFACE_FIELDS;

    CIP_CPF_UNCONNECTED_FIELDS;

    /* Forward Close Response */
    uint8_t resp_service_code;    /* returned as 0xCE */
    uint8_t reserved1;            /* returned as 0x00? */
    uint8_t general_status;       /* 0 on success */
    uint8_t status_size;          /* number of 16-bit words of extra status, 0 if success */
    uint16_le conn_serial_number; /* our connection ID/serial number ?? */
    uint16_le orig_vendor_id;     /* our unique vendor ID */
    uint32_le orig_serial_number; /* our unique serial number */
    uint8_t path_size;            /* size of connection path in 16-bit words*/
    uint8_t reserved;             /* ALWAYS 0 */
    // uint8_t conn_path[ZLA_SIZE];
} END_PACK eip_forward_close_resp_t;


START_PACK typedef struct {
    CIP_EIP_ENCAP_FIELDS;

    CIP_EIP_INTERFACE_FIELDS;

    /* Common Packet Format - CPF Connected */
    uint16_le cpf_item_count;      /* ALWAYS 2 */
    uint16_le cpf_cai_item_type;   /* ALWAYS 0x00A1 Connected Address Item */
    uint16_le cpf_cai_item_length; /* ALWAYS 2 ? */
    uint32_le cpf_targ_conn_id;    /* the connection id from Forward Open */
    uint16_le cpf_cdi_item_type;   /* ALWAYS 0x00B1, Connected Data Item type */
    uint16_le cpf_cdi_item_length; /* length in bytes of the rest of the packet */

    /* Connection sequence number */
    uint16_le cpf_conn_seq_num; /* connection sequence ID, inc for each message */

    /* CIP Service Info */
    // uint8_t service_code;           /* ALWAYS 0x4C, CIP_READ */
    /*uint8_t req_path_size;*/ /* path size in words */
    // uint8_t req_path[ZLA_SIZE];
} END_PACK eip_cip_co_req;


START_PACK typedef struct {
    CIP_EIP_ENCAP_FIELDS;

    CIP_EIP_INTERFACE_FIELDS;

    /* Common Packet Format - CPF Connected */
    uint16_le cpf_item_count;      /* ALWAYS 2 */
    uint16_le cpf_cai_item_type;   /* ALWAYS 0x00A1 Connected Address Item */
    uint16_le cpf_cai_item_length; /* ALWAYS 2 ? */
    uint32_le cpf_orig_conn_id;    /* our connection ID, NOT the target's */
    uint16_le cpf_cdi_item_type;   /* ALWAYS 0x00B1, Connected Data Item type */
    uint16_le cpf_cdi_item_length; /* length in bytes of the rest of the packet */

    /* connection ID from request */
    uint16_le cpf_conn_seq_num; /* connection sequence ID, inc for each message */

    /* CIP Reply */
    uint8_t reply_service;    /* 0xCC CIP READ Reply */
    uint8_t reserved;         /* 0x00 in reply */
    uint8_t status;           /* 0x00 for success */
    uint8_t num_status_words; /* number of 16-bit words in status */

    /* CIP Data*/
    // uint8_t resp_data[ZLA_SIZE];
} END_PACK eip_cip_co_resp;


START_PACK typedef struct {
    CIP_EIP_ENCAP_FIELDS;

    CIP_EIP_INTERFACE_FIELDS;

    CIP_CPF_UNCONNECTED_FIELDS;

    /* CM Service Request - Connection Manager */
    /* NOTE, we overlay the following if this is PCCC */
    uint8_t cm_service_code;  /* ALWAYS 0x52 Unconnected Send */
    uint8_t cm_req_path_size; /* ALWAYS 2, size in words of path, next field */
    uint8_t cm_req_path[4];   /* ALWAYS 0x20,0x06,0x24,0x01 for CM, instance 1*/

    /* Unconnected send */
    uint8_t secs_per_tick; /* seconds per tick */
    uint8_t timeout_ticks; /* timeout = src_secs_per_tick * src_timeout_ticks */

    /* size ? */
    uint16_le uc_cmd_length; /* length of embedded packet */

    /* CIP read/write request, embedded packet */

    /* IOI path to target device, connection IOI */
} END_PACK eip_cip_uc_req;


START_PACK typedef struct {
    CIP_EIP_ENCAP_FIELDS;

    CIP_EIP_INTERFACE_FIELDS;

    CIP_CPF_UNCONNECTED_FIELDS;

    /* CIP read/write response, embedded packet */
    uint8_t reply_service;    /*  */
    uint8_t reserved;         /* 0x00 in reply */
    uint8_t status;           /* 0x00 for success */
    uint8_t num_status_words; /* number of 16-bit words in status */

} END_PACK eip_cip_uc_resp;


/*
 * Wire-layout guards.  These structures are the on-the-wire format, so a change to
 * a field type, order or the packing macros is a protocol bug rather than a
 * compile error.  The sizes and the final-field offsets below were measured before
 * the AB and OMRON copies of these definitions were merged into this file; they
 * must not change.
 */

_Static_assert(sizeof(cip_header) == 4, "cip_header wire size changed");
_Static_assert(offsetof(cip_header, num_status_words) == 3, "cip_header layout changed");
_Static_assert(sizeof(cip_multi_req_header) == 8, "cip_multi_req_header wire size changed");
_Static_assert(offsetof(cip_multi_req_header, request_offsets) == 8, "cip_multi_req_header layout changed");
_Static_assert(sizeof(cip_multi_resp_header) == 6, "cip_multi_resp_header wire size changed");
_Static_assert(offsetof(cip_multi_resp_header, request_offsets) == 6, "cip_multi_resp_header layout changed");
_Static_assert(sizeof(cpf_connected_data_item) == 6, "cpf_connected_data_item wire size changed");
_Static_assert(offsetof(cpf_connected_data_item, cpf_conn_seq_num) == 4, "cpf_connected_data_item layout changed");
_Static_assert(sizeof(cpf_unconnected_data_item) == 4, "cpf_unconnected_data_item wire size changed");
_Static_assert(offsetof(cpf_unconnected_data_item, cpf_udi_item_length) == 2, "cpf_unconnected_data_item layout changed");
_Static_assert(sizeof(eip_cip_co_req) == 46, "eip_cip_co_req wire size changed");
_Static_assert(offsetof(eip_cip_co_req, cpf_conn_seq_num) == 44, "eip_cip_co_req layout changed");
_Static_assert(sizeof(eip_cip_co_resp) == 50, "eip_cip_co_resp wire size changed");
_Static_assert(offsetof(eip_cip_co_resp, num_status_words) == 49, "eip_cip_co_resp layout changed");
_Static_assert(sizeof(eip_cip_uc_req) == 50, "eip_cip_uc_req wire size changed");
_Static_assert(offsetof(eip_cip_uc_req, uc_cmd_length) == 48, "eip_cip_uc_req layout changed");
_Static_assert(sizeof(eip_cip_uc_resp) == 44, "eip_cip_uc_resp wire size changed");
_Static_assert(offsetof(eip_cip_uc_resp, num_status_words) == 43, "eip_cip_uc_resp layout changed");
_Static_assert(sizeof(eip_cpf_uc_header) == 40, "eip_cpf_uc_header wire size changed");
_Static_assert(offsetof(eip_cpf_uc_header, cpf_udi_item_length) == 38, "eip_cpf_uc_header layout changed");
_Static_assert(sizeof(eip_encap) == 24, "eip_encap wire size changed");
_Static_assert(offsetof(eip_encap, encap_options) == 20, "eip_encap layout changed");
_Static_assert(sizeof(eip_forward_close_req_t) == 58, "eip_forward_close_req_t wire size changed");
_Static_assert(offsetof(eip_forward_close_req_t, reserved) == 57, "eip_forward_close_req_t layout changed");
_Static_assert(sizeof(eip_forward_close_resp_t) == 54, "eip_forward_close_resp_t wire size changed");
_Static_assert(offsetof(eip_forward_close_resp_t, reserved) == 53, "eip_forward_close_resp_t layout changed");
_Static_assert(sizeof(eip_forward_open_request_ex_t) == 86, "eip_forward_open_request_ex_t wire size changed");
_Static_assert(offsetof(eip_forward_open_request_ex_t, path_size) == 85, "eip_forward_open_request_ex_t layout changed");
_Static_assert(sizeof(eip_forward_open_request_t) == 82, "eip_forward_open_request_t wire size changed");
_Static_assert(offsetof(eip_forward_open_request_t, path_size) == 81, "eip_forward_open_request_t layout changed");
_Static_assert(sizeof(eip_forward_open_response_t) == 70, "eip_forward_open_response_t wire size changed");
_Static_assert(offsetof(eip_forward_open_response_t, reserved2) == 69, "eip_forward_open_response_t layout changed");
_Static_assert(sizeof(eip_session_reg_req) == 28, "eip_session_reg_req wire size changed");
_Static_assert(offsetof(eip_session_reg_req, option_flags) == 26, "eip_session_reg_req layout changed");
