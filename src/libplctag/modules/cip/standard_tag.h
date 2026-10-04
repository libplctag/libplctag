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
 * The read/write engine for ordinary (symbolic, named) CIP tags.
 *
 * One implementation serves every CIP family.  The request shapes are the same CIP tag
 * services everywhere; what differs is which of them a family implements and how much it
 * can move at once, and the family states that in the ops table below.  This file never
 * asks which family a tag belongs to.
 *
 * Each family exports its own vtable because the abort entry point still differs.
 */

#include <libplctag/lib/tag.h>
#include <stdint.h>


/* a family that cannot split a transfer sets max_transfer_packets to this. */
#define CIP_TRANSFER_SINGLE_PACKET ((int32_t)1)

/* a family with no limit of its own; the payload budget is what bounds it. */
#define CIP_TRANSFER_PACKETS_UNLIMITED ((int32_t)INT32_MAX)


/*
 * What a CIP family can do, stated as values rather than discovered by asking who it is.
 *
 * Every slot is filled.  There is no "NULL means use the default", because a default is a
 * branch in the engine and a silent inheritance for whoever adds the next family: a
 * missing answer should be a question someone has to answer, not one the engine answers
 * on their behalf.
 */
typedef struct cip_standard_tag_ops_t {
    /*
     * The services this family reads and writes with.  Rockwell uses the fragmented read
     * whatever the offset, because the plain Read Tag service carries no offset and so
     * cannot continue a transfer the PLC split across replies.  A family with no
     * fragmented service names the plain one here and sets max_transfer_packets to
     * CIP_TRANSFER_SINGLE_PACKET, which is what stops the engine ever needing one.
     */
    uint8_t read_service;
    uint8_t write_service;       /* the whole tag fits in one request */
    uint8_t write_service_split; /* it does not; unreachable at CIP_TRANSFER_SINGLE_PACKET */

    /*
     * How many packets one transfer may span.  This is the whole of what used to be asked
     * as "does this family support fragments": it decides whether a split write is
     * attempted or refused, and whether a partial-transfer status means "more coming" or
     * is an error the PLC should never have sent.
     */
    int32_t max_transfer_packets;

    /*
     * Append the read request's byte offset, if this family's read service carries one.
     * Always present: cip_encode_no_offset() is cheaper than testing for NULL, and it
     * keeps the offset bound to the service code that requires it -- a fragmented service
     * without its offset is a malformed packet, and the two are set side by side here.
     *
     * Returns the number of bytes written, or a negative status code.
     */
    int32_t (*encode_read_offset)(uint8_t *dest, int32_t capacity, int32_t byte_offset);
} cip_standard_tag_ops_t;


/* the two offset encoders; a family names whichever its read service needs. */
extern int32_t cip_encode_offset_u32(uint8_t *dest, int32_t capacity, int32_t byte_offset);
extern int32_t cip_encode_no_offset(uint8_t *dest, int32_t capacity, int32_t byte_offset);


/*
 * The three vtable entry points.  Each family builds its own vtable around these, because
 * the abort entry point and the attribute table are still the family's own.
 */
extern int cip_standard_tag_read_start(plc_tag_p tag_arg);
extern int cip_standard_tag_write_start(plc_tag_p tag_arg);
extern int cip_standard_tag_tickler(plc_tag_p tag_arg);
