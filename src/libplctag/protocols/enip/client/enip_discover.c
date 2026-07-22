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
 * enip-udp: UDP List Identity discovery. See enip_discover.h and
 * ENIP-METADATA-AND-DISCOVERY-DESIGN.md for the design.
 *
 * A discover tag owns a dedicated worker thread (created at tag-create,
 * joined in the destructor) that idles on a wake condition until a read is
 * requested, then does one broadcast-or-unicast List Identity scan: send,
 * collect replies for discover_wait_ms, dedup by (source ip, serial),
 * append each new device as a raw record (enip_identity_write_raw_record)
 * and fire PLCTAG_EVENT_DATA_RECEIVED, then raise READ_COMPLETED. No
 * enip_connection_t / TCP session is involved -- List Identity is
 * connectionless UDP, so forcing this through the TCP state machine in
 * enip_session.c would mean special-casing every state for a fundamentally
 * different transport.
 */

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/protocols/enip/client/enip_discover.h>
#include <libplctag/protocols/enip/client/enip_eip.h>
#include <libplctag/protocols/enip/common/identity.h>
#include <platform.h>
#include <utils/arena.h>
#include <utils/atomic_utils.h>
#include <utils/attr.h>
#include <utils/bytes.h>
#include <utils/cbor.h>
#include <utils/cbor_schema.h>
#include <utils/debug.h>
#include <utils/rc.h>

#define ENIP_UDP_DEFAULT_PORT ((uint16_t)44818)
#define ENIP_UDP_DEFAULT_WAIT_MS ((int32_t)1000)
#define ENIP_UDP_RECV_BUF_SIZE ((int32_t)1500)
#define ENIP_UDP_SRC_HOST_LEN ((int32_t)64)

/* CIP types are little-endian on the wire; matches client/enip_tag.c's own
 * copy (each ENIP tag file defines its own -- small, and every other
 * protocol in this codebase does the same rather than sharing one). */
static tag_byte_order_t enip_discover_byte_order = {.is_allocated = 0,

                                                     .int16_order = {0, 1},
                                                     .int32_order = {0, 1, 2, 3},
                                                     .int64_order = {0, 1, 2, 3, 4, 5, 6, 7},
                                                     .float32_order = {0, 1, 2, 3},
                                                     .float64_order = {0, 1, 2, 3, 4, 5, 6, 7}};

typedef struct {
    uint32_t ip;
    uint32_t serial;
} enip_discover_seen_t;

struct enip_discover_tag_t {
    TAG_BASE_STRUCT;

    uint32_t target_ip_host; /* host-byte-order; the address actually sent to (directed broadcast, or the given unicast ip) */
    uint16_t port;
    bool is_broadcast;
    int32_t discover_wait_ms;

    uint32_t record_count; /* library-tracked; not stored in tag->data (design doc §8) */

    enip_discover_seen_t *seen; /* dedup set for the current/last scan */
    size_t seen_count, seen_cap;

    /* idle/wake handshake between the API thread (read()) and the worker;
     * separate from api_mutex/tag_cond_wait (the generic layer's own
     * completion-wait mechanism) so the worker blocking here never blocks
     * an API-thread attribute read. */
    mutex_p wake_mutex;
    cond_p wake_cond;
    bool read_requested; /* guarded by wake_mutex */

    sock_p scan_sock; /* non-NULL only while a scan's socket is open; guarded by api_mutex, so abort()/destroy can wake it */

    thread_p worker_thread;
    atomic_bool terminate;
    /* abort_requested is inherited from TAG_BASE_STRUCT -- reused here for
     * the same meaning (an in-progress operation should stop early). */
};
typedef struct enip_discover_tag_t *enip_discover_tag_p;

/* ============================================================================
 * Shared raw record encode (enip_discover.h)
 * ============================================================================ */

size_t enip_identity_raw_record_size(uint8_t name_len) { return (size_t)24 + name_len; }

bool enip_identity_write_raw_record(Bytes dest, size_t *pos, uint32_t ip_host, uint16_t port_host, uint16_t vendor_id,
                                    uint16_t device_type, uint16_t product_code, uint8_t revision_major,
                                    uint8_t revision_minor, uint16_t status, uint32_t serial, uint8_t state,
                                    const uint8_t *product_name, uint8_t name_len) {
    Bytes remaining = bytes_slice(dest, *pos, dest.len - *pos);
    if(bytes_is_null(remaining)) { return false; }

    uint16_t record_len = (uint16_t)(22 + name_len);

    /* ip_host is written in network byte order (big-endian); every other
     * field is little-endian, matching the CIP List Identity reply itself. */
    Bytes rest = bytes_pack_into(remaining, BYTES_LE, record_len);
    if(bytes_is_null(rest)) { return false; }
    rest = bytes_pack_into(rest, BYTES_BE, ip_host);
    if(bytes_is_null(rest)) { return false; }
    rest = bytes_pack_into(rest, BYTES_LE, port_host, vendor_id, device_type, product_code, revision_major, revision_minor,
                           status, serial, state, name_len, bytes_from_buf(product_name, name_len));
    if(bytes_is_null(rest)) { return false; }

    *pos = dest.len - rest.len;
    return true;
}

/* ============================================================================
 * IPv4 / gateway parsing
 * ============================================================================ */

/* Parse a dotted-quad IPv4 address from str[0..len). Returns the number of
 * characters consumed, or 0 on malformed input (manual digit-by-digit scan,
 * matching this codebase's existing style -- see enip_cip.c's route/array-
 * index parsers -- rather than sscanf). */
static size_t enip_discover_parse_ipv4(const char *str, size_t len, uint32_t *out) {
    uint32_t value = 0;
    size_t pos = 0;

    for(int octet = 0; octet < 4; octet++) {
        if(octet > 0) {
            if(pos >= len || str[pos] != '.') { return 0; }
            pos++;
        }

        if(pos >= len || str[pos] < '0' || str[pos] > '9') { return 0; }

        uint32_t v = 0;
        int digits = 0;
        while(pos < len && str[pos] >= '0' && str[pos] <= '9') {
            v = v * 10u + (uint32_t)(str[pos] - '0');
            pos++;
            digits++;
            if(digits > 3 || v > 255u) { return 0; }
        }

        value = (value << 8) | v;
    }

    *out = value;
    return pos;
}

static void enip_discover_format_ipv4(uint32_t ip_host, char *buf, size_t buf_size) {
    snprintf(buf, buf_size, "%u.%u.%u.%u", (unsigned)((ip_host >> 24) & 0xFFu), (unsigned)((ip_host >> 16) & 0xFFu),
             (unsigned)((ip_host >> 8) & 0xFFu), (unsigned)(ip_host & 0xFFu));
}

/* Parse "<ip>[/cidr][:port]" (design doc: enip-udp gateway schema). CIDR and
 * port are both optional; CIDR defaults to /32 (unicast), port to 44818.
 * *target_ip_host_out is the address to actually send to: the directed
 * broadcast address of the /cidr subnet when cidr < 32 (*is_broadcast_out
 * true), else the given ip unchanged. Returns false on a malformed string. */
static bool enip_discover_parse_gateway(const char *gateway, uint32_t *target_ip_host_out, uint16_t *port_out,
                                        bool *is_broadcast_out) {
    if(!gateway) { return false; }
    size_t len = (size_t)str_length(gateway);

    uint32_t ip_host = 0;
    size_t pos = enip_discover_parse_ipv4(gateway, len, &ip_host);
    if(pos == 0) { return false; }

    int cidr = 32;
    if(pos < len && gateway[pos] == '/') {
        pos++;
        if(pos >= len || gateway[pos] < '0' || gateway[pos] > '9') { return false; }
        int v = 0, digits = 0;
        while(pos < len && gateway[pos] >= '0' && gateway[pos] <= '9') {
            v = v * 10 + (gateway[pos] - '0');
            pos++;
            digits++;
            if(digits > 2 || v > 32) { return false; }
        }
        cidr = v;
    }

    uint16_t port = ENIP_UDP_DEFAULT_PORT;
    if(pos < len && gateway[pos] == ':') {
        pos++;
        if(pos >= len || gateway[pos] < '0' || gateway[pos] > '9') { return false; }
        uint32_t v = 0;
        int digits = 0;
        while(pos < len && gateway[pos] >= '0' && gateway[pos] <= '9') {
            v = v * 10u + (uint32_t)(gateway[pos] - '0');
            pos++;
            digits++;
            if(digits > 5 || v > 0xFFFFu) { return false; }
        }
        port = (uint16_t)v;
    }

    if(pos != len) { return false; } /* trailing garbage */

    bool is_broadcast = (cidr < 32);
    uint32_t target_ip = ip_host;
    if(is_broadcast) {
        uint32_t host_mask = 0xFFFFFFFFu >> cidr; /* cidr in [0,31] here; shift is well-defined */
        target_ip = ip_host | host_mask;
    }

    *target_ip_host_out = target_ip;
    *port_out = port;
    *is_broadcast_out = is_broadcast;
    return true;
}

/* ============================================================================
 * List Identity reply parsing
 * ============================================================================ */

/* Append one newly-seen device to tag->data (raw record) and fire
 * PLCTAG_EVENT_DATA_RECEIVED, under api_mutex. Deduplicates by
 * (source ip, serial); a duplicate is silently ignored. */
static void enip_discover_add_record(enip_discover_tag_p t, uint32_t src_ip_host, uint16_t reply_port, uint16_t vendor_id,
                                     uint16_t device_type, uint16_t product_code, uint8_t rev_major, uint8_t rev_minor,
                                     uint16_t status, uint32_t serial, uint8_t state, const uint8_t *name, uint8_t name_len) {
    plc_tag_p tag = (plc_tag_p)t;

    critical_block(t->api_mutex) {
        bool dup = false;
        for(size_t i = 0; i < t->seen_count; i++) {
            if(t->seen[i].ip == src_ip_host && t->seen[i].serial == serial) {
                dup = true;
                break;
            }
        }
        if(dup) { break; }

        if(t->seen_count == t->seen_cap) {
            size_t new_cap = t->seen_cap ? t->seen_cap * 2 : 8;
            enip_discover_seen_t *nseen = mem_realloc(t->seen, (int)(new_cap * sizeof(enip_discover_seen_t)));
            if(!nseen) { break; } /* OOM: drop this record, keep the scan going */
            t->seen = nseen;
            t->seen_cap = new_cap;
        }

        size_t rec_size = enip_identity_raw_record_size(name_len);
        size_t need = (size_t)tag->size + rec_size;
        uint8_t *nbuf = mem_realloc(tag->data, (int)need);
        if(!nbuf) { break; } /* OOM: original tag->data untouched (realloc semantics); drop this record */

        /* Repoint immediately: a successful realloc may have already
         * invalidated the old tag->data pointer, so tag->data must never be
         * left referring to it past this point regardless of what follows. */
        tag->data = nbuf;

        Bytes dest = bytes_from_buf(nbuf + tag->size, rec_size);
        size_t rec_pos = 0;
        if(enip_identity_write_raw_record(dest, &rec_pos, src_ip_host, reply_port, vendor_id, device_type, product_code,
                                          rev_major, rev_minor, status, serial, state, name, name_len)) {
            tag->size = (int32_t)need;
            t->seen[t->seen_count].ip = src_ip_host;
            t->seen[t->seen_count].serial = serial;
            t->seen_count++;
            t->record_count++;

            if(tag->callback) {
                /* Direct call, not tag_raise_event: this must fire once per
                 * record, in arrival order -- routed through the generic
                 * latch it would coalesce N arrivals into one callback (see
                 * design doc §2/§5). Buffer is already updated above. */
                tag->callback(tag->tag_id, PLCTAG_EVENT_DATA_RECEIVED, PLCTAG_STATUS_OK, tag->userdata);
            }
        }
        /* else: write failed despite exact sizing (should be unreachable);
         * tag->data/size still correctly describe the pre-record state, the
         * extra allocated tail is unused slack, not corruption. */
    }
}

/* Parse one UDP datagram as a List Identity reply and, if valid and new,
 * accumulate it. Malformed or non-matching datagrams are silently ignored
 * (best-effort collection; one bad packet must not abort the scan). */
static void enip_discover_handle_datagram(enip_discover_tag_p t, Bytes payload, const char *src_host) {
    enip_eip_hdr_t hdr;
    Bytes eip_payload;

    if(!eip_decode(payload, &hdr, &eip_payload)) { return; }
    if(hdr.cmd != ENIP_CMD_LIST_IDENTITY || hdr.status != 0) { return; }

    /* CPF body: item_count(u16LE), item_type(u16LE), item_len(u16LE), item body.
     * List Identity always answers with exactly one type-0x000C item. */
    uint16_t item_count = 0, item_type = 0, item_len = 0;
    Bytes rest = bytes_unpack(eip_payload, BYTES_LE, &item_count, &item_type, &item_len);
    if(bytes_is_null(rest) || item_count < 1 || item_type != 0x000C || rest.len < item_len) { return; }
    Bytes item = bytes_slice(rest, 0, item_len);
    if(bytes_is_null(item)) { return; }

    /* item body (identity.c's identity_encode_listid_item, the exact inverse):
     * protocol_version(u16LE, ignored) + sockaddr(family u16BE=2, port u16BE,
     * ipv4 u32BE) + reserved(8, ignored) + identity fields (vendor_id u16LE,
     * device_type u16LE, product_code u16LE, rev_major u8, rev_minor u8,
     * status u16LE, serial u32LE, name_len u8, name[name_len]) + state u8. */
    Bytes after_proto = bytes_unpack(item, BYTES_LE, BYTES_SKIP(2));
    if(bytes_is_null(after_proto)) { return; }

    uint16_t sin_family = 0, reply_port = 0;
    uint32_t reply_ip = 0;
    Bytes after_saddr = bytes_unpack(after_proto, BYTES_BE, &sin_family, &reply_port, &reply_ip);
    if(bytes_is_null(after_saddr) || sin_family != 2) { return; }

    Bytes after_reserved = bytes_unpack(after_saddr, BYTES_LE, BYTES_SKIP(8));
    if(bytes_is_null(after_reserved)) { return; }

    identity_t id = {0};
    Bytes after_name = {0};
    if(!identity_decode(after_reserved, &id, &after_name) || after_name.len < 1) { return; } /* 1: trailing state byte */
    uint8_t state = after_name.data[0];

    uint32_t src_ip_host = 0;
    if(enip_discover_parse_ipv4(src_host, (size_t)str_length(src_host), &src_ip_host) == 0) { return; }

    enip_discover_add_record(t, src_ip_host, reply_port, id.vendor_id, id.device_type, id.product_code, id.revision_major,
                             id.revision_minor, id.status, id.serial, state, (const uint8_t *)id.product_name,
                             (uint8_t)str_length(id.product_name));
}

/* ============================================================================
 * Worker thread
 * ============================================================================ */

static void enip_discover_run_scan(enip_discover_tag_p t) {
    plc_tag_p tag = (plc_tag_p)t;
    int32_t final_status = PLCTAG_STATUS_OK;

    sock_p sock = NULL;
    int rc = socket_create(&sock);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "enip-udp: socket_create failed: %s.", plc_tag_decode_error(rc));
        final_status = rc;
        goto finish;
    }

    rc = socket_open_udp(sock, NULL, 0, t->is_broadcast);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "enip-udp: socket_open_udp failed: %s.", plc_tag_decode_error(rc));
        socket_destroy(&sock);
        sock = NULL;
        final_status = rc;
        goto finish;
    }

    critical_block(t->api_mutex) {
        if(tag->data) {
            mem_free(tag->data);
            tag->data = NULL;
        }
        tag->size = 0;
        t->record_count = 0;
        t->seen_count = 0;
        t->scan_sock = sock;
    }

    {
        Arena scratch;
        if(arena_init(&scratch, 256) == 0) {
            Bytes frame = enip_eip_list_identity(&scratch);
            if(!bytes_is_null(frame)) {
                char target_str[32];
                enip_discover_format_ipv4(t->target_ip_host, target_str, sizeof(target_str));
                socket_send_to(sock, frame.data, (int32_t)frame.len, target_str, t->port);
            } else {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "enip-udp: unable to build List Identity request!");
            }
            arena_free(&scratch);
        }
    }

    {
        int64_t deadline = time_ms() + t->discover_wait_ms;
        uint8_t buf[ENIP_UDP_RECV_BUF_SIZE];
        char src_host[ENIP_UDP_SRC_HOST_LEN];

        while(!atomic_get_bool(&t->terminate) && !atomic_get_bool(&t->abort_requested)) {
            int64_t remaining = deadline - time_ms();
            if(remaining <= 0) { break; }
            if(remaining > INT32_MAX) { remaining = 1000; }

            uint16_t src_port = 0;
            int32_t n = socket_recv_from(sock, buf, ENIP_UDP_RECV_BUF_SIZE, src_host, ENIP_UDP_SRC_HOST_LEN, &src_port,
                                         (int32_t)remaining);

            if(n == PLCTAG_ERR_TIMEOUT) { break; }
            if(n == PLCTAG_ERR_ABORT) {
                final_status = PLCTAG_ERR_ABORT;
                break;
            }
            if(n < 0) { continue; } /* isolated recv error; keep collecting until the deadline */

            enip_discover_handle_datagram(t, bytes_from_buf(buf, (size_t)n), src_host);
        }

        if(atomic_get_bool(&t->abort_requested) && final_status == PLCTAG_STATUS_OK) { final_status = PLCTAG_ERR_ABORT; }
    }

finish:
    critical_block(t->api_mutex) { t->scan_sock = NULL; }

    if(sock) {
        socket_close(sock);
        socket_destroy(&sock);
    }

    critical_block(t->api_mutex) {
        tag->read_complete = 1;
        tag->read_in_flight = 0;
        tag->status = (int8_t)final_status;
        tag_raise_event(tag, PLCTAG_EVENT_READ_COMPLETED, (int8_t)final_status);
    }

    plc_tag_generic_handle_event_callbacks(tag);
    plc_tag_generic_wake_tag(tag);
}

static THREAD_FUNC(enip_discover_worker) {
    enip_discover_tag_p t = (enip_discover_tag_p)arg;

    while(!atomic_get_bool(&t->terminate)) {
        bool do_scan = false;

        mutex_lock(t->wake_mutex);
        while(!t->read_requested && !atomic_get_bool(&t->terminate)) {
            cond_wait(t->wake_cond, 1000); /* periodic wake so terminate is re-checked even if never signaled */
        }
        do_scan = t->read_requested;
        t->read_requested = false;
        mutex_unlock(t->wake_mutex);

        if(atomic_get_bool(&t->terminate)) { break; }
        if(!do_scan) { continue; }

        atomic_set_bool(&t->abort_requested, false);
        enip_discover_run_scan(t);
    }

    THREAD_RETURN(0);
}

/* ============================================================================
 * Vtable
 * ============================================================================ */

static int32_t enip_discover_tag_abort(plc_tag_p tag) {
    enip_discover_tag_p t = (enip_discover_tag_p)tag;

    atomic_set_bool(&t->abort_requested, true);
    critical_block(t->api_mutex) {
        if(t->scan_sock) { socket_wake(t->scan_sock); }
    }
    tag->read_in_flight = 0;

    return PLCTAG_STATUS_OK;
}

static int32_t enip_discover_tag_read(plc_tag_p tag) {
    enip_discover_tag_p t = (enip_discover_tag_p)tag;

    tag->read_complete = 0;
    tag->read_in_flight = 1;
    tag->status = (int8_t)PLCTAG_STATUS_PENDING;
    tag_raise_event(tag, PLCTAG_EVENT_READ_STARTED, (int8_t)PLCTAG_STATUS_OK);

    mutex_lock(t->wake_mutex);
    t->read_requested = true;
    cond_signal(t->wake_cond);
    mutex_unlock(t->wake_mutex);

    return PLCTAG_STATUS_PENDING;
}

static int32_t enip_discover_tag_status(plc_tag_p tag) { return tag->status; }

static int enip_discover_get_int_attrib(plc_tag_p tag, const char *attrib_name, int default_value) {
    enip_discover_tag_p t = (enip_discover_tag_p)tag;

    tag->status = (int8_t)PLCTAG_STATUS_OK;

    if(str_cmp_i(attrib_name, "record_count") == 0) { return (int)t->record_count; }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unsupported attribute \"%s\"!", attrib_name);
    tag->status = (int8_t)PLCTAG_ERR_UNSUPPORTED;

    return default_value;
}

/* ============================================================================
 * @identity structured presentation (format/schema subsystem; design doc
 * ENIP-METADATA-AND-DISCOVERY-DESIGN.md §0/§8). "cbor" is rendered on demand
 * by walking the raw records accumulated in tag->data (enip_identity_write_
 * raw_record's layout, above) -- nothing structured is cached. Reuses the
 * TCP @identity codec's field vocabulary (client/enip_tag.c's "identity"
 * schema) plus ip/port/state, which only exist for a UDP discovery scan.
 * Read-only: no set_formatted_data/set_schema.
 * ============================================================================ */

#define ENIP_UDP_SCHEMA_NAME "enip-udp-identity"
#define ENIP_UDP_SCHEMA_VERSION ((uint64_t)1)

typedef struct {
    uint32_t ip_host;
    uint16_t port, vendor_id, device_type, product_code, status;
    uint8_t revision_major, revision_minor, state;
    uint32_t serial;
    const char *product_name;
    uint8_t product_name_len;
} enip_udp_record_fields_t;

/* Parse one raw record starting at tag->data[off] (pointing at the record's
 * own u16LE length prefix, enip_identity_write_raw_record's layout). Returns
 * the offset of the next record, or 0 on end-of-data/malformed trailing
 * bytes (0 is never a valid "next" offset since every record is >= 24
 * bytes). *ok_out is only meaningful when a nonzero offset is returned. */
static size_t enip_udp_parse_record_at(plc_tag_p tag, size_t off, enip_udp_record_fields_t *out) {
    Bytes data = bytes_from_buf(tag->data, (size_t)tag->size);
    Bytes at = bytes_slice(data, off, data.len - off);
    if(bytes_is_null(at)) { return 0; }

    uint16_t record_len = 0;
    Bytes rest = bytes_unpack(at, BYTES_LE, &record_len);
    if(bytes_is_null(rest) || record_len < 22) { return 0; }

    Bytes payload = bytes_slice(rest, 0, record_len);
    if(bytes_is_null(payload)) { return 0; }

    uint32_t ip_be = 0;
    Bytes r2 = bytes_unpack(payload, BYTES_BE, &ip_be);
    if(bytes_is_null(r2)) { return 0; }
    out->ip_host = ip_be;

    r2 = bytes_unpack(r2, BYTES_LE, &out->port, &out->vendor_id, &out->device_type, &out->product_code,
                      &out->revision_major, &out->revision_minor, &out->status, &out->serial, &out->state,
                      &out->product_name_len);
    if(bytes_is_null(r2) || r2.len < out->product_name_len) { return 0; }
    out->product_name = (const char *)r2.data;

    return off + 2 + record_len;
}

/* Pointer+length view of one record, for the generic cbor_field_t table
 * below -- identity's 8 fields + ip/port/state (11 total). ip is a dotted
 * quad string (not part of the raw wire record; formatted into a
 * caller-owned buffer), matching the "gateway" attribute's own notation. */
typedef struct {
    const char *ip;
    size_t ip_len;
    uint16_t port, vendor_id, device_type, product_code, status;
    uint8_t revision_major, revision_minor, state;
    uint32_t serial;
    const char *product_name;
    size_t product_name_len;
} enip_udp_view_t;

static const cbor_field_t ENIP_UDP_RECORD_FIELDS[] = {
    {"ip", CBOR_FIELD_TEXT, offsetof(enip_udp_view_t, ip), offsetof(enip_udp_view_t, ip_len), -1},
    {"port", CBOR_FIELD_U16, offsetof(enip_udp_view_t, port), 0, -1},
    {"vendor_id", CBOR_FIELD_U16, offsetof(enip_udp_view_t, vendor_id), 0, -1},
    {"device_type", CBOR_FIELD_U16, offsetof(enip_udp_view_t, device_type), 0, -1},
    {"product_code", CBOR_FIELD_U16, offsetof(enip_udp_view_t, product_code), 0, -1},
    {"revision_major", CBOR_FIELD_U8, offsetof(enip_udp_view_t, revision_major), 0, -1},
    {"revision_minor", CBOR_FIELD_U8, offsetof(enip_udp_view_t, revision_minor), 0, -1},
    {"status", CBOR_FIELD_U16, offsetof(enip_udp_view_t, status), 0, -1},
    {"serial", CBOR_FIELD_U32, offsetof(enip_udp_view_t, serial), 0, -1},
    {"state", CBOR_FIELD_U8, offsetof(enip_udp_view_t, state), 0, -1},
    {"product_name", CBOR_FIELD_TEXT, offsetof(enip_udp_view_t, product_name), offsetof(enip_udp_view_t, product_name_len),
     -1},
};
#define ENIP_UDP_RECORD_FIELD_COUNT ((size_t)(sizeof(ENIP_UDP_RECORD_FIELDS) / sizeof(ENIP_UDP_RECORD_FIELDS[0])))

/* ip_buf must outlive the caller's use of *out (cbor_record_size/
 * cbor_emit_record only borrow the pointer, they don't copy). */
static void enip_udp_view_init(enip_udp_view_t *out, const enip_udp_record_fields_t *f, char *ip_buf, size_t ip_buf_size) {
    enip_discover_format_ipv4(f->ip_host, ip_buf, ip_buf_size);
    out->ip = ip_buf;
    out->ip_len = (size_t)str_length(ip_buf);
    out->port = f->port;
    out->vendor_id = f->vendor_id;
    out->device_type = f->device_type;
    out->product_code = f->product_code;
    out->revision_major = f->revision_major;
    out->revision_minor = f->revision_minor;
    out->status = f->status;
    out->serial = f->serial;
    out->state = f->state;
    out->product_name = f->product_name;
    out->product_name_len = f->product_name_len;
}

/* Two-pass envelope size/write: pass 1 walks tag->data to get the true
 * record count (not t->record_count -- that's the scan's own running
 * counter; recomputing from the data itself avoids ever mismatching what
 * this function can actually parse) and total size; pass 2 (write only)
 * walks again and encodes each record. Stops early, not erroring, on a
 * malformed trailing record -- the same defensive stance as any other
 * accumulated-buffer parse in this dialect. */
static bool enip_udp_envelope_cbor_size(plc_tag_p tag, size_t *record_count_out, size_t *size_out) {
    size_t count = 0;
    size_t records_size = 0;
    size_t off = 0;

    while((size_t)tag->size > off) {
        enip_udp_record_fields_t f;
        size_t next = enip_udp_parse_record_at(tag, off, &f);
        if(next == 0) { break; }

        char ip_buf[16];
        enip_udp_view_t v;
        enip_udp_view_init(&v, &f, ip_buf, sizeof(ip_buf));
        records_size += cbor_record_size(ENIP_UDP_RECORD_FIELDS, ENIP_UDP_RECORD_FIELD_COUNT, &v);
        count++;
        off = next;
    }

    *record_count_out = count;
    *size_out = cbor_envelope_size(ENIP_UDP_SCHEMA_NAME, ENIP_UDP_SCHEMA_VERSION, count, records_size);
    return true;
}

static bool enip_udp_envelope_cbor_write(plc_tag_p tag, size_t record_count, Bytes dest, size_t *pos) {
    if(!cbor_emit_envelope(dest, pos, ENIP_UDP_SCHEMA_NAME, ENIP_UDP_SCHEMA_VERSION, record_count)) { return false; }

    size_t off = 0;
    for(size_t i = 0; i < record_count; i++) {
        enip_udp_record_fields_t f;
        size_t next = enip_udp_parse_record_at(tag, off, &f);
        if(next == 0) { return false; } /* record count changed since the size pass -- shouldn't happen, no concurrent writer */

        char ip_buf[16];
        enip_udp_view_t v;
        enip_udp_view_init(&v, &f, ip_buf, sizeof(ip_buf));
        if(!cbor_emit_record(dest, pos, ENIP_UDP_RECORD_FIELDS, ENIP_UDP_RECORD_FIELD_COUNT, &v)) { return false; }
        off = next;
    }
    return true;
}

static int enip_discover_get_formatted_data_size(plc_tag_p tag, plc_tag_format_type_t format) {
    if(format != PLCTAG_FORMAT_CBOR) { return PLCTAG_ERR_UNSUPPORTED; }

    size_t count = 0, size = 0;
    if(!enip_udp_envelope_cbor_size(tag, &count, &size)) { return PLCTAG_ERR_BAD_DATA; }

    return (int)size;
}

static int enip_discover_get_formatted_data(plc_tag_p tag, plc_tag_format_type_t format, uint8_t *buffer, int buffer_length) {
    if(format != PLCTAG_FORMAT_CBOR) { return PLCTAG_ERR_UNSUPPORTED; }

    size_t count = 0, size = 0;
    if(!enip_udp_envelope_cbor_size(tag, &count, &size)) { return PLCTAG_ERR_BAD_DATA; }
    if(size > (size_t)buffer_length) { return PLCTAG_ERR_TOO_SMALL; }

    Bytes dest = bytes_from_buf(buffer, (size_t)buffer_length);
    size_t pos = 0;
    if(!enip_udp_envelope_cbor_write(tag, count, dest, &pos)) { return PLCTAG_ERR_TOO_SMALL; }

    return PLCTAG_STATUS_OK;
}

/* Field-name reflection of the "enip-udp-identity" schema. Not tag-specific
 * (every enip-udp @identity tag has the same schema), so this ignores `tag`. */
static const char *const ENIP_UDP_FIELD_NAMES[] = {"ip",  "port",           "vendor_id",      "device_type",
                                                    "product_code", "revision_major", "revision_minor", "status",
                                                    "serial", "state", "product_name"};
#define ENIP_UDP_FIELD_COUNT ((size_t)(sizeof(ENIP_UDP_FIELD_NAMES) / sizeof(ENIP_UDP_FIELD_NAMES[0])))

static int enip_discover_get_schema_size(plc_tag_p tag, plc_tag_format_type_t format) {
    (void)tag;
    if(format != PLCTAG_FORMAT_CBOR) { return PLCTAG_ERR_UNSUPPORTED; }
    return (int)cbor_schema_size(ENIP_UDP_SCHEMA_NAME, ENIP_UDP_SCHEMA_VERSION, ENIP_UDP_FIELD_NAMES, ENIP_UDP_FIELD_COUNT);
}

static int enip_discover_get_schema(plc_tag_p tag, plc_tag_format_type_t format, uint8_t *buffer, int buffer_length) {
    (void)tag;
    if(format != PLCTAG_FORMAT_CBOR) { return PLCTAG_ERR_UNSUPPORTED; }
    if(cbor_schema_size(ENIP_UDP_SCHEMA_NAME, ENIP_UDP_SCHEMA_VERSION, ENIP_UDP_FIELD_NAMES, ENIP_UDP_FIELD_COUNT)
       > (size_t)buffer_length) {
        return PLCTAG_ERR_TOO_SMALL;
    }

    Bytes dest = bytes_from_buf(buffer, (size_t)buffer_length);
    size_t pos = 0;
    if(!cbor_emit_schema(dest, &pos, ENIP_UDP_SCHEMA_NAME, ENIP_UDP_SCHEMA_VERSION, ENIP_UDP_FIELD_NAMES,
                        ENIP_UDP_FIELD_COUNT)) {
        return PLCTAG_ERR_TOO_SMALL;
    }

    return PLCTAG_STATUS_OK;
}

static struct tag_vtable_t enip_discover_tag_vtable = {
    .abort = enip_discover_tag_abort,
    .read = enip_discover_tag_read,
    .status = enip_discover_tag_status,
    .tickler = NULL,
    .write = NULL,
    .wake_plc = NULL,
    .tag_data_written = NULL,
    .get_int_attrib = enip_discover_get_int_attrib,
    .set_int_attrib = NULL,
    .get_byte_array_attrib = NULL,
    .get_formatted_data_size = enip_discover_get_formatted_data_size,
    .get_formatted_data = enip_discover_get_formatted_data,
    .set_formatted_data = NULL, /* read-only tag */
    .get_schema_size = enip_discover_get_schema_size,
    .get_schema = enip_discover_get_schema,
    .set_schema = NULL, /* built-in schema, not user-settable */
};

/* ============================================================================
 * Create / destroy
 * ============================================================================ */

static void enip_discover_tag_destructor(void *arg) {
    enip_discover_tag_p tag = (enip_discover_tag_p)arg;
    if(!tag) { return; }

    atomic_set_bool(&tag->terminate, true);

    if(tag->wake_mutex) {
        mutex_lock(tag->wake_mutex);
        if(tag->wake_cond) { cond_signal(tag->wake_cond); }
        mutex_unlock(tag->wake_mutex);
    }
    if(tag->api_mutex) {
        critical_block(tag->api_mutex) {
            if(tag->scan_sock) { socket_wake(tag->scan_sock); }
        }
    }

    if(tag->worker_thread) {
        thread_join(tag->worker_thread);
        thread_destroy(&tag->worker_thread);
    }

    if(tag->wake_cond) { cond_destroy(&tag->wake_cond); }
    if(tag->wake_mutex) { mutex_destroy(&tag->wake_mutex); }

    if(tag->seen) { mem_free(tag->seen); }

    if(((plc_tag_p)tag)->data) {
        mem_free(((plc_tag_p)tag)->data);
        ((plc_tag_p)tag)->data = NULL;
    }
    if(tag->api_mutex) { mutex_destroy(&tag->api_mutex); }
    if(tag->ext_mutex) { mutex_destroy(&tag->ext_mutex); }
    if(tag->tag_cond_wait) { cond_destroy(&tag->tag_cond_wait); }
}

plc_tag_p enip_udp_tag_create_impl(attr attribs,
                                   void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                   void *userdata, plc_tag_p src_tag) {
    (void)src_tag; /* no connection/session sharing concept for discovery -- each tag scans independently */

    const char *name = attr_get_str(attribs, "name", "");
    if(str_length(name) > 0 && str_cmp(name, "@identity") != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
               "enip-udp: unsupported tag name \"%s\" (only \"@identity\" is supported).", name);
        return NULL;
    }

    const char *gateway = attr_get_str(attribs, "gateway", NULL);
    if(!gateway || str_length(gateway) == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "enip-udp: missing required \"gateway\" attribute.");
        return NULL;
    }

    uint32_t target_ip_host = 0;
    uint16_t port = 0;
    bool is_broadcast = false;
    if(!enip_discover_parse_gateway(gateway, &target_ip_host, &port, &is_broadcast)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
               "enip-udp: malformed \"gateway\" attribute \"%s\" (expected <ip>[/cidr][:port]).", gateway);
        return NULL;
    }

    enip_discover_tag_p tag = (enip_discover_tag_p)rc_alloc((int)sizeof(struct enip_discover_tag_t), enip_discover_tag_destructor);
    if(!tag) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "enip-udp: unable to allocate tag!");
        return NULL;
    }

    tag->vtable = &enip_discover_tag_vtable;
    tag->byte_order = &enip_discover_byte_order;

    tag->target_ip_host = target_ip_host;
    tag->port = port;
    tag->is_broadcast = is_broadcast;
    tag->discover_wait_ms = (int32_t)attr_get_int(attribs, "discover_wait_ms", ENIP_UDP_DEFAULT_WAIT_MS);
    if(tag->discover_wait_ms < 1) { tag->discover_wait_ms = 1; }

    atomic_init_bool(&tag->terminate, false);
    atomic_init_bool(&tag->abort_requested, false);

    if(mutex_create(&tag->wake_mutex) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "enip-udp: unable to create wake mutex!");
        rc_dec(tag);
        return NULL;
    }
    if(cond_create(&tag->wake_cond) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "enip-udp: unable to create wake condition variable!");
        rc_dec(tag);
        return NULL;
    }

    int32_t rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "enip-udp: unable to initialize generic tag parts!");
        rc_dec(tag);
        return NULL;
    }

    tag->protocol_type = TAG_PROTOCOL_ENIP_UDP;
    tag->skip_tickler = 1; /* driven by our own worker thread, not the global tickler */

    if(thread_create(&tag->worker_thread, enip_discover_worker, 32768, (void *)tag) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "enip-udp: unable to create worker thread!");
        rc_dec(tag);
        return NULL;
    }

    tag->status = (int8_t)PLCTAG_STATUS_OK;
    tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, (int8_t)PLCTAG_STATUS_OK);
    plc_tag_generic_handle_event_callbacks((plc_tag_p)tag);

    return (plc_tag_p)tag;
}
