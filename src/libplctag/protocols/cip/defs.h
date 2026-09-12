#pragma once

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


/*
 * CIP service codes, status codes, data types and connection parameters.
 *
 * Split out of ab/defs.h and omron/defs.h, which carried byte-identical copies
 * under AB_ and OMRON_ prefixes.  These come from the CIP specification.
 *
 * Note the old names said EIP where the value is a CIP service code -- e.g.
 * AB_EIP_CMD_CIP_READ for CIP service 0x4C.  They are CIP_CMD_* here.
 */

#include <stdint.h>

#include <utils/byteorder.h>
#include <utils/macros.h>

/* CIP limits.  These were duplicated in ab/{tag,session}.h and omron/{tag,conn}.h. */
#define MAX_TAG_NAME (260)
#define MAX_CONN_PATH (260) /* 256 plus padding. */
#define MAX_IP_ADDR_SEG_LEN (16)



#define CIP_CONN_PARAM ((uint16_t)0x4200)
#define CIP_CONN_PARAM_EX ((uint32_t)0x42000000)
#define CIP_CMD_PCCC_EXECUTE ((uint8_t)0x4B)
#define CIP_CMD_FORWARD_CLOSE ((uint8_t)0x4E)
#define CIP_CMD_UNCONNECTED_SEND ((uint8_t)0x52)
#define CIP_CMD_FORWARD_OPEN ((uint8_t)0x54)
#define CIP_CMD_FORWARD_OPEN_EX ((uint8_t)0x5B)
#define CIP_CMD_GET_ATTR_LIST ((uint8_t)0x03)
#define CIP_CMD_MULTI ((uint8_t)0x0A)
#define CIP_CMD_READ ((uint8_t)0x4C)
#define CIP_CMD_WRITE ((uint8_t)0x4D)
#define CIP_CMD_RMW ((uint8_t)0x4E)
#define CIP_CMD_READ_FRAG ((uint8_t)0x52)
#define CIP_CMD_WRITE_FRAG ((uint8_t)0x53)
#define CIP_CMD_LIST_TAGS ((uint8_t)0x55)
#define CIP_CMD_OK ((uint8_t)0x80)
#define CIP_STATUS_OK ((uint8_t)0x00)
#define CIP_STATUS_FRAG ((uint8_t)0x06)
#define CIP_ERR_UNSUPPORTED_SERVICE ((uint8_t)0x08)
#define CIP_ERR_PARTIAL_ERROR ((uint8_t)0x1e)
#define CIP_DATA_DT ((uint8_t)0xC0)            /* DT value, 64 bit */
#define CIP_DATA_BIT ((uint8_t)0xC1)           /* Boolean value, 1 bit */
#define CIP_DATA_SINT ((uint8_t)0xC2)          /* Signed 8–bit integer value */
#define CIP_DATA_INT ((uint8_t)0xC3)           /* Signed 16–bit integer value */
#define CIP_DATA_DINT ((uint8_t)0xC4)          /* Signed 32–bit integer value */
#define CIP_DATA_LINT ((uint8_t)0xC5)          /* Signed 64–bit integer value */
#define CIP_DATA_USINT ((uint8_t)0xC6)         /* Unsigned 8–bit integer value */
#define CIP_DATA_UINT ((uint8_t)0xC7)          /* Unsigned 16–bit integer value */
#define CIP_DATA_UDINT ((uint8_t)0xC8)         /* Unsigned 32–bit integer value */
#define CIP_DATA_ULINT ((uint8_t)0xC9)         /* Unsigned 64–bit integer value */
#define CIP_DATA_REAL ((uint8_t)0xCA)          /* 32–bit floating point value, IEEE format */
#define CIP_DATA_LREAL ((uint8_t)0xCB)         /* 64–bit floating point value, IEEE format */
#define CIP_DATA_STIME ((uint8_t)0xCC)         /* Synchronous time value */
#define CIP_DATA_DATE ((uint8_t)0xCD)          /* Date value */
#define CIP_DATA_TIME_OF_DAY ((uint8_t)0xCE)   /* Time of day value */
#define CIP_DATA_DATE_AND_TIME ((uint8_t)0xCF) /* Date and time of day value */
#define CIP_DATA_STRING ((uint8_t)0xD0)        /* Character string, 1 byte per character */
#define CIP_DATA_BYTE ((uint8_t)0xD1)          /* 8-bit bit string */
#define CIP_DATA_WORD ((uint8_t)0xD2)          /* 16-bit bit string */
#define CIP_DATA_DWORD ((uint8_t)0xD3)         /* 32-bit bit string */
#define CIP_DATA_LWORD ((uint8_t)0xD4)         /* 64-bit bit string */
#define CIP_DATA_STRING2 ((uint8_t)0xD5)       /* Wide char character string, 2 bytes per character */
#define CIP_DATA_FTIME ((uint8_t)0xD6)         /* High resolution duration value */
#define CIP_DATA_LTIME ((uint8_t)0xD7)         /* Medium resolution duration value */
#define CIP_DATA_ITIME ((uint8_t)0xD8)         /* Low resolution duration value */
#define CIP_DATA_STRINGN ((uint8_t)0xD9)       /* N-byte per char character string */
#define CIP_DATA_SHORT_STRING \
    ((uint8_t)0xDA)                         /* Counted character sting with 1 byte per character and 1 byte length indicator */
#define CIP_DATA_TIME ((uint8_t)0xDB)    /* Duration in milliseconds */
#define CIP_DATA_EPATH ((uint8_t)0xDC)   /* CIP path segment(s) */
#define CIP_DATA_ENGUNIT ((uint8_t)0xDD) /* Engineering units */
#define CIP_DATA_STRINGI ((uint8_t)0xDE) /* International character string (encoding?) */
#define CIP_DATA_ABREV_STRUCT \
    ((uint8_t)0xA0)                             /* Data is an abbreviated struct type, i.e. a CRC of the actual type descriptor */
#define CIP_DATA_ABREV_ARRAY ((uint8_t)0xA1) /* Data is an abbreviated array type. The limits are left off */
#define CIP_DATA_FULL_STRUCT ((uint8_t)0xA2) /* Data is a struct type descriptor */
#define CIP_DATA_FULL_ARRAY ((uint8_t)0xA3)  /* Data is an array type descriptor */
#define CIP_TRANSPORT_CLASS_T3 ((uint8_t)0xA3)
#define CIP_SECS_PER_TICK 0x0A
#define CIP_TIMEOUT_TICKS 0x0E
#define CIP_VENDOR_ID 0xF33D     /*tres 1337 */
#define CIP_VENDOR_SN 0x21504345 /* the string !PCE */
#define CIP_TIMEOUT_MULTIPLIER 0x03
#define CIP_RPI 1000000 /* in microseconds */
#define CIP_CONN_TIMEOUT_MS ((CIP_RPI * 4 * (1 << CIP_TIMEOUT_MULTIPLIER)) / 1000)


/*********************************************************************
 ** CIP messages as carried over EtherNet/IP
 **
 ** These were carried in identical copies in ab/defs.h and omron/defs.h.
 *********************************************************************/


/*********************************************************************
 ************************ AB EIP Structures **************************
 ********************************************************************/


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


/* Forward Open Request */
START_PACK typedef struct {
    /* encap header */
    uint16_le encap_command;        /* ALWAYS 0x006f Unconnected Send*/
    uint16_le encap_length;         /* packet size in bytes - 24 */
    uint32_le encap_session_handle; /* from session set up */
    uint32_le encap_status;         /* always _sent_ as 0 */
    uint64_le encap_sender_context; /* whatever we want to set this to, used for
                                     * identifying responses when more than one
                                     * are in flight at once.
                                     */
    uint32_le encap_options;        /* 0, reserved for future use */

    /* Interface Handle etc. */
    uint32_le interface_handle; /* ALWAYS 0 */
    uint16_le router_timeout;   /* in seconds */

    /* Common Packet Format - CPF Unconnected */
    uint16_le cpf_item_count;      /* ALWAYS 2 */
    uint16_le cpf_nai_item_type;   /* ALWAYS 0 */
    uint16_le cpf_nai_item_length; /* ALWAYS 0 */
    uint16_le cpf_udi_item_type;   /* ALWAYS 0x00B2 - Unconnected Data Item */
    uint16_le cpf_udi_item_length; /* REQ: fill in with length of remaining data. */

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
} END_PACK eip_forward_open_request_t;


/* Forward Open Request Extended */
START_PACK typedef struct {
    /* encap header */
    uint16_le encap_command;        /* ALWAYS 0x006f Unconnected Send*/
    uint16_le encap_length;         /* packet size in bytes - 24 */
    uint32_le encap_session_handle; /* from session set up */
    uint32_le encap_status;         /* always _sent_ as 0 */
    uint64_le encap_sender_context; /* whatever we want to set this to, used for
                                     * identifying responses when more than one
                                     * are in flight at once.
                                     */
    uint32_le encap_options;        /* 0, reserved for future use */

    /* Interface Handle etc. */
    uint32_le interface_handle; /* ALWAYS 0 */
    uint16_le router_timeout;   /* in seconds */

    /* Common Packet Format - CPF Unconnected */
    uint16_le cpf_item_count;      /* ALWAYS 2 */
    uint16_le cpf_nai_item_type;   /* ALWAYS 0 */
    uint16_le cpf_nai_item_length; /* ALWAYS 0 */
    uint16_le cpf_udi_item_type;   /* ALWAYS 0x00B2 - Unconnected Data Item */
    uint16_le cpf_udi_item_length; /* REQ: fill in with length of remaining data. */

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
} END_PACK eip_forward_open_request_ex_t;


/* Forward Open Response */
START_PACK typedef struct {
    /* encap header */
    uint16_le encap_command;        /* ALWAYS 0x006f Unconnected Send*/
    uint16_le encap_length;         /* packet size in bytes - 24 */
    uint32_le encap_session_handle; /* from session set up */
    uint32_le encap_status;         /* always _sent_ as 0 */
    uint64_le encap_sender_context; /* whatever we want to set this to, used for
                                     * identifying responses when more than one
                                     * are in flight at once.
                                     */
    uint32_le options;              /* 0, reserved for future use */

    /* Interface Handle etc. */
    uint32_le interface_handle; /* ALWAYS 0 */
    uint16_le router_timeout;   /* in seconds */

    /* Common Packet Format - CPF Unconnected */
    uint16_le cpf_item_count;      /* ALWAYS 2 */
    uint16_le cpf_nai_item_type;   /* ALWAYS 0 */
    uint16_le cpf_nai_item_length; /* ALWAYS 0 */
    uint16_le cpf_udi_item_type;   /* ALWAYS 0x00B2 - Unconnected Data Item */
    uint16_le cpf_udi_item_length; /* REQ: fill in with length of remaining data. */

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
} END_PACK eip_forward_open_response_t;


/* Forward Close Request */
START_PACK typedef struct {
    /* encap header */
    uint16_le encap_command;        /* ALWAYS 0x006f Unconnected Send*/
    uint16_le encap_length;         /* packet size in bytes - 24 */
    uint32_le encap_session_handle; /* from session set up */
    uint32_le encap_status;         /* always _sent_ as 0 */
    uint64_le encap_sender_context; /* whatever we want to set this to, used for
                                     * identifying responses when more than one
                                     * are in flight at once.
                                     */
    uint32_le encap_options;        /* 0, reserved for future use */

    /* Interface Handle etc. */
    uint32_le interface_handle; /* ALWAYS 0 */
    uint16_le router_timeout;   /* in seconds */

    /* Common Packet Format - CPF Unconnected */
    uint16_le cpf_item_count;      /* ALWAYS 2 */
    uint16_le cpf_nai_item_type;   /* ALWAYS 0 */
    uint16_le cpf_nai_item_length; /* ALWAYS 0 */
    uint16_le cpf_udi_item_type;   /* ALWAYS 0x00B2 - Unconnected Data Item */
    uint16_le cpf_udi_item_length; /* REQ: fill in with length of remaining data. */

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
} END_PACK eip_forward_close_req_t;


/* Forward Close Response */
START_PACK typedef struct {
    /* encap header */
    uint16_le encap_command;        /* ALWAYS 0x006f Unconnected Send*/
    uint16_le encap_length;         /* packet size in bytes - 24 */
    uint32_le encap_session_handle; /* from session set up */
    uint32_le encap_status;         /* always _sent_ as 0 */
    uint64_le encap_sender_context; /* whatever we want to set this to, used for
                                     * identifying responses when more than one
                                     * are in flight at once.
                                     */
    uint32_le encap_options;        /* 0, reserved for future use */

    /* Interface Handle etc. */
    uint32_le interface_handle; /* ALWAYS 0 */
    uint16_le router_timeout;   /* in seconds */

    /* Common Packet Format - CPF Unconnected */
    uint16_le cpf_item_count;      /* ALWAYS 2 */
    uint16_le cpf_nai_item_type;   /* ALWAYS 0 */
    uint16_le cpf_nai_item_length; /* ALWAYS 0 */
    uint16_le cpf_udi_item_type;   /* ALWAYS 0x00B2 - Unconnected Data Item */
    uint16_le cpf_udi_item_length; /* REQ: fill in with length of remaining data. */

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
} END_PACK eip_forward_close_resp_t;


/* CIP generic connected response */
START_PACK typedef struct {
    /* encap header */
    uint16_le encap_command;        /* ALWAYS 0x0070 Connected Send */
    uint16_le encap_length;         /* packet size in bytes less the header size, which is 24 bytes */
    uint32_le encap_session_handle; /* from session set up */
    uint32_le encap_status;         /* always _sent_ as 0 */
    uint64_le encap_sender_context; /* whatever we want to set this to, used for
                                     * identifying responses when more than one
                                     * are in flight at once.
                                     */
    uint32_le options;              /* 0, reserved for future use */

    /* Interface Handle etc. */
    uint32_le interface_handle; /* ALWAYS 0 */
    uint16_le router_timeout;   /* in seconds, zero for Connected Sends! */

    /* Common Packet Format - CPF Connected */
    uint16_le cpf_item_count;      /* ALWAYS 2 */
    uint16_le cpf_cai_item_type;   /* ALWAYS 0x00A1 Connected Address Item */
    uint16_le cpf_cai_item_length; /* ALWAYS 2 ? */
    uint32_le cpf_targ_conn_id;    /* the connection id from Forward Open */
    uint16_le cpf_cdi_item_type;   /* ALWAYS 0x00B1, Connected Data Item type */
    uint16_le cpf_cdi_item_length; /* length in bytes of the rest of the packet */

    /* Connection sequence number */
    uint16_le cpf_conn_seq_num; /* connection sequence ID, inc for each message */
} END_PACK eip_cip_co_generic_response;


/* CIP "native" Request */
START_PACK typedef struct {
    /* encap header */
    uint16_le encap_command;        /* ALWAYS 0x0070 Connected Send */
    uint16_le encap_length;         /* packet size in bytes less the header size, which is 24 bytes */
    uint32_le encap_session_handle; /* from session set up */
    uint32_le encap_status;         /* always _sent_ as 0 */
    uint64_le encap_sender_context; /* whatever we want to set this to, used for
                                     * identifying responses when more than one
                                     * are in flight at once.
                                     */
    uint32_le options;              /* 0, reserved for future use */

    /* Interface Handle etc. */
    uint32_le interface_handle; /* ALWAYS 0 */
    uint16_le router_timeout;   /* in seconds, zero for Connected Sends! */

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
} END_PACK eip_cip_co_req;


/* CIP Response */
START_PACK typedef struct {
    /* encap header */
    uint16_le encap_command;        /* ALWAYS 0x0070 Connected Send */
    uint16_le encap_length;         /* packet size in bytes less the header size, which is 24 bytes */
    uint32_le encap_session_handle; /* from session set up */
    uint32_le encap_status;         /* always _sent_ as 0 */
    uint64_le encap_sender_context; /* whatever we want to set this to, used for
                                     * identifying responses when more than one
                                     * are in flight at once.
                                     */
    uint32_le options;              /* 0, reserved for future use */

    /* Interface Handle etc. */
    uint32_le interface_handle; /* ALWAYS 0 */
    uint16_le router_timeout;   /* in seconds, zero for Connected Sends! */

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
} END_PACK eip_cip_co_resp;


/* CIP "native" Unconnected Request */
START_PACK typedef struct {
    /* encap header */
    uint16_le encap_command;        /* ALWAYS 0x006f Unconnected Send*/
    uint16_le encap_length;         /* packet size in bytes - 24 */
    uint32_le encap_session_handle; /* from session set up */
    uint32_le encap_status;         /* always _sent_ as 0 */
    uint64_le encap_sender_context; /* whatever we want to set this to, used for
                                     * identifying responses when more than one
                                     * are in flight at once.
                                     */
    uint32_le encap_options;        /* 0, reserved for future use */

    /* Interface Handle etc. */
    uint32_le interface_handle; /* ALWAYS 0 */
    uint16_le router_timeout;   /* in seconds, 5 or 10 seems to be good.*/

    /* Common Packet Format - CPF Unconnected */
    uint16_le cpf_item_count;      /* ALWAYS 2 */
    uint16_le cpf_nai_item_type;   /* ALWAYS 0 */
    uint16_le cpf_nai_item_length; /* ALWAYS 0 */
    uint16_le cpf_udi_item_type;   /* ALWAYS 0x00B2 - Unconnected Data Item */
    uint16_le cpf_udi_item_length; /* REQ: fill in with length of remaining data. */

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


/* CIP "native" Unconnected Response */
START_PACK typedef struct {
    /* encap header */
    uint16_le encap_command;        /* ALWAYS 0x006f Unconnected Send*/
    uint16_le encap_length;         /* packet size in bytes - 24 */
    uint32_le encap_session_handle; /* from session set up */
    uint32_le encap_status;         /* always _sent_ as 0 */
    uint64_le encap_sender_context; /* whatever we want to set this to, used for
                                     * identifying responses when more than one
                                     * are in flight at once.
                                     */
    uint32_le encap_options;        /* 0, reserved for future use */

    /* Interface Handle etc. */
    uint32_le interface_handle; /* ALWAYS 0 */
    uint16_le router_timeout;   /* in seconds, 5 or 10 seems to be good.*/

    /* Common Packet Format - CPF Unconnected */
    uint16_le cpf_item_count;      /* ALWAYS 2 */
    uint16_le cpf_nai_item_type;   /* ALWAYS 0 */
    uint16_le cpf_nai_item_length; /* ALWAYS 0 */
    uint16_le cpf_udi_item_type;   /* ALWAYS 0x00B2 - Unconnected Data Item */
    uint16_le cpf_udi_item_length; /* REQ: fill in with length of remaining data. */

    /* CIP read/write response, embedded packet */
    uint8_t reply_service;    /*  */
    uint8_t reserved;         /* 0x00 in reply */
    uint8_t status;           /* 0x00 for success */
    uint8_t num_status_words; /* number of 16-bit words in status */

} END_PACK eip_cip_uc_resp;
