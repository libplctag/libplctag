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

#include <libplctag/modules/cip/wire.h>
#include <utils/byteorder.h>


#define AB_EIP_PLC5_PARAM ((uint16_t)0x4302)
#define AB_EIP_SLC_PARAM ((uint16_t)0x4302)
#define AB_EIP_LGX_PARAM ((uint16_t)0x43F8)


#define AB_EIP_CONN_PARAM ((uint16_t)0x4200)
// 0100 0011 1111 1000
// 0100 001 1 1111 1000
#define AB_EIP_CONN_PARAM_EX ((uint32_t)0x42000000)
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
#define AB_EIP_OK (0)
#define AB_EIP_VERSION ((uint16_t)0x0001)

/* in milliseconds */
#define AB_EIP_DEFAULT_TIMEOUT 2000 /* in ms */

/* AB Commands */
#define AB_EIP_REGISTER_SESSION ((uint16_t)0x0065)
#define AB_EIP_UNREGISTER_SESSION ((uint16_t)0x0066)
#define AB_EIP_UNCONNECTED_SEND ((uint16_t)0x006F)
#define AB_EIP_CONNECTED_SEND ((uint16_t)0x0070)

/* AB packet info */
#define AB_EIP_DEFAULT_PORT 44818

/* specific sub-commands */
#define AB_EIP_CMD_PCCC_EXECUTE ((uint8_t)0x4B)
#define AB_EIP_CMD_FORWARD_CLOSE ((uint8_t)0x4E)
#define AB_EIP_CMD_UNCONNECTED_SEND ((uint8_t)0x52)
#define AB_EIP_CMD_FORWARD_OPEN ((uint8_t)0x54)
#define AB_EIP_CMD_FORWARD_OPEN_EX ((uint8_t)0x5B)

/* CIP embedded packet commands */
#define AB_EIP_CMD_CIP_GET_ATTR_LIST ((uint8_t)0x03)
#define AB_EIP_CMD_CIP_MULTI ((uint8_t)0x0A)
#define AB_EIP_CMD_CIP_READ ((uint8_t)0x4C)
#define AB_EIP_CMD_CIP_WRITE ((uint8_t)0x4D)
#define AB_EIP_CMD_CIP_RMW ((uint8_t)0x4E)
#define AB_EIP_CMD_CIP_READ_FRAG ((uint8_t)0x52)
#define AB_EIP_CMD_CIP_WRITE_FRAG ((uint8_t)0x53)
#define AB_EIP_CMD_CIP_LIST_TAGS ((uint8_t)0x55)

/* flag set when command is OK */
#define AB_EIP_CMD_CIP_OK ((uint8_t)0x80)

#define AB_CIP_STATUS_OK ((uint8_t)0x00)
#define AB_CIP_STATUS_FRAG ((uint8_t)0x06)

#define AB_CIP_ERR_UNSUPPORTED_SERVICE ((uint8_t)0x08)
#define AB_CIP_ERR_PARTIAL_ERROR ((uint8_t)0x1e)

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
#define AB_CIP_DATA_DT ((uint8_t)0xC0)            /* DT value, 64 bit */
#define AB_CIP_DATA_BIT ((uint8_t)0xC1)           /* Boolean value, 1 bit */
#define AB_CIP_DATA_SINT ((uint8_t)0xC2)          /* Signed 8–bit integer value */
#define AB_CIP_DATA_INT ((uint8_t)0xC3)           /* Signed 16–bit integer value */
#define AB_CIP_DATA_DINT ((uint8_t)0xC4)          /* Signed 32–bit integer value */
#define AB_CIP_DATA_LINT ((uint8_t)0xC5)          /* Signed 64–bit integer value */
#define AB_CIP_DATA_USINT ((uint8_t)0xC6)         /* Unsigned 8–bit integer value */
#define AB_CIP_DATA_UINT ((uint8_t)0xC7)          /* Unsigned 16–bit integer value */
#define AB_CIP_DATA_UDINT ((uint8_t)0xC8)         /* Unsigned 32–bit integer value */
#define AB_CIP_DATA_ULINT ((uint8_t)0xC9)         /* Unsigned 64–bit integer value */
#define AB_CIP_DATA_REAL ((uint8_t)0xCA)          /* 32–bit floating point value, IEEE format */
#define AB_CIP_DATA_LREAL ((uint8_t)0xCB)         /* 64–bit floating point value, IEEE format */
#define AB_CIP_DATA_STIME ((uint8_t)0xCC)         /* Synchronous time value */
#define AB_CIP_DATA_DATE ((uint8_t)0xCD)          /* Date value */
#define AB_CIP_DATA_TIME_OF_DAY ((uint8_t)0xCE)   /* Time of day value */
#define AB_CIP_DATA_DATE_AND_TIME ((uint8_t)0xCF) /* Date and time of day value */
#define AB_CIP_DATA_STRING ((uint8_t)0xD0)        /* Character string, 1 byte per character */
#define AB_CIP_DATA_BYTE ((uint8_t)0xD1)          /* 8-bit bit string */
#define AB_CIP_DATA_WORD ((uint8_t)0xD2)          /* 16-bit bit string */
#define AB_CIP_DATA_DWORD ((uint8_t)0xD3)         /* 32-bit bit string */
#define AB_CIP_DATA_LWORD ((uint8_t)0xD4)         /* 64-bit bit string */
#define AB_CIP_DATA_STRING2 ((uint8_t)0xD5)       /* Wide char character string, 2 bytes per character */
#define AB_CIP_DATA_FTIME ((uint8_t)0xD6)         /* High resolution duration value */
#define AB_CIP_DATA_LTIME ((uint8_t)0xD7)         /* Medium resolution duration value */
#define AB_CIP_DATA_ITIME ((uint8_t)0xD8)         /* Low resolution duration value */
#define AB_CIP_DATA_STRINGN ((uint8_t)0xD9)       /* N-byte per char character string */
#define AB_CIP_DATA_SHORT_STRING \
    ((uint8_t)0xDA)                         /* Counted character sting with 1 byte per character and 1 byte length indicator */
#define AB_CIP_DATA_TIME ((uint8_t)0xDB)    /* Duration in milliseconds */
#define AB_CIP_DATA_EPATH ((uint8_t)0xDC)   /* CIP path segment(s) */
#define AB_CIP_DATA_ENGUNIT ((uint8_t)0xDD) /* Engineering units */
#define AB_CIP_DATA_STRINGI ((uint8_t)0xDE) /* International character string (encoding?) */

/* aggregate data type byte values */
#define AB_CIP_DATA_ABREV_STRUCT \
    ((uint8_t)0xA0)                             /* Data is an abbreviated struct type, i.e. a CRC of the actual type descriptor */
#define AB_CIP_DATA_ABREV_ARRAY ((uint8_t)0xA1) /* Data is an abbreviated array type. The limits are left off */
#define AB_CIP_DATA_FULL_STRUCT ((uint8_t)0xA2) /* Data is a struct type descriptor */
#define AB_CIP_DATA_FULL_ARRAY ((uint8_t)0xA3)  /* Data is an array type descriptor */


/* transport class */
#define AB_EIP_TRANSPORT_CLASS_T3 ((uint8_t)0xA3)


#define AB_EIP_SECS_PER_TICK 0x0A
#define AB_EIP_TIMEOUT_TICKS 0x0E
#define AB_EIP_VENDOR_ID 0xF33D     /*tres 1337 */
#define AB_EIP_VENDOR_SN 0x21504345 /* the string !PCE */
#define AB_EIP_TIMEOUT_MULTIPLIER 0x03
#define AB_EIP_RPI 1000000 /* in microseconds */

#define AB_EIP_CONN_TIMEOUT_MS ((AB_EIP_RPI * 4 * (1 << AB_EIP_TIMEOUT_MULTIPLIER)) / 1000)

// #define AB_EIP_TRANSPORT 0xA3


/* EIP Item Types */
#define AB_EIP_ITEM_NAI ((uint16_t)0x0000) /* NULL Address Item */
#define AB_EIP_ITEM_CAI ((uint16_t)0x00A1) /* connected address item */
#define AB_EIP_ITEM_CDI ((uint16_t)0x00B1) /* connected data item */
#define AB_EIP_ITEM_UDI ((uint16_t)0x00B2) /* Unconnected data item */


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
} ab_plc_type_t;


/*********************************************************************
 ************************ AB EIP Structures **************************
 ********************************************************************/


/* EIP Encapsulation Header */


/* Session Registration Request */


/* just the encap header and the unconnected CPF header and items. */


/* just the encap header and the connected CPF header and items. */


/* Forward Open Request */


/* Forward Open Request Extended */


/* Forward Open Response */


/* Forward Close Request */


/* Forward Close Response */


/* CIP generic connected response */


/* PCCC Request */


/* PCCC Response */


/* PCCC Request PLC5 DH+ Only */
//
//


/* PCCC PLC5 DH+ Only Response */
//


/* CIP "native" Request */


/* CIP Response */


/* CIP "native" Unconnected Request */


/* CIP "native" Unconnected Response */


//
//
//
