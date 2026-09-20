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

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "modbus_fault.h"

/*
 * A built Modbus/TCP response, by byte offset.  The mutations below reach
 * into the buffer at these positions rather than rebuilding the header,
 * because the point is to produce something modbus_build_response_header()
 * would never produce.
 */
#define OFF_TXN_ID (0)
#define OFF_PROTO_ID (2)
#define OFF_LENGTH (4)
#define OFF_UNIT_ID (6)
#define OFF_FUNC_CODE (7)
#define OFF_BYTE_COUNT (8)

/* indexed by modbus_fault_kind_t, so this must stay in the same order as the enum. */
static const char *fault_names[MODBUS_FAULT_MAX] = {
    "none", "txn_id", "proto_id", "unit_id", "func_code", "length", "short_pdu", "exception", "byte_count", "close",
    "delay", "dribble",
};

/*
 * One counter per kind.  Plain int32_t rather than atomics: this server runs
 * one coroutine thread, so unlike ab_server there is no second connection
 * racing for the same count.  See the note in modbus_fault.h.
 */
static int32_t fault_counts[MODBUS_FAULT_MAX] = {0};


const char *modbus_fault_kind_name(modbus_fault_kind_t kind) {
    if(kind <= MODBUS_FAULT_NONE || kind >= MODBUS_FAULT_MAX) { return "none"; }

    return fault_names[kind];
}


const char **modbus_fault_kind_names(void) {
    static const char *names[MODBUS_FAULT_MAX] = {0};
    int32_t kind = 0;

    /* skip "none", which is not a thing anyone can arm */
    for(kind = MODBUS_FAULT_NONE + 1; kind < MODBUS_FAULT_MAX; kind++) { names[kind - 1] = fault_names[kind]; }

    names[MODBUS_FAULT_MAX - 1] = NULL;

    return names;
}


bool modbus_fault_parse(const char *spec) {
    const char *colon = NULL;
    size_t name_len = 0;
    long count = 1;
    int32_t kind = 0;

    if(!spec) { return false; }

    /*
     * "<kind>" or "<kind>:<count>".  The count is separated with a colon
     * rather than another "=" because args_parse() has already split the
     * argument at the first "=".
     */
    colon = strchr(spec, ':');
    name_len = colon ? (size_t)(colon - spec) : strlen(spec);

    if(colon) {
        char *end = NULL;

        /* where long is 32 bits strtol() saturates at LONG_MAX == INT32_MAX, so the
         * range check below cannot see an overflow on its own. */
        errno = 0;

        count = strtol(colon + 1, &end, 10);

        if(errno == ERANGE || end == colon + 1 || *end != '\0' || count < 1 || count > INT32_MAX) {
            pdlog(LOG_MODULE_MODBUS_FAULT, LOG_LEVEL_ERROR, "Fault count in \"%s\" must be a positive number.", spec);
            return false;
        }
    }

    for(kind = MODBUS_FAULT_NONE + 1; kind < MODBUS_FAULT_MAX; kind++) {
        if(strlen(fault_names[kind]) != name_len || strncmp(spec, fault_names[kind], name_len) != 0) { continue; }

        fault_counts[kind] = (int32_t)count;

        if(kind == MODBUS_FAULT_DELAY) {
            pdlog(LOG_MODULE_MODBUS_FAULT, LOG_LEVEL_INFO, "Delaying every response by %" PRId32 "ms.", (int32_t)count);
        } else if(kind == MODBUS_FAULT_DRIBBLE) {
            pdlog(LOG_MODULE_MODBUS_FAULT, LOG_LEVEL_INFO, "Sending every response in %" PRId32 " byte pieces.",
                  (int32_t)count);
        } else {
            pdlog(LOG_MODULE_MODBUS_FAULT, LOG_LEVEL_INFO, "Corrupting the next %" PRId32 " response(s) with fault \"%s\".",
                  (int32_t)count, fault_names[kind]);
        }

        return true;
    }

    pdlog(LOG_MODULE_MODBUS_FAULT, LOG_LEVEL_ERROR, "Unknown fault kind in \"%s\".", spec);

    return false;
}


bool modbus_fault_fires(modbus_fault_kind_t kind) {
    if(kind <= MODBUS_FAULT_NONE || kind >= MODBUS_FAULT_MAX) { return false; }

    if(fault_counts[kind] <= 0) { return false; }

    fault_counts[kind]--;

    pdlog(LOG_MODULE_MODBUS_FAULT, LOG_LEVEL_INFO, "Injecting fault \"%s\", %" PRId32 " to go.", fault_names[kind],
          fault_counts[kind]);

    return true;
}


int32_t modbus_fault_delay_ms(void) {
    /* read without consuming: a delay applies for as long as it is armed */
    return fault_counts[MODBUS_FAULT_DELAY] > 0 ? fault_counts[MODBUS_FAULT_DELAY] : 0;
}


int32_t modbus_fault_dribble_bytes(void) {
    return fault_counts[MODBUS_FAULT_DRIBBLE] > 0 ? fault_counts[MODBUS_FAULT_DRIBBLE] : 0;
}


/* the MBAP length field, as the response currently declares it */
static uint16_t response_length_field(const buf_t *response) {
    const uint8_t *data = response->data;

    return (uint16_t)(((uint16_t)data[OFF_LENGTH] << 8) | (uint16_t)data[OFF_LENGTH + 1]);
}


static void set_response_length_field(buf_t *response, uint16_t length) {
    uint8_t *data = response->data;

    data[OFF_LENGTH] = (uint8_t)((length >> 8) & 0xFF);
    data[OFF_LENGTH + 1] = (uint8_t)(length & 0xFF);
}


void modbus_fault_apply_response(buf_t *response, const mbap_header_t *req_header) {
    uint8_t *data = NULL;
    size_t length = 0;

    if(!response || !req_header) { return; }

    data = response->data;
    length = response->write;

    /*
     * Every mutation below writes a fixed offset, so a response too short to
     * hold the field being attacked is skipped rather than clamped -- an
     * exception reply is only 9 bytes and has no byte count at all.
     */
    if(length < MBAP_HEADER_SIZE + 1) {
        pdlog(LOG_MODULE_MODBUS_FAULT, LOG_LEVEL_DETAIL, "Response of %" PRIu64 " bytes is too short to corrupt.",
              (uint64_t)length);
        return;
    }

    if(modbus_fault_fires(MODBUS_FAULT_TXN_ID)) {
        uint16_t bad_txn = (uint16_t)(req_header->transaction_id ^ 0xA5A5u);

        data[OFF_TXN_ID] = (uint8_t)((bad_txn >> 8) & 0xFF);
        data[OFF_TXN_ID + 1] = (uint8_t)(bad_txn & 0xFF);
    }

    if(modbus_fault_fires(MODBUS_FAULT_PROTO_ID)) {
        /* anything but zero; the client checks this field explicitly */
        data[OFF_PROTO_ID] = 0x00;
        data[OFF_PROTO_ID + 1] = 0x01;
    }

    if(modbus_fault_fires(MODBUS_FAULT_UNIT_ID)) { data[OFF_UNIT_ID] = (uint8_t)(req_header->unit_id ^ 0xFFu); }

    if(modbus_fault_fires(MODBUS_FAULT_FUNC_CODE)) {
        /*
         * Shift the function code rather than setting the exception bit, so
         * that this stays distinct from the EXCEPTION fault: a client that
         * treats any unexpected code as an exception would otherwise pass
         * this by accident.
         */
        data[OFF_FUNC_CODE] = (uint8_t)((data[OFF_FUNC_CODE] + 1u) & 0x7Fu);
    }

    if(modbus_fault_fires(MODBUS_FAULT_BYTE_COUNT)) {
        if(length > OFF_BYTE_COUNT) {
            /* a byte count larger than the data that follows it */
            data[OFF_BYTE_COUNT] = (uint8_t)(data[OFF_BYTE_COUNT] + 2u);
        } else {
            pdlog(LOG_MODULE_MODBUS_FAULT, LOG_LEVEL_DETAIL, "Response has no byte count field to corrupt.");
        }
    }

    if(modbus_fault_fires(MODBUS_FAULT_EXCEPTION)) {
        /*
         * Turn a good reply into an ILLEGAL DATA ADDRESS exception: function
         * code with the high bit set, one exception byte, nothing else.
         */
        data[OFF_FUNC_CODE] = (uint8_t)(data[OFF_FUNC_CODE] | 0x80u);
        data[OFF_FUNC_CODE + 1] = 0x02;

        response->write = OFF_FUNC_CODE + 2;
        set_response_length_field(response, 3); /* unit id + function code + exception */

        length = response->write;
    }

    if(modbus_fault_fires(MODBUS_FAULT_SHORT_PDU)) {
        /*
         * Keep the MBAP header and the function code and drop everything
         * after, with the length field corrected to match.  The packet is
         * then self-consistent and still too short to be a legal reply, which
         * is the check being aimed at -- a reply that merely stops early
         * leaves the client waiting for bytes that never come, and tests a
         * timeout instead.
         */
        response->write = OFF_FUNC_CODE + 1;
        set_response_length_field(response, 2); /* unit id + function code */

        length = response->write;
    }

    if(modbus_fault_fires(MODBUS_FAULT_LENGTH)) {
        /*
         * Applied last so that it is the declared length that disagrees with
         * the packet, whatever the other faults did to it.
         */
        set_response_length_field(response, (uint16_t)(response_length_field(response) + 4u));
    }
}
