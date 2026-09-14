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
 * EtherNet/IP encapsulation constants.
 *
 * Split out of ab/defs.h and omron/defs.h, which carried byte-identical copies
 * under AB_EIP_ and OMRON_EIP_ prefixes.  These are defined by the EtherNet/IP
 * specification, not by the device at the far end.
 */

#include <stdint.h>

#include <utils/byteorder.h>
#include <utils/macros.h>

#define EIP_OK (0)
#define EIP_VERSION ((uint16_t)0x0001)
#define EIP_DEFAULT_TIMEOUT 2000 /* in ms */
#define EIP_REGISTER_SESSION ((uint16_t)0x0065)
#define EIP_UNREGISTER_SESSION ((uint16_t)0x0066)
#define EIP_UNCONNECTED_SEND ((uint16_t)0x006F)
#define EIP_CONNECTED_SEND ((uint16_t)0x0070)
#define EIP_DEFAULT_PORT 44818
#define EIP_ITEM_NAI ((uint16_t)0x0000) /* NULL Address Item */
#define EIP_ITEM_CAI ((uint16_t)0x00A1) /* connected address item */
#define EIP_ITEM_CDI ((uint16_t)0x00B1) /* connected data item */
#define EIP_ITEM_UDI ((uint16_t)0x00B2) /* Unconnected data item */


/*********************************************************************
 ** EtherNet/IP encapsulation and Common Packet Format
 **
 ** These were carried in identical copies in ab/defs.h and omron/defs.h.
 *********************************************************************/


/* EIP Encapsulation Header */
START_PACK typedef struct {
    uint16_le encap_command;
    uint16_le encap_length;
    uint32_le encap_session_handle;
    uint32_le encap_status;
    uint64_le encap_sender_context;
    uint32_le encap_options;
} END_PACK eip_encap;


/* Session Registration Request */
START_PACK typedef struct {
    /* encap header */
    uint16_le encap_command;        /* ALWAYS 0x0065 Register Session*/
    uint16_le encap_length;         /* packet size in bytes - 24 */
    uint32_le encap_session_handle; /* from session set up */
    uint32_le encap_status;         /* always _sent_ as 0 */
    uint64_le encap_sender_context; /* whatever we want to set this to, used for
                                     * identifying responses when more than one
                                     * are in flight at once.
                                     */
    uint32_le encap_options;        /* 0, reserved for future use */

    /* session registration request */
    uint16_le eip_version;
    uint16_le option_flags;
} END_PACK eip_session_reg_req;


START_PACK typedef struct {
    uint16_le cpf_nai_item_type;   /* ALWAYS 0 */
    uint16_le cpf_nai_item_length; /* ALWAYS 0 */
} END_PACK cpf_unconnected_addr_item;


START_PACK typedef struct {
    uint16_le cpf_udi_item_type;   /* ALWAYS 0x00B2 - Unconnected Data Item */
    uint16_le cpf_udi_item_length; /* REQ: fill in with length of remaining data. */
} END_PACK cpf_unconnected_data_item;


START_PACK typedef struct {
    uint16_le cpf_cai_item_type;   /* ALWAYS 0x00A1 Connected Address Item */
    uint16_le cpf_cai_item_length; /* ALWAYS 4 */
    uint32_le cpf_targ_conn_id;    /* the connection id from Forward Open */
} END_PACK cpf_connected_addr_item;


START_PACK typedef struct {
    uint16_le cpf_cdi_item_type;   /* ALWAYS 0x00B1, Connected Data Item type */
    uint16_le cpf_cdi_item_length; /* length in bytes of the rest of the packet */

    /* Connection sequence number */
    uint16_le cpf_conn_seq_num; /* connection sequence ID,*/
} END_PACK cpf_connected_data_item;


/*
 * The encapsulation header plus the CPF items, without the CIP request behind
 * them.  Used to measure a built request's payload without caring what kind of
 * CIP message it carries.
 */

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
