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
#include <stdint.h>

#include "compat.h"
#include "mutex.h"

typedef uint16_t tag_type_t;

/* CIP data types. */
#define TAG_CIP_TYPE_BOOL ((tag_type_t)0x00C1)  /* 8-bit boolean value */
#define TAG_CIP_TYPE_SINT ((tag_type_t)0x00C2)  /* Signed 8–bit integer value */
#define TAG_CIP_TYPE_INT ((tag_type_t)0x00C3)   /* Signed 16–bit integer value */
#define TAG_CIP_TYPE_DINT ((tag_type_t)0x00C4)  /* Signed 32–bit integer value */
#define TAG_CIP_TYPE_LINT ((tag_type_t)0x00C5)  /* Signed 64–bit integer value */
#define TAG_CIP_TYPE_USINT ((tag_type_t)0x00C6) /* Unsigned 8–bit integer value */
#define TAG_CIP_TYPE_UINT ((tag_type_t)0x00C7)  /* Unsigned 16–bit integer value */
#define TAG_CIP_TYPE_UDINT ((tag_type_t)0x00C8) /* Unsigned 32–bit integer value */
#define TAG_CIP_TYPE_ULINT ((tag_type_t)0x00C9) /* Unsigned 64–bit integer value */
#define TAG_CIP_TYPE_REAL ((tag_type_t)0x00CA)  /* 32–bit floating point value, IEEE format */
#define TAG_CIP_TYPE_LREAL ((tag_type_t)0x00CB) /* 64–bit floating point value, IEEE format */
#define TAG_CIP_TYPE_STRING ((tag_type_t)0x00D0) /* CIP STRING: 2-byte count word + 82 chars = 84 bytes. */

/*
 * SHORT_STRING is a 1-byte count followed by that many characters, and it is *not* fixed
 * length: a real Micro800 returns only the count byte and the valid characters, so the size on
 * the wire varies per string.  The count is one byte, so 255 characters is the hard ceiling
 * (the PLC's own internal limit is lower but undocumented).  This simulator stores each element
 * in a full-size slot rather than packing them, so the slot is the worst case.
 */
#define TAG_CIP_TYPE_SHORT_STRING ((tag_type_t)0x00DA)

/*
 * Logix "STRING" is not a CIP type at all -- it is a UDT, and the wire encoding is a structure
 * marker rather than a type code:
 *
 *     A0 02 CE 0F
 *      |  |  \--/-- struct handle, 0x0FCE, which is also the UDT's CIP object ID.
 *      |  \-------- two bytes of handle follow.
 *      \----------- this is an abbreviated struct type.
 *
 * The UDT is { DINT LEN at offset 0; SINT DATA[82] at offset 4 } == 88 bytes.  Control- and
 * CompactLogix use this; Micro800 uses SHORT_STRING and Omron uses the CIP STRING above.
 */
#define TAG_CIP_TYPE_ABBREV_STRUCT ((uint8_t)0xA0)
#define TAG_CIP_STRUCT_HANDLE_STRING ((uint16_t)0x0FCE)

#define TAG_CIP_SIZE_STRING (84)
#define TAG_CIP_SIZE_SHORT_STRING (256) /* 1 count byte + up to 255 characters. */
#define TAG_CIP_SIZE_LOGIX_STRING (88)

/* longest encoded type is the 4-byte abbreviated struct above. */
#define TAG_TYPE_INFO_MAX (4)

/* PCCC data types.   FIXME */
#define TAG_PCCC_TYPE_BIT ((uint8_t)0x85)    /* 1-bit boolean value as unsigned 16-bit integer */
#define TAG_PCCC_TYPE_INT ((uint8_t)0x89)    /* Signed 16–bit integer value */
#define TAG_PCCC_TYPE_DINT ((uint8_t)0x91)   /* Signed 32–bit integer value */
#define TAG_PCCC_TYPE_REAL ((uint8_t)0x8a)   /* 32–bit floating point value, IEEE format */
/*
 * The PCCC/DF1 string is its own type, unrelated to the CIP STRING above: a 2-byte count word
 * followed by 82 characters, 84 bytes in all, and the characters are byte-swapped within each
 * word.  The library's matching definition is in eip_plc5_pccc.c / eip_slc_pccc.c, and the
 * field layout is the ST file in pccc.c's file-type table.
 */
#define TAG_PCCC_TYPE_STRING ((uint8_t)0x8d)
#define TAG_PCCC_SIZE_STRING (84)

/*
 * Deliberate corruption of outgoing responses, so the client's response-validation code can be
 * tested.  The other fault-injection flags (--reject_fo, --reject_size, --empty_frag, --delay)
 * all send well-formed responses that merely say "no"; these send malformed ones.  The
 * machinery that arms and fires them lives in fault.c; the enum is here because plc_s below
 * needs one counter per kind.
 *
 * Note what is *not* here: there is no way to make the packet shorter than an EIP header.  The
 * client sizes its second read from the encapsulation length field, so a response that is
 * simply cut short leaves the client waiting for bytes that never arrive and it times out
 * rather than rejecting anything.  Every corruption below therefore keeps the packet coherent
 * at every layer outside the one being attacked -- see FAULT_SHORT_CPF and FAULT_SHORT_CIP,
 * which shrink the response but fix up the lengths around it.
 */
typedef enum {
    FAULT_NONE = 0,
    FAULT_CPF_COUNT,  /* claim a CPF item count other than two. */
    FAULT_CPF_TYPE,   /* claim an address item type that is neither CAI nor NAI. */
    FAULT_CONN_ID,    /* answer a connected send with the wrong connection ID. */
    FAULT_ITEM_LEN,   /* declare a CPF data item length that disagrees with the packet. */
    FAULT_SHORT_CPF,  /* end the response right after the CPF header, lengths adjusted to match. */
    FAULT_SHORT_CIP,  /* leave a CIP payload too short to hold a CIP response header. */
    FAULT_EIP_CMD,    /* answer with a different EIP command than the one sent. */
    FAULT_SESSION,    /* answer with a session handle other than the registered one. */
    FAULT_CONTEXT,    /* echo back a sender context other than the one that was sent. */
    FAULT_PCCC_REPLY, /* answer a PCCC request with an unexpected reply code. */
    FAULT_PCCC_TNS,   /* answer a PCCC request with a transaction number we never sent. */
    FAULT_MAX
} fault_kind_t;

struct tag_def_s {
    struct tag_def_s *next_tag;
    char *name;
    tag_type_t tag_type;

    /*
     * The encoded type exactly as it goes on the wire, because it is not always derivable from
     * tag_type: an atomic type is its type byte plus a pad byte, but a structure is a 4-byte
     * abbreviated-struct descriptor with no single type code.  Read responses emit these bytes
     * and write requests must match them.  Unused by the PCCC tags, which have their own types.
     */
    uint8_t type_info[TAG_TYPE_INFO_MAX];
    size_t type_info_size;

    size_t elem_size;
    size_t elem_count;
    size_t data_file_num;
    size_t num_dimensions;
    size_t dimensions[3];
    uint8_t *data;
    /* Note we make a big simplifying assumption that the only access to the tag requiring thread
       protection, is to the data. The rest of the fields (the list itself, and the tags' names
       and types) are expected to be created once, in a single thread. From then on those fields
       are expected to be read-only (even if by multiple threads). */
    mutex_p data_mutex;

    /* Fairness tracking - per-request latency statistics */
    atomic_int32_t request_count;
    atomic_int64_t total_latency_us;     /* sum of all request latencies in microseconds */
    atomic_int64_t min_latency_us;       /* minimum request latency */
    atomic_int64_t max_latency_us;       /* maximum request latency */
    atomic_int64_t last_request_time_us; /* timestamp when request arrived */
};

typedef struct tag_def_s tag_def_s;

/*
 * PLC_LGX_PCCC is a ControlLogix reached through the PCCC mapping: CIP framing and a CIP
 * routing path to the CPU, but PCCC data files and PCCC typed read/write commands on top.
 */
typedef enum { PLC_CONTROL_LOGIX, PLC_MICRO800, PLC_OMRON, PLC_PLC5, PLC_SLC, PLC_MICROLOGIX, PLC_LGX_PCCC } plc_type_t;

/* Define the context that is passed around. */
typedef struct plc_s {
    plc_type_t plc_type;
    const char *port_str;
    uint8_t path[20];
    uint8_t path_len;

    /* connection info. */
    uint32_t session_handle;
    uint64_t sender_context;
    uint32_t server_connection_id;
    uint16_t server_connection_seq;
    uint32_t server_to_client_rpi;
    uint32_t client_connection_id;
    uint16_t client_connection_seq;
    uint16_t client_connection_serial_number;
    uint16_t client_vendor_id;
    uint32_t client_serial_number;
    uint32_t client_to_server_rpi;

    uint32_t client_to_server_max_packet;
    uint32_t server_to_client_max_packet;

    /* PCCC info */
    uint16_t pccc_seq_id;

    /*
     * debugging. Points at a single atomic_int32_t shared by every
     * connection's copy of this struct (see tcp_server.c: each accepted
     * connection gets its own memcpy'd plc_s), so the count of remaining
     * ForwardOpen rejections persists across the client reconnecting with a
     * new TCP session on every retry, instead of resetting to the original
     * CLI value each time.
     */
    atomic_int32_t *reject_fo_count;

    /*
     * As above: shared across every connection's copy of this struct.
     *
     * empty_frag_count makes the next N read responses come back with a partial-transfer
     * status and no payload at all.  That is what a real PLC sends when several requests are
     * packed into one packet and the earlier ones consume all the room, so it exercises the
     * client's handling of a legitimate response that makes no forward progress.
     *
     * reject_size_count makes the next N ForwardOpen requests fail with extended status
     * 0x0109 (connection size not supported) and offer a smaller size, which is how a real
     * PLC negotiates the client down.
     */
    atomic_int32_t *empty_frag_count;
    atomic_int32_t *reject_size_count;

    /* size offered back with the 0x0109 rejection above. */
    uint16_t reject_size_supported;

    /*
     * One counter per fault_kind_t, for the deliberate response corruptions behind --corrupt.
     * Shared across every connection's copy of this struct, as above.
     */
    atomic_int32_t *fault_counts[FAULT_MAX];

    /* response delay */
    int response_delay;

    /* list of tags served by this "PLC" */
    struct tag_def_s *tags;
} plc_s;
