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

#include <libplctag/protocols/cip/defs.h>
#include <libplctag/protocols/eip/defs.h>
#include <utils/byteorder.h>
#include <utils/macros.h>


#define AB_EIP_PLC5_PARAM ((uint16_t)0x4302)
#define AB_EIP_SLC_PARAM ((uint16_t)0x4302)
#define AB_EIP_LGX_PARAM ((uint16_t)0x43F8)


// 0100 0011 1111 1000
// 0100 001 1 1111 1000
// 0100 001 0 0000 0000  0000 0100 0000 0000
// 0x42000400


#define DEFAULT_MAX_REQUESTS (10) /* number of requests and request sizes to allocate by default. */

/*
 * Hard ceiling on the size of a tag's data buffer.  A PLC can keep returning
 * partial (fragmented) responses forever, and each fragment grows the tag
 * buffer.  Refuse to grow past this so a misbehaving or hostile PLC cannot
 * drive us out of memory.
 */
/*
 * How many consecutive zero-payload fragment responses to accept before giving up.
 *
 * A partial-transfer status with no data is legitimate -- packing several requests into one
 * packet can leave the later ones with only a bare CIP header -- but it makes no progress, and
 * a PLC that sends nothing else keeps us asking for the same fragment forever.  This is high
 * enough that ordinary packing starvation resolves itself and low enough that a stuck transfer
 * fails quickly.
 */
#define MAX_FRAGMENT_RETRIES (100)

#define AB_MAX_TAG_DATA_SIZE (8 * 1024 * 1024)


/* AB Constants*/

/* in milliseconds */

/* AB Commands */

/* AB packet info */

/* specific sub-commands */

/* CIP embedded packet commands */

/* flag set when command is OK */


/* PCCC commands */
#define AB_EIP_PCCC_TYPED_CMD ((uint8_t)0x0F)
#define AB_EIP_PLC5_RANGE_READ_FUNC ((uint8_t)0x01)
#define AB_EIP_PLC5_RANGE_WRITE_FUNC ((uint8_t)0x00)
#define AB_EIP_PLC5_RMW_FUNC ((uint8_t)0x26)
#define AB_EIP_PCCCLGX_TYPED_READ_FUNC ((uint8_t)0x68)
#define AB_EIP_PCCCLGX_TYPED_WRITE_FUNC ((uint8_t)0x67)
#define AB_EIP_SLC_RANGE_READ_FUNC ((uint8_t)0xA2)
#define AB_EIP_SLC_RANGE_WRITE_FUNC ((uint8_t)0xAA)
#define AB_EIP_SLC_RANGE_WRITE_MASK_FUNC ((uint8_t)0xAB)


#define AB_PCCC_DATA_BIT 1
#define AB_PCCC_DATA_BIT_STRING 2
#define AB_PCCC_DATA_BYTE_STRING 3
#define AB_PCCC_DATA_INT 4
#define AB_PCCC_DATA_TIMER 5
#define AB_PCCC_DATA_COUNTER 6
#define AB_PCCC_DATA_CONTROL 7
#define AB_PCCC_DATA_REAL 8
#define AB_PCCC_DATA_ARRAY 9
#define AB_PCCC_DATA_ADDRESS 15
#define AB_PCCC_DATA_BCD 16


/* base data type byte values */
/* OBSOLETE - this is now in cip.c in a table. */

/* aggregate data type byte values */


/* transport class */


// #define AB_EIP_TRANSPORT 0xA3


/* EIP Item Types */


/* Types of AB protocols */
// #define AB_PLC_PLC         (1)
// #define AB_PLC_MLGX        (2)
// #define AB_PLC_LGX         (3)
// #define AB_PLC_MICRO800     (4)
// #define AB_PLC_LGX_PCCC    (5)

typedef enum {
    AB_PLC_NONE = 0,
    AB_PLC_PLC5 = 1,
    AB_PLC_SLC,
    AB_PLC_MLGX,
    AB_PLC_LGX,
    AB_PLC_LGX_PCCC,
    AB_PLC_MICRO800,
    AB_PLC_OMRON_NJNX,
    AB_PLC_GENERIC, /* Generic CIP device access (no PLC-specific protocol) */
    AB_PLC_TYPE_LAST,
} plc_type_t;


/* just the encap header and the unconnected CPF header and items. */
START_PACK typedef struct {
    uint16_le encap_command;        /* EIP command*/
    uint16_le encap_length;         /* payload size in bytes */
    uint32_le encap_session_handle; /* session handle */
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
} END_PACK eip_cpf_uc_header;

/* just the encap header and the connected CPF header and items. */
START_PACK typedef struct {
    uint16_le encap_command;        /* EIP command*/
    uint16_le encap_length;         /* payload size in bytes */
    uint32_le encap_session_handle; /* session handle */
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
    uint16_le cpf_cai_item_type;   /* ALWAYS 0x00A1 Connected Address Item */
    uint16_le cpf_cai_item_length; /* ALWAYS 4 */
    uint32_le cpf_targ_conn_id;    /* the connection id from Forward Open */
    uint16_le cpf_cdi_item_type;   /* ALWAYS 0x00B1, Connected Data Item type */
    uint16_le cpf_cdi_item_length; /* length in bytes of the rest of the packet */

    /* Connection sequence number */
    uint16_le cpf_conn_seq_num; /* connection sequence ID,*/
} END_PACK eip_cpf_co_header;


/* PCCC Request */
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

    /* PCCC Command Req Routing */
    uint8_t service_code;           /* ALWAYS 0x4B, Execute PCCC */
    uint8_t req_path_size;          /* ALWAYS 0x02, in 16-bit words */
    uint8_t req_path[4];            /* ALWAYS 0x20,0x67,0x24,0x01 for PCCC */
    uint8_t request_id_size;        /* ALWAYS 7 */
    uint16_le vendor_id;            /* Our CIP Vendor ID */
    uint32_le vendor_serial_number; /* Our CIP Vendor Serial Number */

    /* PCCC Command */
    uint8_t pccc_command;         /* CMD read, write etc. */
    uint8_t pccc_status;          /* STS 0x00 in request */
    uint16_le pccc_seq_num;       /* TNS transaction/sequence id */
    uint8_t pccc_function;        /* FNC sub-function of command */
    uint16_le pccc_offset;        /* offset of requested in total request */
    uint16_le pccc_transfer_size; /* total number of words requested */
} END_PACK eip_pccc_req_old;


/* PCCC Response */
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

    /* connection ID from request */
    uint16_le cpf_conn_seq_num; /* connection sequence ID, inc for each message */

    /* PCCC Reply */
    uint8_t reply_service;  /* 0xCB Execute PCCC Reply */
    uint8_t reserved;       /* 0x00 in reply */
    uint8_t general_status; /* 0x00 for success */
    uint8_t status_size;    /* number of 16-bit words of extra status, 0 if success */

    /* PCCC Command Req Routing */
    uint8_t request_id_size;        /* ALWAYS 7 */
    uint16_le vendor_id;            /* Our CIP Vendor ID */
    uint32_le vendor_serial_number; /* Our CIP Vendor Serial Number */

    /* PCCC Command */
    uint8_t pccc_command;   /* CMD read, write etc. */
    uint8_t pccc_status;    /* STS 0x00 in request */
    uint16_le pccc_seq_num; /* TNSW transaction/connection sequence number */
} END_PACK eip_pccc_resp_old;


/* PCCC Request PLC5 DH+ Only */
// START_PACK typedef struct {
//     /* encap header */
//     uint16_le encap_command;    /* ALWAYS 0x0070 Connected Send */
//     uint16_le encap_length;   /* packet size in bytes less the header size, which is 24 bytes */
//     uint32_le encap_session_handle;  /* from session set up */
//     uint32_le encap_status;          /* always _sent_ as 0 */
//     uint64_le encap_sender_context;  /* whatever we want to set this to, used for
//                                      * identifying responses when more than one
//                                      * are in flight at once.
//                                      */
//     uint32_le options;               /* 0, reserved for future use */
//
//     /* Interface Handle etc. */
//     uint32_le interface_handle;      /* ALWAYS 0 */
//     uint16_le router_timeout;        /* in seconds, zero for Connected Sends! */
//
//     /* Common Packet Format - CPF Connected */
//     uint16_le cpf_item_count;        /* ALWAYS 2 */
//     uint16_le cpf_cai_item_type;     /* ALWAYS 0x00A1 Connected Address Item */
//     uint16_le cpf_cai_item_length;   /* ALWAYS 2 ? */
//     uint32_le cpf_targ_conn_id;           /* the connection id from Forward Open */
//     uint16_le cpf_cdi_item_type;     /* ALWAYS 0x00B1, Connected Data Item type */
//     uint16_le cpf_cdi_item_length;   /* length in bytes of the rest of the packet */
//
//     /* Connection sequence number */
//     uint16_le cpf_conn_seq_num;      /* connection sequence ID, inc for each message */
//
//     /* PLC5 DH+ Routing */
//     uint16_le dest_link;
//     uint16_le dest_node;
//     uint16_le src_link;
//     uint16_le src_node;
//
//     /* PCCC Command */
//     uint8_t pccc_command;           /* CMD read, write etc. */
//     uint8_t pccc_status;            /* STS 0x00 in request */
//     uint16_le pccc_seq_num;          /* TNSW transaction/sequence id */
//     uint8_t pccc_function;          /* FNC sub-function of command */
//     uint16_le pccc_transfer_offset;           /* offset of this request? */
//     uint16_le pccc_transfer_size;    /* number of elements requested */
// } END_PACK pccc_dhp_co_req;
//


/* PCCC PLC5 DH+ Only Response */
// START_PACK typedef struct {
//     /* encap header */
//     uint16_le encap_command;    /* ALWAYS 0x0070 Connected Send */
//     uint16_le encap_length;   /* packet size in bytes less the header size, which is 24 bytes */
//     uint32_le encap_session_handle;  /* from session set up */
//     uint32_le encap_status;          /* always _sent_ as 0 */
//     uint64_le encap_sender_context;  /* whatever we want to set this to, used for
//                                      * identifying responses when more than one
//                                      * are in flight at once.
//                                      */
//     uint32_le options;               /* 0, reserved for future use */
//
//     /* Interface Handle etc. */
//     uint32_le interface_handle;      /* ALWAYS 0 */
//     uint16_le router_timeout;        /* in seconds, zero for Connected Sends! */
//
//     /* Common Packet Format - CPF Connected */
//     uint16_le cpf_item_count;        /* ALWAYS 2 */
//     uint16_le cpf_cai_item_type;     /* ALWAYS 0x00A1 Connected Address Item */
//     uint16_le cpf_cai_item_length;   /* ALWAYS 2 ? */
//     uint32_le cpf_targ_conn_id;           /* the connection id from Forward Open */
//     uint16_le cpf_cdi_item_type;     /* ALWAYS 0x00B1, Connected Data Item type */
//     uint16_le cpf_cdi_item_length;   /* length in bytes of the rest of the packet */
//
//     /* connection ID from request */
//     uint16_le cpf_conn_seq_num;      /* connection sequence ID, inc for each message */
//
//     /* PLC5 DH+ Routing */
//     uint16_le dest_link;
//     uint16_le dest_node;
//     uint16_le src_link;
//     uint16_le src_node;
//
//     /* PCCC Command */
//     uint8_t pccc_command;           /* CMD read, write etc. */
//     uint8_t pccc_status;            /* STS 0x00 in request */
//     uint16_le pccc_seq_num;         /* TNSW transaction/connection sequence number */
// } END_PACK pccc_dhp_co_resp;


//
// START_PACK typedef struct {
//    /* encap header */
//    uint16_le encap_command;         /* ALWAYS 0x006f Unconnected Send*/
//    uint16_le encap_length;          /* packet size in bytes - 24 */
//    uint32_le encap_session_handle;  /* from session set up */
//    uint32_le encap_status;          /* always _sent_ as 0 */
//    uint64_le encap_sender_context;  /* whatever we want to set this to, used for
//                                     * identifying responses when more than one
//                                     * are in flight at once.
//                                     */
//    uint32_le encap_options;         /* 0, reserved for future use */
//
//    /* Interface Handle etc. */
//    uint32_le interface_handle;      /* ALWAYS 0 */
//    uint16_le router_timeout;        /* in seconds, 5 or 10 seems to be good.*/
//
//    /* Common Packet Format - CPF Unconnected */
//    uint16_le cpf_item_count;        /* ALWAYS 2 */
//    uint16_le cpf_nai_item_type;     /* ALWAYS 0 */
//    uint16_le cpf_nai_item_length;   /* ALWAYS 0 */
//    uint16_le cpf_udi_item_type;     /* ALWAYS 0x00B2 - Unconnected Data Item */
//    uint16_le cpf_udi_item_length;   /* REQ: fill in with length of remaining data. */
//
//    /* PCCC Command Req Routing */
//    uint8_t service_code;           /* ALWAYS 0x4B, Execute PCCC */
//    uint8_t req_path_size;          /* ALWAYS 0x02, in 16-bit words */
//    uint8_t req_path[4];            /* ALWAYS 0x20,0x67,0x24,0x01 for PCCC */
//    uint8_t request_id_size;        /* ALWAYS 7 */
//    uint16_le vendor_id;             /* Our CIP Vendor ID */
//    uint32_le vendor_serial_number;  /* Our CIP Vendor Serial Number */
//
//    /* PCCC Command */
//    uint8_t pccc_command;           /* CMD read, write etc. */
//    uint8_t pccc_status;            /* STS 0x00 in request */
//    uint16_le pccc_seq_num;          /* TNS transaction/sequence id */
//    uint8_t pccc_function;          /* FNC sub-function of command */
////    uint16_le pccc_offset;           /* offset of requested in total request */
//    uint8_t pccc_transfer_size;    /* total number of bytes requested */
//} END_PACK pccc_req;
//


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

    /* PCCC Reply */
    uint8_t reply_code;     /* 0xCB Execute PCCC Reply */
    uint8_t reserved;       /* 0x00 in reply */
    uint8_t general_status; /* 0x00 for success */
    uint8_t status_size;    /* number of 16-bit words of extra status, 0 if success */

    /* PCCC Command Req Routing */
    uint8_t request_id_size;        /* ALWAYS 7 counting*/
    uint16_le vendor_id;            /* Our CIP Vendor ID */
    uint32_le vendor_serial_number; /* Our CIP Vendor Serial Number */

    /* PCCC Command */
    uint8_t pccc_command;   /* CMD read, write etc. */
    uint8_t pccc_status;    /* STS 0x00 in request */
    uint16_le pccc_seq_num; /* TNSW transaction/connection sequence number */
} END_PACK pccc_resp;


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
    uint8_t cm_req_path[6];   /* ALWAYS 0x20,0x06,0x24,0x01 for CM, instance 1*/

    /* Unconnected send */
    uint8_t secs_per_tick; /* seconds per tick */
    uint8_t timeout_ticks; /* timeout = src_secs_per_tick * src_timeout_ticks */

    /* size ? */
    uint16_le uc_cmd_length; /* length of embedded packet */

    /* needed when talking to PLC5 over DH+ */
    uint16_le dest_link;
    uint16_le dest_node;
    uint16_le src_link;
    uint16_le src_node;

    /* PCCC Command */
    uint8_t pccc_command;         /* CMD read, write etc. */
    uint8_t pccc_status;          /* STS 0x00 in request */
    uint16_le pccc_seq_num;       /* TNS transaction/sequence id */
    uint8_t pccc_function;        /* FNC sub-function of command */
    uint16_le pccc_offset;        /* offset of requested in total request */
    uint16_le pccc_transfer_size; /* total number of words requested */

    /* IOI path to DHRIO */
} END_PACK pccc_dhp_req;


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

    /* PCCC Reply */
    uint8_t reply_code;     /* 0xCB Execute PCCC Reply */
    uint8_t reserved;       /* 0x00 in reply */
    uint8_t general_status; /* 0x00 for success */
    uint8_t status_size;    /* number of 16-bit words of extra status, 0 if success */

    /* PCCC Command Req Routing */
    uint8_t request_id_size;        /* ALWAYS 7 */
    uint16_le vendor_id;            /* Our CIP Vendor ID */
    uint32_le vendor_serial_number; /* Our CIP Vendor Serial Number */

    /* PCCC Command */
    uint8_t pccc_command;   /* CMD read, write etc. */
    uint8_t pccc_status;    /* STS 0x00 in request */
    uint16_le pccc_seq_num; /* TNSW transaction/connection sequence number */
} END_PACK pccc_dhp_resp;
