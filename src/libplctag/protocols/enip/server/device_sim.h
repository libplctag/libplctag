/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever    *
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
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#pragma once

/*
 * device_sim.h — internal endpoint runtime (device_t lifecycle, listener +
 * discovery threads).  Not a public header: it is not installed, and the
 * only callers are server/endpoint.c (endpoint_find_or_create/_release) and
 * server/eip_server_tag.c, which together implement
 * plc_tag_create("...&role=server&...") — see SERVER_TAGS.md.  External
 * embedding programs (the device_sim CLI, the devsim_with_plctag POC) go
 * through role=server, not through this API directly.
 *

 * Threading rules:
 *   - All device_sim_add_* calls must complete before device_sim_start().
 *   - After start, only device_sim_tag_get/set, device_sim_get/set_identity,
 *     and device_sim_stop are safe to call from any thread.
 *   - device_sim_destroy() is called after stop; it joins the threads and
 *     frees all resources.
 */

#include <stddef.h>
#include <stdint.h>

#include <libplctag/protocols/enip/common/plc_type.h>

/* ============================================================================
 * Opaque handle
 * ============================================================================ */

typedef struct device_sim_s device_sim_t;

/* PLC type to emulate: enip_plc_type_t (common/plc_type.h) is shared with the
 * client's CIP-Identity auto-classification -- whatever family the client
 * can detect on the wire, the simulator can emulate, and vice versa. */

/* ============================================================================
 * Tag type codes (CIP and PCCC)
 * ============================================================================ */

typedef uint16_t tag_type_t;

#define TAG_CIP_TYPE_BOOL ((tag_type_t)0x00C1)
#define TAG_CIP_TYPE_SINT ((tag_type_t)0x00C2)
#define TAG_CIP_TYPE_INT ((tag_type_t)0x00C3)
#define TAG_CIP_TYPE_DINT ((tag_type_t)0x00C4)
#define TAG_CIP_TYPE_LINT ((tag_type_t)0x00C5)
#define TAG_CIP_TYPE_USINT ((tag_type_t)0x00C6)
#define TAG_CIP_TYPE_UINT ((tag_type_t)0x00C7)
#define TAG_CIP_TYPE_UDINT ((tag_type_t)0x00C8)
#define TAG_CIP_TYPE_ULINT ((tag_type_t)0x00C9)
#define TAG_CIP_TYPE_REAL ((tag_type_t)0x00CA)
#define TAG_CIP_TYPE_LREAL ((tag_type_t)0x00CB)
#define TAG_CIP_TYPE_STRING ((tag_type_t)0x00D0)
#define TAG_CIP_TYPE_BYTE ((tag_type_t)0x00D1)
#define TAG_CIP_TYPE_WORD ((tag_type_t)0x00D2)
#define TAG_CIP_TYPE_DWORD ((tag_type_t)0x00D3)
#define TAG_CIP_TYPE_LWORD ((tag_type_t)0x00D4)

/* Logix STRING is a structure (DINT length + SINT data[82], padded to a
 * 4-byte boundary) -- device_sim doesn't interpret member layout, so it's
 * just given a fixed instance size matching the real wire structure. */
#define TAG_CIP_STRING_SIZE ((size_t)88)

#define TAG_PCCC_TYPE_BIT ((tag_type_t)0x0085)
#define TAG_PCCC_TYPE_INT ((tag_type_t)0x0089)
#define TAG_PCCC_TYPE_DINT ((tag_type_t)0x0091)
#define TAG_PCCC_TYPE_REAL ((tag_type_t)0x008A)
#define TAG_PCCC_TYPE_STRING ((tag_type_t)0x008D)

/* PCCC ST string file element: 2-byte length + 82-byte data. */
#define TAG_PCCC_STRING_SIZE ((size_t)84)

/* ============================================================================
 * Identity object (CIP class 0x01, instance 1)
 * ============================================================================ */

#define IDENTITY_MAX_NAME (64)

typedef struct {
    uint16_t vendor_id;
    uint16_t device_type;
    uint16_t product_code;
    uint8_t revision_major;
    uint8_t revision_minor;
    uint16_t status;
    uint32_t serial;
    char product_name[IDENTITY_MAX_NAME];
    uint8_t state;
} identity_t;

/* ============================================================================
 * Callbacks
 *
 * All callbacks run on library-owned threads (the listener and per-connection
 * threads), NOT the caller's thread.  A binding for a runtime with a global
 * lock or thread-affine VM (Python GIL, Node, JVM, CLR) must marshal back to
 * its own thread before touching managed state.
 * ============================================================================ */

/*
 * Tag read/write callback (fired by the protocol handler on each access).
 *
 * read_cb:  called before the response is serialised; the callback may modify
 *           `data` to change what the client reads.
 * write_cb: called after the new bytes have been stored; the callback may
 *           inspect or react to the new value.
 *
 * Both run WITHOUT holding the tag's data_mutex.  The buffer is a mutable
 * scratch copy (read) or the just-stored bytes (write).  Use
 * device_sim_tag_set/get to reach OTHER tags from within a callback.
 */
typedef void (*device_sim_tag_cb)(device_sim_t *sim, const char *name, void *data, uint32_t data_len, void *user_data);

/* Fired when a TCP client connects or disconnects. */
typedef void (*device_sim_conn_cb)(device_sim_t *sim, void *user_data);

/*
 * Generic CIP class/instance handler.  Called when no built-in handler matches.
 *
 * Return DEVICE_SIM_NOT_HANDLED to let the dispatcher try the next entry or
 * return CIP "service unsupported".  Return PLCTAG_STATUS_OK and write up to
 * resp_cap bytes into resp; set *resp_len to the byte count.  Return any other
 * negative PLCTAG_ERR_* to send a CIP "service unsupported" error response.
 */
/* Sentinel instance_id for device_sim_add_cip_object: matches any instance. */
#define DEVICE_SIM_ANY_INSTANCE ((uint32_t)0xFFFFFFFFu)

#define DEVICE_SIM_NOT_HANDLED ((int32_t)1)

/* Return from a device_sim_cip_cb to signal "partial response, more data follows"
 * (CIP general status 0x06).  Write the partial data into resp as usual. */
#define DEVICE_SIM_MORE_DATA ((int32_t)2)

typedef int32_t (*device_sim_cip_cb)(device_sim_t *sim, uint8_t service, const uint8_t *path, uint32_t path_len, const uint8_t *req,
                                     uint32_t req_len, uint8_t *resp, uint32_t resp_cap, uint32_t *resp_len, void *user_data);

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/*
 * Allocate and initialise a simulator instance.  Only scalar arguments — no
 * struct — so every FFI layer can call it without replicating struct layout.
 *
 *   plc_type   the PLC dialect to emulate (also seeds the identity defaults).
 *   model      optional catalog model within plc_type's family (e.g. "NX102"
 *              for ENIP_PLC_OMRON_NJNX; see common/identity.c's table).
 *              NULL/"" uses the family's default model.
 *   bind_addr  listen address; NULL = all interfaces.
 *   port       TCP + UDP listen port; 0 = default 44818.
 *
 * The optional knobs (response delay, max packet sizes) keep their defaults
 * unless overridden by the device_sim_set_* functions below, which must be
 * called before device_sim_start().  The IPv4 address reported in List Identity
 * replies is computed by the library per request from the arrival path, so it
 * is always one the querying client can connect back to.
 *
 * The identity is seeded from the built-in defaults for (plc_type, model) and
 * can be overridden with device_sim_set_identity() before or after start.
 * Returns NULL on allocation failure.
 */
extern device_sim_t *device_sim_create(enip_plc_type_t plc_type, const char *model, const char *bind_addr, uint16_t port);

/*
 * Optional configuration — all must be called before device_sim_start().
 * Each returns PLCTAG_STATUS_OK or PLCTAG_ERR_BAD_PARAM.
 */

/* Artificial per-response delay in milliseconds (default 0). */
extern int32_t device_sim_set_response_delay(device_sim_t *sim, uint32_t response_delay_ms);

/* Max CIP packet sizes in each direction (default 508 each; 0 keeps the default). */
extern int32_t device_sim_set_max_packet(device_sim_t *sim, uint32_t client_to_server, uint32_t server_to_client);

/*
 * Spawn the TCP listener and UDP discovery threads.
 * Must be called after all device_sim_add_* calls.
 * Returns PLCTAG_STATUS_OK or a negative PLCTAG_ERR_* code.
 */
extern int32_t device_sim_start(device_sim_t *sim);

/*
 * Signal shutdown: sets the per-device terminate flag and wakes all blocked
 * socket calls.  Idempotent; safe to call from a signal handler's trampoline
 * (i.e. from main(), not from the handler itself, because it takes a mutex).
 */
extern int32_t device_sim_stop(device_sim_t *sim);

/*
 * Join the listener and discovery threads, then free all resources including
 * tags.  Calls device_sim_stop() internally if not already stopped.
 */
extern void device_sim_destroy(device_sim_t *sim);

/* ============================================================================
 * Tag management (must be called before device_sim_start)
 * ============================================================================ */

/*
 * Add a CIP tag.  dims[0..num_dims-1] are the element counts per dimension
 * (1-D: {100}; 2-D: {2,3}; 3-D: {2,3,4}).  read_cb and write_cb may be NULL.
 * Returns PLCTAG_STATUS_OK or a negative error code.
 */
extern int32_t device_sim_add_tag(device_sim_t *sim, const char *name, tag_type_t type, const uint32_t *dims, uint32_t num_dims,
                                  device_sim_tag_cb read_cb, device_sim_tag_cb write_cb, void *user_data);

/*
 * Add a PCCC tag.  name is the file designator string (e.g. "B3", "N7").
 * file_num identifies the PCCC file number stored in data_file_num.
 * read_cb and write_cb may be NULL.
 */
extern int32_t device_sim_add_pccc_tag(device_sim_t *sim, const char *name, tag_type_t type, uint32_t file_num, uint32_t elem_count,
                                       device_sim_tag_cb read_cb, device_sim_tag_cb write_cb, void *user_data);

/* ============================================================================
 * UDT/structure template registration (must be called before device_sim_start)
 *
 * Registers a CIP class 0x6C template so that a tag using
 * DEVICE_SIM_STRUCTURE_TYPE(template_id) as its type (in device_sim_add_tag)
 * enumerates correctly under `@tags`/`@udt` (ROCKWELL-SPECIFIC-DESIGN.md §5).
 * The simulator does not otherwise interpret member layout -- tag data is
 * still a flat byte buffer accessed via device_sim_tag_get/set exactly as for
 * any other tag; `members` only feeds the template *definition* served over
 * class 0x6C service 0x4C so a real client's UDT decoder can parse it.
 * ============================================================================ */

/* One member of a UDT template definition, in declaration order. type is an
 * atomic tag_type_t (e.g. TAG_CIP_TYPE_DINT) or another template's
 * DEVICE_SIM_STRUCTURE_TYPE(id) for a nested UDT member. array_count 0 or 1
 * means scalar. offset is the member's byte offset within one instance. */
typedef struct {
    const char  *name;
    tag_type_t   type;
    uint32_t     array_count;
    uint32_t     offset;
} udt_member_t;

/* Structure/UDT symbol type: set as a tag's tag_type (device_sim_add_tag) to
 * mark it as an instance of the template registered with this id. */
#define DEVICE_SIM_STRUCTURE_TYPE(template_id) ((tag_type_t)(0x8000u | ((template_id) & 0x0FFFu)))

/*
 * Register a UDT template. instance_size is the total byte size of one
 * instance (the caller computes any padding a real controller would add;
 * the simulator does not infer it from members). Returns the assigned
 * template id (1..0x0FFF) via *template_id_out and PLCTAG_STATUS_OK, or a
 * negative PLCTAG_ERR_* code. members may be NULL/num_members 0 for a
 * template with no published field layout (definition reads return 0 bytes).
 */
extern int32_t device_sim_add_udt_type(device_sim_t *sim, const char *struct_name, uint32_t instance_size,
                                       const udt_member_t *members, uint32_t num_members, uint16_t *template_id_out);

/* ============================================================================
 * Identity access (thread-safe; may be called before or after start)
 * ============================================================================ */

extern int32_t device_sim_get_identity(device_sim_t *sim, identity_t *out);
extern int32_t device_sim_set_identity(device_sim_t *sim, const identity_t *id);

/* ============================================================================
 * Direct tag data access (thread-safe; may be called from any thread or callback)
 *
 * Offsets and lengths are in bytes.  The function copies min(len, available)
 * bytes; it does NOT zero-pad if len > available.
 * Returns PLCTAG_STATUS_OK, PLCTAG_ERR_NOT_FOUND, or PLCTAG_ERR_OUT_OF_BOUNDS.
 * ============================================================================ */

extern int32_t device_sim_tag_get(device_sim_t *sim, const char *name, uint32_t offset, void *dst, uint32_t len);

extern int32_t device_sim_tag_set(device_sim_t *sim, const char *name, uint32_t offset, const void *src, uint32_t len);

/* ============================================================================
 * Client connect/disconnect callbacks (must be set before device_sim_start)
 *
 * connect_cb fires in the connection thread immediately after the TCP session
 * is established.  disconnect_cb fires just before the thread exits, whether
 * the client closed cleanly, timed out, or the simulator is shutting down.
 * Either may be NULL.  cb and user_data are stored directly — no locking.
 * ============================================================================ */

/* ============================================================================
 * Generic CIP object registry (must be called before device_sim_start)
 *
 * Register a handler for the given (class_id, instance_id) pair.  All CIP
 * services directed at that pair are routed to cb; the callback returns
 * DEVICE_SIM_NOT_HANDLED to fall through to the next registry entry or the
 * built-in "service unsupported" response.  class_id and instance_id are
 * 32-bit to accommodate all CIP logical segment widths (8/16/32-bit).
 * ============================================================================ */

extern int32_t device_sim_add_cip_object(device_sim_t *sim, uint32_t class_id, uint32_t instance_id, device_sim_cip_cb cb,
                                         void *user_data);

extern int32_t device_sim_set_connect_cb(device_sim_t *sim, device_sim_conn_cb cb, void *user_data);

extern int32_t device_sim_set_disconnect_cb(device_sim_t *sim, device_sim_conn_cb cb, void *user_data);
