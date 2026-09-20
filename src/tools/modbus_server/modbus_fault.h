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
 * LGPL 2:                                                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 ***************************************************************************/

/*
 * Deliberate response corruption, for testing what the client does with a
 * Modbus server that misbehaves.  The shape follows ab_server/fault.[ch] --
 * "<kind>[:<count>]" on the command line, a counter per kind, each response
 * consuming one use -- so that the two servers are driven the same way.
 *
 * Two differences from the AB version, both deliberate:
 *
 *   - The counters are plain, not atomic.  ab_server runs a thread per
 *     connection and shares one plc_s, so two connections could otherwise
 *     both decide a count of one was theirs.  This server is a single
 *     coroutine thread, so there is no such race.
 *   - Not every fault is a mutation of the built response.  CLOSE, DELAY and
 *     DRIBBLE change how the reply is delivered rather than what it says,
 *     which the caller has to act on; see modbus_fault_fires().
 *
 * Every mutation keeps the packet coherent outside the layer it attacks.  A
 * reply that is simply cut short leaves the client waiting for bytes that
 * never arrive, which tests a timeout rather than a check -- so SHORT_PDU
 * fixes up the MBAP length to match the bytes it actually sends.
 */

#include <stdbool.h>
#include <stdint.h>

#include "buf.h"
#include "modbus_protocol.h"

typedef enum {
    MODBUS_FAULT_NONE = 0,
    MODBUS_FAULT_TXN_ID,     /* echo a transaction ID the client never sent. */
    MODBUS_FAULT_PROTO_ID,   /* claim a protocol identifier other than zero. */
    MODBUS_FAULT_UNIT_ID,    /* answer with a unit ID other than the one asked for. */
    MODBUS_FAULT_FUNC_CODE,  /* answer with a different function code than was sent. */
    MODBUS_FAULT_LENGTH,     /* declare an MBAP length that disagrees with the packet. */
    MODBUS_FAULT_SHORT_PDU,  /* cut the PDU down below a legal reply, length adjusted to match. */
    MODBUS_FAULT_EXCEPTION,  /* answer a perfectly good request with a Modbus exception. */
    MODBUS_FAULT_BYTE_COUNT, /* declare a PDU byte count that disagrees with the data after it. */
    MODBUS_FAULT_CLOSE,      /* close the connection instead of answering. */
    MODBUS_FAULT_DELAY,      /* answer late.  The count is milliseconds, not a number of uses. */
    MODBUS_FAULT_DRIBBLE,    /* answer in N-byte pieces so the client sees a partial read. */
    MODBUS_FAULT_MAX
} modbus_fault_kind_t;


/*
 * Arm a fault from a "<kind>[:<count>]" specification, as passed to
 * --corrupt.  The count defaults to one and means a number of responses,
 * except for DELAY where it is milliseconds and DRIBBLE where it is the
 * piece size in bytes.  Returns false if the kind is unknown or the count is
 * not a positive number.
 */
extern bool modbus_fault_parse(const char *spec);

/*
 * True if this kind should fire for the response being handled right now, in
 * which case one use is consumed.  Safe to call when nothing is armed.
 *
 * Callers only need this for the three faults that are not mutations:
 * CLOSE, DELAY and DRIBBLE.  The rest are applied by
 * modbus_fault_apply_response().
 */
extern bool modbus_fault_fires(modbus_fault_kind_t kind);

/*
 * Applies every armed mutation to a response that is otherwise ready to
 * send, consuming one use of each that fires.  Call it once, after the
 * response is built and before it goes out.
 */
extern void modbus_fault_apply_response(buf_t *response, const mbap_header_t *req_header);

/*
 * The DELAY count in milliseconds, or the DRIBBLE piece size in bytes.  Both
 * return zero when the fault is not armed.  These read the counter without
 * consuming it: a delay is a property of the connection for as long as it is
 * armed, not a one-shot.
 */
extern int32_t modbus_fault_delay_ms(void);
extern int32_t modbus_fault_dribble_bytes(void);

/* the name as spelled on the command line, for logging and the usage message. */
extern const char *modbus_fault_kind_name(modbus_fault_kind_t kind);

/* every kind's name, for the usage message.  NULL terminated. */
extern const char **modbus_fault_kind_names(void);
