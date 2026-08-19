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
 * Connection lifecycle and IO thread (design doc §3-5, §10, §13, §14.3).
 *
 * MVP scope (§15.2): connect -> RegisterSession -> ForwardOpen -> READY,
 * then service a single in-flight tag op at a time (OPEN_PROBE / READ /
 * OPEN_BULK). elem_count > 1 tags are primed by OPEN_PROBE (element 0) then
 * OPEN_BULK (remaining elements, windowed per §11.3).
 *
 * Deviation from the literal §14.3 text: the tx frame is produced directly
 * by enip_cip_read / cpf_wrap_* / enip_eip_* (each an arena allocation),
 * rather than being assembled byte-by-byte into a pre-sized tx_buf.  The
 * arena is reset before building and the resulting Bytes is used in place
 * as tx_buf/tx_len.
 */

#include <inttypes.h>
#include <string.h>

#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/protocols/enip/client/enip_cip.h>
#include <libplctag/protocols/enip/client/enip_connection_internal.h>
#include <libplctag/protocols/enip/client/enip_dialect.h>
#include <libplctag/protocols/enip/client/enip_session.h>
#include <libplctag/protocols/enip/common/eip.h>
#include <libplctag/protocols/enip/client/enip_tag.h>
#include <libplctag/protocols/enip/client/enip_type.h>
#include <libplctag/protocols/enip/common/cpf.h>
#include <libplctag/protocols/enip/common/identity.h>
#include <libplctag/protocols/enip/common/plc_classify.h>
#include <libplctag/protocols/enip/common/plc_type.h>
#include <platform.h>
#include <utils/arena.h>
#include <utils/atomic_utils.h>
#include <utils/attr.h>
#include <utils/bytes.h>
#include <utils/debug.h>
#include <utils/random_utils.h>
#include <utils/rc.h>

#define ENIP_DEFAULT_PORT ((int)44818)

/* rx_cap before ForwardOpen succeeds (RegisterSession/ForwardOpen replies are small). */
#define ENIP_BOOTSTRAP_PACKET ((size_t)256)

/* Requested/negotiated CIP payload size for the standard ForwardOpen (0x43F8). */
#define ENIP_FO_CIP_SIZE ((size_t)504)

/* EIP header (24) + connected CPF overhead (22). Also used to size rx_cap on
 * the unconnected path, where the real CPF overhead is smaller -- over-sizing
 * the receive buffer is harmless, under-sizing it drops replies. */
#define ENIP_FRAMING_OVERHEAD \
    (EIP_HEADER_SIZE + CPF_HEADER_SIZE + CPF_CONNECTED_ADDR_ITEM_SIZE + CPF_CONNECTED_DATA_ITEM_SIZE + CPF_CONN_SEQ_NUM_SIZE)

/* Unconnected_Send (0x52) envelope around an embedded message: service(1) +
 * path_size_words(1) + Connection Manager path(4) + priority/ticks(1) +
 * timeout_ticks(1) + embedded_len(2), then after the embedded message a
 * route_path_size(1) + reserved(1). The odd-length alignment pad before the
 * route is counted too, so the number is a ceiling for any embedded length. */
#define ENIP_UNCONN_SEND_OVERHEAD ((size_t)13)

/* Backing store for the per-connection arena; covers rx_cap (max_cip_packet_size
 * + ENIP_FRAMING_OVERHEAD) and tx-side scratch. Building a write request
 * concatenates the CIP payload, then the CPF wrap, then the EIP frame -- each
 * step allocates a fresh buffer without freeing the last, so a single
 * max-size write request can consume ~3x its wire size before arena_reset.
 * Sized for the largest requested_cip_size any dialect asks for via Large
 * Forward Open (currently 4002, enip_logix_dialect/enip_omron_dialect --
 * see ENIP-SESSION-DESIGN.md §16.4) with headroom; bump this if a future
 * dialect requests something bigger. */
#define ENIP_ARENA_SIZE ((size_t)16384)

/* Backoff after a connect/IO failure, and idle poll cadence. */
#define ENIP_RECONNECT_DELAY_MS ((int64_t)1000)
#define ENIP_IDLE_WAIT_MS ((int64_t)1000)

/* Scheduler op_time for a special tag that has nothing pending: parked far in
 * the future, made due again by set_conn_status (or a read) when work appears. */
#define ENIP_FAR_FUTURE ((int64_t)INT64_MAX)
/* Retry cadence for an @identity tag whose payload is not cached yet. */
#define ENIP_SPECIAL_RETRY_MS ((int64_t)50)

/* Inactivity timeout bounds; default is the maximum. After this much idle time
 * with no scheduled work, the session disconnects and waits for new work. */
#define ENIP_MIN_INACTIVITY_MS ((int64_t)1000)
#define ENIP_MAX_INACTIVITY_MS ((int64_t)30000)

/* Upper bound on batch array sizes (stack-allocated); the actual runtime limit
 * c->max_batch is derived from the negotiated CIP payload after ForwardOpen
 * and is always clamped to this value. */
#define ENIP_BATCH_ARRAY_SIZE \
    ((size_t)((ENIP_FO_CIP_SIZE - CIP_CONNECTED_ITEM_OVERHEAD - ENIP_MS_REQ_FIXED) / ENIP_MS_MIN_SUB_REQ_SIZE))

/* originator vendor id / serial number used in ForwardOpen (arbitrary but fixed). */
#define ENIP_VENDOR_ID ((uint16_t)0xF33D)
#define ENIP_ORIGINATOR_SERIAL ((uint32_t)0x21504345)

/* CIP Identity classification: see common/plc_classify.h for why this is
 * centralized (vendor id + product-name catalog-family prefix, matched
 * against a table shared with the server-side emulator). */

/* §4/§5: connection IO thread states. */
enum {
    CONN_CONNECT = 0,
    CONN_REGISTER,
    CONN_IDENTITY, /* Get_Attributes_All on the CIP Identity object, before ForwardOpen */
    CONN_OPEN,
    CONN_READY,
    CONN_SENDING,
    CONN_WAITING,
    CONN_CLOSE, /* sending a ForwardClose before an idle teardown */
    CONN_IDLE,  /* disconnected after inactivity timeout; reconnects when work arrives */
    CONN_CLOSING
};

/* struct enip_connection_t's full definition moved to
 * enip_connection_internal.h (3.d) -- shared with the dialect plugins
 * (dialects/rockwell/logix_client.c, dialects/omron/omron_client.c,
 * dialects/pccc/pccc_client.c), which need field access the same way this
 * file does. Still opaque to everything outside this seam via
 * enip_session.h. */

/* Connection registry (§13.7). Colocated here -- enip_session_create() does
 * the find-or-create and link; conn_destructor() unlinks. */
static enip_connection_t *s_conns = NULL;
static mutex_p s_registry_mutex = NULL;

static void sched_insert_sorted(enip_connection_t *c, enip_tag_p t);
static void sched_unlink(enip_connection_t *c, enip_tag_p t);
static void pick_batch(enip_connection_t *c, int64_t now, int64_t *wait_ms);
static void service_due_tags(enip_connection_t *c, int64_t now);
static int64_t rearm_time(enip_tag_p t, int64_t now);
static int64_t next_special_wait(enip_connection_t *c, int64_t now, int64_t cap);
static int32_t build_request(enip_connection_t *c);
static int32_t build_tag_request(enip_connection_t *c, enip_tag_p t);
static int32_t build_batch_request(enip_connection_t *c);
static void complete_tag(enip_connection_t *c, enip_tag_p t, int8_t status);
static void complete_batch(enip_connection_t *c, int8_t status);
static void handle_batch_reply(enip_connection_t *c, eip_hdr_t *hdr, Bytes payload);
static void reset_connection(enip_connection_t *c);
static void idle_disconnect(enip_connection_t *c);
static void set_conn_status(enip_connection_t *c, uint8_t status);
static void conn_destructor(void *arg);
static THREAD_FUNC(io_thread_func);

static int32_t step_connect(enip_connection_t *c);
static int32_t step_register(enip_connection_t *c);
static int32_t step_open(enip_connection_t *c);
static int32_t step_close(enip_connection_t *c);
static int32_t step_sending(enip_connection_t *c);
static int32_t step_waiting(enip_connection_t *c);

static void on_register_reply(enip_connection_t *c, eip_hdr_t *hdr);
static void on_open_reply(enip_connection_t *c, eip_hdr_t *hdr, Bytes payload);
static void on_close_reply(enip_connection_t *c, eip_hdr_t *hdr, Bytes payload);
static void handle_tag_reply(enip_connection_t *c, eip_hdr_t *hdr, Bytes payload);
static const enip_dialect_t *listing_dialect_for(enip_connection_t *c);

/* A tag that issues network ops via pick_batch (vs. the special tags that the
 * service pass handles): a data tag, a PCCC tag, or an @tags/@udt listing tag.
 * PCCC (PLC-5/SLC/MicroLogix) tags are not batch-eligible (is_batch_eligible
 * requires ENIP_TAG_KIND_DATA; PCCC's own dialect also caps max_batch_cap at
 * 1), but they still need pick_batch's single-in-flight dispatch to ever send
 * a request -- without this, service_due_tags's tickler-only pass runs
 * forever and a PCCC read/write never leaves the scheduler. */
static inline bool is_network_op_kind(enip_tag_p t) {
    return t->kind == ENIP_TAG_KIND_DATA || t->kind == ENIP_TAG_KIND_PCCC || t->kind == ENIP_TAG_KIND_LISTING
           || t->kind == ENIP_TAG_KIND_UDT;
}

static Bytes build_forward_open(enip_connection_t *c, bool use_large);
static int32_t parse_forward_open_reply(enip_connection_t *c, Bytes cip_reply_bytes);
static Bytes wrap_unconnected_frame(enip_connection_t *c, Bytes embedded);
static Bytes wrap_tag_frame(enip_connection_t *c, Bytes cip_req);
static Bytes build_forward_close(enip_connection_t *c);
static int32_t step_identity(enip_connection_t *c);
static Bytes build_identity_request(enip_connection_t *c);
static void on_identity_reply(enip_connection_t *c, eip_hdr_t *hdr, Bytes payload);

/* ============================================================================
 * Registry / lifecycle
 * ============================================================================ */

/*
 * Select the transport for tag requests and size the per-request framing
 * budget that follows from it. Called with false during create (bring-up is
 * unconnected on both paths) and again from on_identity_reply once the PLC
 * family is known.
 */
/*
 * How many sub-requests one Multiple Service Packet may hold, from the CIP
 * payload now available. Both transports need this: connected sizes it after
 * ForwardOpen, unconnected after Identity. Never zero -- pick_batch dispatches
 * even a single tag through the batch path.
 */
static void set_batch_limit(enip_connection_t *c) {
    size_t cip_payload = (c->max_cip_packet_size > c->cip_overhead) ? (c->max_cip_packet_size - c->cip_overhead) : 0;
    size_t sub_space = (cip_payload > ENIP_MS_REQ_FIXED) ? (cip_payload - ENIP_MS_REQ_FIXED) : 0;
    size_t computed_max = sub_space / ENIP_MS_MIN_SUB_REQ_SIZE;

    if(computed_max > ENIP_BATCH_ARRAY_SIZE) { computed_max = ENIP_BATCH_ARRAY_SIZE; }
    if(computed_max < 1) { computed_max = 1; }

    c->max_batch = (uint16_t)computed_max;
}

static void set_path_mode(enip_connection_t *c, bool connected) {
    c->is_connected_path = connected;

    if(connected) {
        c->cip_overhead = CIP_CONNECTED_ITEM_OVERHEAD;
    } else {
        c->cip_overhead = CPF_UNCONNECTED_DATA_ITEM_SIZE + (c->route_len ? ENIP_UNCONN_SEND_OVERHEAD + c->route_len : 0);
    }
}

static bool conn_key_matches(enip_connection_t *c, const char *gateway, const char *path, int port, bool pref_set, bool pref) {
    return c->tcp_port == port && c->connected_pref_set == pref_set && (!pref_set || c->connected_pref == pref)
           && str_cmp(c->gateway, gateway) == 0 && str_cmp(c->path, path) == 0;
}

static enip_connection_t *create_connection(const char *gateway, const char *path, const char *model, int port, bool pref_set,
                                            bool pref) {
    enip_connection_t *c = rc_alloc((int)sizeof(enip_connection_t), conn_destructor);
    if(!c) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to allocate connection!");
        return NULL;
    }

    atomic_init_bool(&c->terminate, false);

    c->gateway = str_dup(gateway);
    c->path = str_dup(path);
    /* model is optional (str_dup(NULL) is NULL, not an error -- unlike
     * gateway/path, which are required and checked below). */
    c->model = (model && model[0] != '\0') ? str_dup(model) : NULL;
    c->tcp_port = port;
    c->connected_pref_set = pref_set;
    c->connected_pref = pref;
    c->try_large_fo = true;

    if(!c->gateway || !c->path) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to copy gateway/path strings!");
        rc_dec(c);
        return NULL;
    }

    c->our_conn_id = (uint32_t)random_u64(0xFFFFFFFFu);
    if(c->our_conn_id == 0) { c->our_conn_id = 1; }

    c->conn_serial = (uint16_t)(random_u64(0xFFFFu) + 1u);

    if(arena_init(&c->arena, ENIP_ARENA_SIZE) != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to allocate connection arena!");
        rc_dec(c);
        return NULL;
    }

    if(mutex_create(&c->sched_mutex) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to create scheduler mutex!");
        rc_dec(c);
        return NULL;
    }

    /*
     * Measure the route once, while the arena is still empty. A malformed path
     * is not diagnosed here -- bring-up reports it where the route is actually
     * built.
     */
    if(c->path[0] != '\0') {
        Bytes route = enip_cip_encode_route(&c->arena, c->path);
        c->route_len = bytes_is_null(route) ? 0 : route.len;
        arena_reset(&c->arena);
    }

    /* Bring-up is unconnected either way; on_identity_reply settles this. */
    set_path_mode(c, false);

    c->state = CONN_CONNECT;
    c->resume_state = CONN_CONNECT;
    c->dialect = &enip_logix_dialect; /* reselected from Identity at bring-up */
    c->rx_cap = ENIP_BOOTSTRAP_PACKET;
    c->max_cip_packet_size = ENIP_FO_CIP_SIZE;
    c->inactivity_timeout_ms = ENIP_MAX_INACTIVITY_MS;
    c->last_activity_ms = time_ms();

    /* Start at DOWN with an empty ring, then record the CONNECTING transition so a
     * fresh @connection tag (read idx 0) observes CONNECTING before UP. */
    c->conn_status = (uint8_t)PLCTAG_CONN_STATUS_DOWN;
    atomic_init_int32(&c->conn_status_ring_write_idx, 0);
    set_conn_status(c, (uint8_t)PLCTAG_CONN_STATUS_CONNECTING);

    if(thread_create(&c->thread, io_thread_func, 32768, (void *)c) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to create IO thread!");
        rc_dec(c);
        return NULL;
    }

    c->next = s_conns;
    s_conns = c;

    return c;
}

extern enip_connection_t *enip_session_create(attr attribs, bool *is_new_out) {
    const char *gateway_raw = attr_get_str(attribs, "gateway", NULL);
    /* "path" is optional: a device reachable directly over Ethernet (no
     * backplane/DH+ bridging hop -- the common case for a MicroLogix/SLC/PLC-5
     * with its own Ethernet port) needs no CIP route. enip_cip_encode_route
     * treats "" as a valid empty route, not an error. */
    const char *path = attr_get_str(attribs, "path", "");
    /* model= overrides identity-based classification for this connection
     * (see on_identity_reply); only used when creating a new connection --
     * an existing connection at this (gateway,path,port) keeps whatever its
     * first tag set, same as path/gateway themselves. */
    const char *model = attr_get_str(attribs, "model", NULL);

    /*
     * use_connected_msg= overrides the transport the identified PLC family
     * would pick (enip_plc_prefers_connected). Same attribute name as
     * protocols/ab, but no default here: absent means "let the family decide",
     * which ab could not express because it has no auto-detection.
     */
    bool connected_pref_set = (attr_get_str(attribs, "use_connected_msg", NULL) != NULL);
    bool connected_pref = (attr_get_int(attribs, "use_connected_msg", 0) != 0);

    if(is_new_out) { *is_new_out = false; }

    if(!gateway_raw || str_length(gateway_raw) == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Missing required \"gateway\" attribute.");
        return NULL;
    }

    /* No separate "port" attribute: gateway is "host" or "host:port" (same
     * convention as protocols/ab/session.c's server_port split), the only
     * place a non-default port is ever specified. */
    char **host_port = str_split(gateway_raw, ":");
    if(!host_port || !host_port[0]) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Malformed \"gateway\" attribute \"%s\".", gateway_raw);
        if(host_port) { mem_free(host_port); }
        return NULL;
    }

    const char *gateway = host_port[0];
    int port = ENIP_DEFAULT_PORT;
    if(host_port[1] && str_to_int(host_port[1], &port) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to extract port number from gateway string \"%s\".", gateway_raw);
        mem_free(host_port);
        return NULL;
    }

    enip_connection_t *result = NULL;

    critical_block(s_registry_mutex) {
        enip_connection_t *c = s_conns;

        while(c != NULL) {
            if(conn_key_matches(c, gateway, path, port, connected_pref_set, connected_pref)) {
                result = rc_inc(c);
                break;
            }
            c = c->next;
        }

        if(!result) {
            result = create_connection(gateway, path, model, port, connected_pref_set, connected_pref);
            if(result && is_new_out) { *is_new_out = true; }
        }
    }

    mem_free(host_port);

    return result;
}

extern int32_t enip_session_module_init(void) {
    if(mutex_create(&s_registry_mutex) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to create registry mutex!");
        return PLCTAG_ERR_CREATE;
    }

    s_conns = NULL;

    return PLCTAG_STATUS_OK;
}

extern void enip_session_module_teardown(void) {
    /* Connections are referenced by tags; by teardown time all tags should
     * already have been destroyed, but walk defensively and drop our
     * (nonexistent) extra refs is not needed -- just ensure the mutex is
     * freed once the list is empty. */
    if(s_registry_mutex) {
        mutex_destroy(&s_registry_mutex);
        s_registry_mutex = NULL;
    }

    s_conns = NULL;
}

static void conn_destructor(void *arg) {
    enip_connection_t *c = (enip_connection_t *)arg;

    if(!c) { return; }

    atomic_set_bool(&c->terminate, true);

    if(c->sock) { socket_wake(c->sock); }

    if(c->thread) {
        thread_join(c->thread);
        thread_destroy(&c->thread);
    }

    if(c->sock) { socket_destroy(&c->sock); }

    if(c->sched_mutex) { mutex_destroy(&c->sched_mutex); }

    arena_free(&c->arena);

    if(s_registry_mutex) {
        critical_block(s_registry_mutex) {
            enip_connection_t **walker = &s_conns;
            while(*walker && *walker != c) { walker = &(*walker)->next; }
            if(*walker) { *walker = c->next; }
        }
    }

    if(c->gateway) { mem_free(c->gateway); }
    if(c->path) { mem_free(c->path); }
    if(c->model) { mem_free(c->model); }
    if(c->identity_data) { mem_free(c->identity_data); }
}

/* ============================================================================
 * Scheduler list (§3, §13.2-13.6)
 * ============================================================================ */

static void sched_insert_sorted(enip_connection_t *c, enip_tag_p t) {
    enip_tag_p cur = c->sched_head;

    while(cur != NULL && cur->op_time <= t->op_time) { cur = cur->sched_next; }

    if(cur == NULL) {
        t->sched_prev = c->sched_tail;
        t->sched_next = NULL;

        if(c->sched_tail) { c->sched_tail->sched_next = t; }
        else { c->sched_head = t; }

        c->sched_tail = t;
    } else {
        t->sched_next = cur;
        t->sched_prev = cur->sched_prev;

        if(cur->sched_prev) { cur->sched_prev->sched_next = t; }
        else { c->sched_head = t; }

        cur->sched_prev = t;
    }

    t->scheduled = 1;
}

static void sched_unlink(enip_connection_t *c, enip_tag_p t) {
    if(t->sched_prev) { t->sched_prev->sched_next = t->sched_next; }
    else { c->sched_head = t->sched_next; }

    if(t->sched_next) { t->sched_next->sched_prev = t->sched_prev; }
    else { c->sched_tail = t->sched_prev; }

    t->sched_prev = NULL;
    t->sched_next = NULL;
    t->scheduled = 0;
}

/* Per-op traits (3.e / plan item 3.3): complete_tag, build_tag_request, and
 * is_batch_eligible each used to re-enumerate ENIP_OP_* in their own ||/switch
 * ladder -- the exact bug class that produced last session's OMRON hang (a
 * missing arm in one of them). One table now, sized off the last enip_op_t
 * value; a new op appended after ENIP_OP_UDT_FIELDS defaults to all-false
 * traits (not a compile error -- C's designated-initializer arrays can't
 * enforce "every index named"), so still audit this table by hand whenever
 * enip_op_t grows. The _Static_assert only guards ENIP_OP_COUNT itself
 * against drifting out of sync with the macro that derives it. */
#define ENIP_OP_COUNT (ENIP_OP_UDT_FIELDS + 1)

/* apply_tag_reply (3.f) per-op handlers: parse one CIP sub-reply already
 * routed to the right op, copy into t->data, set *frag_more via t->frag_more
 * when another round trip is due. Only the network-data ops (not IDLE, not
 * the listing ops -- those are the dialect's own apply_listing) have one;
 * apply_tag_reply's dispatch treats a NULL entry as "unexpected op". */
static int32_t apply_open_probe(enip_connection_t *c, enip_tag_p t, uint8_t status, Bytes data);
static int32_t apply_open_probe_frag(enip_connection_t *c, enip_tag_p t, uint8_t status, Bytes data);
static int32_t apply_open_bulk(enip_connection_t *c, enip_tag_p t, uint8_t status, Bytes data);
static int32_t apply_read(enip_connection_t *c, enip_tag_p t, uint8_t status, Bytes data);
static int32_t apply_write(enip_connection_t *c, enip_tag_p t, uint8_t status, Bytes data);

typedef struct {
    bool creates;    /* completion raises PLCTAG_EVENT_CREATED (the OPEN_* probe/bulk sequence) */
    bool reads;      /* completion raises PLCTAG_EVENT_READ_COMPLETED (data read, or @tags/@udt) */
    bool writes;     /* completion raises PLCTAG_EVENT_WRITE_COMPLETED */
    bool listing;    /* build_tag_request routes through the dialect's build_listing, not build */
    bool batchable;  /* is_batch_eligible may fold this op into a Multiple Service Packet */
    int32_t (*apply)(enip_connection_t *c, enip_tag_p t, uint8_t status, Bytes data); /* apply_tag_reply's handler, or NULL */
} enip_op_traits_t;

static const enip_op_traits_t op_traits[ENIP_OP_COUNT] = {
    [ENIP_OP_IDLE]            = {0},
    [ENIP_OP_READ]            = {.reads = true, .batchable = true, .apply = apply_read},
    [ENIP_OP_WRITE]           = {.writes = true, .batchable = true, .apply = apply_write},
    [ENIP_OP_OPEN_PROBE]      = {.creates = true, .apply = apply_open_probe},
    [ENIP_OP_OPEN_PROBE_FRAG] = {.creates = true, .apply = apply_open_probe_frag},
    [ENIP_OP_OPEN_BULK]       = {.creates = true, .apply = apply_open_bulk},
    [ENIP_OP_LIST]            = {.reads = true, .listing = true},
    [ENIP_OP_UDT_META]        = {.reads = true, .listing = true},
    [ENIP_OP_UDT_FIELDS]      = {.reads = true, .listing = true},
};

_Static_assert(sizeof(op_traits) / sizeof(op_traits[0]) == ENIP_OP_COUNT, "op_traits must cover every enip_op_t");

static bool is_batch_eligible(enip_tag_p t) {
    if(t->kind != ENIP_TAG_KIND_DATA) { return false; }
    if(atomic_get_bool(&t->abort_requested)) { return false; }
    if(!t->ready) { return false; }
    /* §16a.6: a fragmented element needs its own exclusive Read/WriteFrag
     * continuation loop; it can never be folded into a Multiple Service
     * Packet alongside other tags. */
    if(t->fragmented_elem) { return false; }
    if(!op_traits[t->op].batchable) { return false; }
    if(t->op == ENIP_OP_READ) { return t->elem_count <= t->window_elems; }
    return t->elem_count <= t->write_window_elems; /* ENIP_OP_WRITE, the only other batchable op */
}

static size_t batch_req_size(enip_tag_p t) {
    size_t path_len = (size_t)t->path.len;
    if(t->elem_count > 1) { path_len += 2; } /* index segment for [0] */
    if(t->op == ENIP_OP_READ) { return (size_t)4 + path_len; }
    /* WRITE: service(1)+path_size(1)+elem_count(2) + path + type_header + data */
    return (size_t)4 + path_len + (size_t)t->type_header_len + (size_t)t->elem_size * (size_t)t->elem_count;
}

static size_t batch_resp_size(enip_tag_p t) {
    if(t->op == ENIP_OP_WRITE) { return (size_t)CIP_READ_REPLY_OVERHEAD; }
    /* READ: reply hdr(4) + type_header + element data */
    return (size_t)CIP_READ_REPLY_OVERHEAD + (size_t)t->type_header_len + (size_t)t->elem_size * (size_t)t->elem_count;
}

static void pick_batch(enip_connection_t *c, int64_t now, int64_t *wait_ms) {
    *wait_ms = ENIP_IDLE_WAIT_MS;

    critical_block(c->sched_mutex) {
        if(c->in_flight != NULL || c->batch_head != NULL) { break; }

        /* Special tags (@connection/@identity) are serviced by service_due_tags,
         * not the network-op path; skip them to find the first network-op tag.
         * @tags/@udt are not batch-eligible, so they fall through to the single
         * in_flight dispatch below. */
        enip_tag_p head = c->sched_head;
        while(head != NULL && !is_network_op_kind(head)) { head = head->sched_next; }

        if(head == NULL || head->op_time > now) {
            if(head != NULL) {
                int64_t remaining = head->op_time - now;
                *wait_ms = (remaining < 0) ? 0 : remaining;
            }
            break;
        }

        if(!is_batch_eligible(head)) {
            sched_unlink(c, head);
            atomic_set_bool(&head->abort_requested, false);
            c->in_flight = rc_inc(head);
            break;
        }

        size_t req_budget = c->max_cip_packet_size - c->cip_overhead - ENIP_MS_REQ_FIXED;
        size_t resp_budget = c->max_cip_packet_size - c->cip_overhead - ENIP_MS_RESP_FIXED;

        enip_tag_p batch_tail = NULL;
        uint16_t count = 0;
        enip_tag_p cur = head;

        while(cur != NULL && count < c->max_batch && cur->op_time <= now && is_batch_eligible(cur)) {
            size_t rs = (size_t)2 + batch_req_size(cur);
            size_t ps = (size_t)2 + batch_resp_size(cur);

            if(rs > req_budget || ps > resp_budget) { break; }

            req_budget -= rs;
            resp_budget -= ps;

            enip_tag_p next_sched = cur->sched_next;
            sched_unlink(c, cur);
            atomic_set_bool(&cur->abort_requested, false);
            rc_inc(cur);
            cur->batch_next = NULL;

            if(batch_tail == NULL) { c->batch_head = cur; }
            else { batch_tail->batch_next = cur; }
            batch_tail = cur;
            count++;
            cur = next_sched;
        }

        if(count == 0) {
            /* First eligible tag exceeds batch budget; fall back to single in-flight. */
            sched_unlink(c, head);
            atomic_set_bool(&head->abort_requested, false);
            c->in_flight = rc_inc(head);
        } else if(count == 1) {
            /* Not worth a Multi-Service wrapper for one tag; use in-flight path. */
            c->in_flight = c->batch_head;
            c->batch_head = NULL;
            c->batch_count = 0;
        } else {
            c->batch_count = count;
            c->batch_complete_idx = 0;
        }
    }
}

/* Where a tag goes after service_due_tags runs its tickler:
 *   - @connection: parked far in the future (set_conn_status re-arms it).
 *   - @identity: short retry until the cached payload exists, then parked.
 *   - data tag with a live network op: stays "now" for pick_batch.
 *   - data auto-sync timer: the earliest of its next read/write fire time. */
static int64_t rearm_time(enip_tag_p t, int64_t now) {
    if(t->kind == ENIP_TAG_KIND_CONNECTION) {
        /* The first tickler run bails until CREATED is dispatched; keep retrying
         * until it has actually emitted the initial/late-join state, then park
         * (set_conn_status re-arms it on the next transition). */
        return t->first_tickler_run ? (now + ENIP_SPECIAL_RETRY_MS) : ENIP_FAR_FUTURE;
    }

    if(t->kind == ENIP_TAG_KIND_IDENTITY) {
        return t->read_in_flight ? (now + ENIP_SPECIAL_RETRY_MS) : ENIP_FAR_FUTURE;
    }

    /* Idle data tag: park at its next auto-sync fire time (pick_batch owns a tag
     * once it has a live op, so service_due_tags never re-arms a non-idle one). */
    int64_t next = ENIP_FAR_FUTURE;
    if(t->auto_sync_read_ms > 0 && t->auto_sync_next_read < next) { next = t->auto_sync_next_read; }
    if(t->auto_sync_write_ms > 0 && t->auto_sync_next_write > 0 && t->auto_sync_next_write < next) {
        next = t->auto_sync_next_write;
    }
    return next;
}

/* IO-thread tickler for every due tag in the active list: replaces the global
 * tag tickler for ENIP (all ENIP tags set skip_tickler). Each tag is pinned
 * across its api_mutex section so an API-thread destroy/unschedule cannot free
 * it mid-service. c->sched_cursor always points at the pinned tag during the
 * lock-release window, so removing the tag after it (sched_unlink) keeps the
 * walk's successor pointer valid; the pinned tag itself cannot be removed. */
/* First tag at or after `from` (following sched_next) that service_due_tags
 * should run now: due (op_time <= now) and not a data tag with a live network
 * op. A live-op data tag is pick_batch's to dispatch -- ticking it here would
 * spin (it stays due until pick_batch consumes it). The list is sorted by
 * op_time, so the first tag with op_time > now ends the due prefix. */
static enip_tag_p next_serviceable(enip_tag_p from, int64_t now) {
    for(enip_tag_p t = from; t != NULL && t->op_time <= now; t = t->sched_next) {
        if(is_network_op_kind(t) && t->op != ENIP_OP_IDLE) { continue; }
        return t;
    }
    return NULL;
}

static void service_due_tags(enip_connection_t *c, int64_t now) {
    enip_tag_p t = NULL;

    critical_block(c->sched_mutex) {
        t = next_serviceable(c->sched_head, now);
        if(t != NULL) {
            rc_inc(t);
            c->sched_cursor = t;
        }
    }

    while(t != NULL) {
        mutex_lock(t->api_mutex);
        plc_tag_generic_tickler((plc_tag_p)t);
        if(t->vtable && t->vtable->tickler) { t->vtable->tickler((plc_tag_p)t); }
        plc_tag_generic_handle_event_callbacks((plc_tag_p)t);
        plc_tag_generic_wake_tag((plc_tag_p)t); /* wake any synchronous read/write */
        mutex_unlock(t->api_mutex);

        enip_tag_p next = NULL;

        critical_block(c->sched_mutex) {
            if(t->scheduled) {
                enip_tag_p after = t->sched_next;

                /* Re-arm only if the tickler did not hand the tag to pick_batch
                 * (i.e. it is a special tag, or still an idle data tag). A tag
                 * whose tickler just armed a read/write is now at op_time=now for
                 * pick_batch -- leave it in place. */
                if(!is_network_op_kind(t) || t->op == ENIP_OP_IDLE) {
                    sched_unlink(c, t);
                    t->op_time = rearm_time(t, now);
                    sched_insert_sorted(c, t);
                }

                next = next_serviceable(after, now);
            }

            if(next != NULL) { rc_inc(next); }
            c->sched_cursor = next;
        }

        rc_dec(t);
        t = next;
    }

    critical_block(c->sched_mutex) { c->sched_cursor = NULL; }
}

/* Delay (ms, capped at cap) until the next @connection/@identity tag needs
 * servicing. Special tags are serviceable in any connection state, so the IO
 * loop clamps its wait to this; data tags are left to pick_batch (CONN_READY)
 * to avoid spinning while disconnected. */
static int64_t next_special_wait(enip_connection_t *c, int64_t now, int64_t cap) {
    int64_t wait = cap;

    critical_block(c->sched_mutex) {
        for(enip_tag_p t = c->sched_head; t != NULL; t = t->sched_next) {
            int64_t until = t->op_time - now;
            if(until >= wait) { break; } /* list is sorted: nothing sooner remains */
            if(!is_network_op_kind(t)) {
                wait = (until < 0) ? 0 : until;
                break;
            }
        }
    }

    return wait;
}

extern int32_t enip_session_schedule(enip_connection_t *c, enip_tag_p t, uint8_t op, int64_t op_time) {
    critical_block(c->sched_mutex) {
        t->op = op;

        /* Re-sort if already listed (e.g. an auto-sync timer or parked @identity
         * being pulled forward to fire now); plain insert otherwise. */
        if(t->scheduled) {
            if(t->op_time != op_time) {
                sched_unlink(c, t);
                t->op_time = op_time;
                sched_insert_sorted(c, t);
            }
        } else {
            t->op_time = op_time;
            sched_insert_sorted(c, t);
        }
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, t->tag_id,
           "schedule op=%d op_time=%" PRId64 " scheduled=%d sched_head=%p in_flight=%p", op, op_time,
           (int)t->scheduled, (void *)c->sched_head, (void *)c->in_flight);

    if(c->sock) { socket_wake(c->sock); }

    return PLCTAG_STATUS_PENDING;
}

extern int32_t enip_session_unschedule(enip_connection_t *c, enip_tag_p t) {
    critical_block(c->sched_mutex) {
        if(t->scheduled && t != c->in_flight) {
            sched_unlink(c, t);
            t->op = ENIP_OP_IDLE;
        } else if(t == c->in_flight) {
            atomic_set_bool(&t->abort_requested, true);
        }
    }

    if(c->sock) { socket_wake(c->sock); }

    return PLCTAG_STATUS_OK;
}

extern void enip_session_tag_detach(enip_connection_t *c, enip_tag_p t) {
    critical_block(c->sched_mutex) {
        if(t->scheduled) { sched_unlink(c, t); }
    }
}

extern size_t enip_session_max_cip(enip_connection_t *c) { return c->max_cip_packet_size; }

extern int enip_session_get_status(enip_connection_t *c) { return (int)c->conn_status; }

/* Cached CIP Identity payload (raw Get_Attributes_All response). Returns false
 * until the bring-up identity query has completed. */
extern bool enip_session_get_identity(enip_connection_t *c, uint8_t **data_out, uint16_t *len_out) {
    if(!c || !c->identity_valid) { return false; }
    if(data_out) { *data_out = c->identity_data; }
    if(len_out) { *len_out = c->identity_len; }
    return true;
}

extern enip_plc_type_t enip_session_get_plc_type(enip_connection_t *c) { return c ? c->plc_type : ENIP_PLC_UNKNOWN; }

extern int enip_session_get_inactivity_timeout(enip_connection_t *c) { return (int)c->inactivity_timeout_ms; }

/* Clamp to [ENIP_MIN, ENIP_MAX]; returns PLCTAG_ERR_OUT_OF_BOUNDS (and still
 * stores the clamped value) if the request was out of range. */
extern int enip_session_set_inactivity_timeout(enip_connection_t *c, int new_value) {
    int64_t v = (int64_t)new_value;
    int rc = PLCTAG_STATUS_OK;

    if(v > ENIP_MAX_INACTIVITY_MS) {
        v = ENIP_MAX_INACTIVITY_MS;
        rc = PLCTAG_ERR_OUT_OF_BOUNDS;
    } else if(v < ENIP_MIN_INACTIVITY_MS) {
        v = ENIP_MIN_INACTIVITY_MS;
        rc = PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    c->inactivity_timeout_ms = v;

    return rc;
}

/* Record a connection-status transition: update the field and, if it changed,
 * push it onto the ring for @connection tags to observe. IO-thread only. */
static void set_conn_status(enip_connection_t *c, uint8_t status) {
    if(c->conn_status == status) { return; }

    c->conn_status = status;

    int32_t idx = (atomic_get_int32(&c->conn_status_ring_write_idx) + 1) & ENIP_CONN_STATUS_RING_MASK;
    c->conn_status_ring[idx] = status;
    atomic_set_int32(&c->conn_status_ring_write_idx, idx);

    /* Wake every @connection tag so service_due_tags drains the new transition.
     * They are re-sorted to "now"; service re-parks them at ENIP_FAR_FUTURE. */
    critical_block(c->sched_mutex) {
        enip_tag_p t = c->sched_head;
        while(t != NULL) {
            enip_tag_p next = t->sched_next;
            if(t->kind == ENIP_TAG_KIND_CONNECTION && t->op_time > 0) {
                sched_unlink(c, t);
                t->op_time = 0;
                sched_insert_sorted(c, t);
            }
            t = next;
        }
    }
}

extern int32_t enip_session_conn_status_idx(enip_connection_t *c) { return atomic_get_int32(&c->conn_status_ring_write_idx); }

extern bool enip_session_next_conn_status(enip_connection_t *c, int32_t *read_idx, int32_t *status_out) {
    if(*read_idx == atomic_get_int32(&c->conn_status_ring_write_idx)) { return false; }

    *read_idx = (*read_idx + 1) & ENIP_CONN_STATUS_RING_MASK;
    *status_out = (int32_t)c->conn_status_ring[*read_idx];

    return true;
}

/* ============================================================================
 * IO thread state machine (§5)
 * ============================================================================ */

static int32_t step_connect(enip_connection_t *c) {
    int rc;

    if(time_ms() < c->reconnect_at_ms) { return PLCTAG_STATUS_PENDING; }

    if(!c->sock) {
        rc = socket_create(&c->sock);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "socket_create failed: %s.", plc_tag_decode_error(rc));
            c->reconnect_at_ms = time_ms() + ENIP_RECONNECT_DELAY_MS;
            return rc;
        }

        c->connect_started = false;
    }

    if(!c->connect_started) {
        rc = socket_connect_tcp_start(c->sock, c->gateway, c->tcp_port);

        if(rc == PLCTAG_STATUS_OK) {
            c->state = CONN_REGISTER;
            return PLCTAG_STATUS_OK;
        } else if(rc == PLCTAG_STATUS_PENDING) {
            c->connect_started = true;
            return PLCTAG_STATUS_PENDING;
        } else {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "socket_connect_tcp_start failed: %s.", plc_tag_decode_error(rc));
            socket_close(c->sock);
            c->reconnect_at_ms = time_ms() + ENIP_RECONNECT_DELAY_MS;
            return rc;
        }
    }

    rc = socket_connect_tcp_check(c->sock, 0);

    if(rc == PLCTAG_STATUS_OK) {
        c->connect_started = false;
        c->state = CONN_REGISTER;
    } else if(rc == PLCTAG_ERR_TIMEOUT) {
        return PLCTAG_STATUS_PENDING;
    } else {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "socket_connect_tcp_check failed: %s.", plc_tag_decode_error(rc));
        socket_close(c->sock);
        c->connect_started = false;
        c->reconnect_at_ms = time_ms() + ENIP_RECONNECT_DELAY_MS;
    }

    return PLCTAG_STATUS_OK;
}

static int32_t step_register(enip_connection_t *c) {
    arena_reset(&c->arena);

    Bytes frame = eip_frame(&c->arena, EIP_CMD_REGISTER_SESSION, 0, bytes_pack(&c->arena, BYTES_LE, (uint16_t)1, (uint16_t)0));
    if(bytes_is_null(frame)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to build RegisterSession request!");
        reset_connection(c);
        return PLCTAG_ERR_NO_MEM;
    }

    c->tx_buf = frame.data;
    c->tx_len = frame.len;
    c->tx_off = 0;
    c->resume_state = CONN_REGISTER;
    c->state = CONN_SENDING;

    return PLCTAG_STATUS_OK;
}

static int32_t step_open(enip_connection_t *c) {
    arena_reset(&c->arena);

    /* Use a fresh connection serial and O->T connection ID on every ForwardOpen
     * attempt. A reconnect after an idle/abrupt close would otherwise reuse the
     * triad of the connection the PLC still holds, and the target rejects it as
     * a duplicate (CIP status 0x01, ext 0x0100). */
    c->conn_serial++;
    if(c->conn_serial == 0) { c->conn_serial = 1; }

    c->our_conn_id = (uint32_t)random_u64(0xFFFFFFFFu);
    if(c->our_conn_id == 0) { c->our_conn_id = 1; }

    c->used_large_fo = c->try_large_fo;

    Bytes frame = build_forward_open(c, c->used_large_fo);
    if(bytes_is_null(frame)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to build ForwardOpen request!");
        reset_connection(c);
        return PLCTAG_ERR_NO_MEM;
    }

    c->tx_buf = frame.data;
    c->tx_len = frame.len;
    c->tx_off = 0;
    c->resume_state = CONN_OPEN;
    c->state = CONN_SENDING;

    return PLCTAG_STATUS_OK;
}

/* Send a ForwardClose for the active CIP connection ahead of an idle teardown.
 * If the request cannot be built, fall back to dropping the socket directly. */
static int32_t step_close(enip_connection_t *c) {
    arena_reset(&c->arena);

    Bytes frame = build_forward_close(c);
    if(bytes_is_null(frame)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to build ForwardClose request; dropping connection.");
        idle_disconnect(c);
        return PLCTAG_ERR_NO_MEM;
    }

    c->tx_buf = frame.data;
    c->tx_len = frame.len;
    c->tx_off = 0;
    c->resume_state = CONN_CLOSE;
    c->state = CONN_SENDING;

    return PLCTAG_STATUS_OK;
}

/* Fire PLCTAG_EVENT_DATA_SENT once per tag whose op is riding this tx
 * (single in_flight tag, or every member of a batch_head chain). Direct
 * tag->callback call, not tag_raise_event: like DATA_RECEIVED, this must
 * fire once per packet actually sent, not be coalesced by the generic
 * once-per-tickler-pass latch. Best-effort api_mutex, same as the rest of
 * the batch code -- a concurrent plc_tag_destroy skips, doesn't block. */
static void raise_data_sent(enip_connection_t *c) {
    enip_tag_p t = (c->batch_count >= 2) ? c->batch_head : c->in_flight;

    while(t != NULL) {
        enip_tag_p next = (c->batch_count >= 2) ? t->batch_next : NULL;

        if(mutex_try_lock(t->api_mutex) == PLCTAG_STATUS_OK) {
            plc_tag_p tag = (plc_tag_p)t;
            if(tag->callback) { tag->callback(tag->tag_id, PLCTAG_EVENT_DATA_SENT, PLCTAG_STATUS_OK, tag->userdata); }
            mutex_unlock(t->api_mutex);
        }

        t = next;
    }
}

static int32_t step_sending(enip_connection_t *c) {
    int rc = socket_write(c->sock, c->tx_buf + c->tx_off, (int)(c->tx_len - c->tx_off), 0);

    if(rc == PLCTAG_ERR_TIMEOUT) { return PLCTAG_STATUS_PENDING; }

    if(rc < 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "socket_write failed: %s.", plc_tag_decode_error(rc));
        reset_connection(c);
        return rc;
    }

    c->tx_off += (size_t)rc;

    if(c->tx_off < c->tx_len) { return PLCTAG_STATUS_PENDING; }

    raise_data_sent(c);

    arena_reset(&c->arena);

    c->rx_buf = arena_alloc(&c->arena, c->rx_cap);
    if(!c->rx_buf) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to allocate rx buffer!");
        reset_connection(c);
        return PLCTAG_ERR_NO_MEM;
    }

    c->rx_len = 0;
    c->tx_buf = NULL;
    c->tx_len = c->tx_off = 0;
    c->state = CONN_WAITING;

    return PLCTAG_STATUS_OK;
}

static int32_t step_waiting(enip_connection_t *c) {
    size_t needed = 0;

    if(c->rx_len < EIP_HEADER_SIZE) {
        needed = EIP_HEADER_SIZE - c->rx_len;
    } else {
        uint16_t plen = 0;
        bytes_unpack(bytes_from_buf(c->rx_buf, c->rx_len), BYTES_LE, BYTES_SKIP(2), &plen);
        size_t total = EIP_HEADER_SIZE + plen;

        if(total > c->rx_cap) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Reply packet (%zu bytes) exceeds rx buffer (%zu bytes)!", total,
                   c->rx_cap);
            reset_connection(c);
            return PLCTAG_ERR_TOO_LARGE;
        }

        if(c->rx_len < total) { needed = total - c->rx_len; }
    }

    if(needed > 0) {
        int rc = socket_read(c->sock, c->rx_buf + c->rx_len, (int)needed, 0);

        if(rc == PLCTAG_ERR_TIMEOUT) { return PLCTAG_STATUS_PENDING; }

        if(rc < 0) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "socket_read failed: %s.", plc_tag_decode_error(rc));
            reset_connection(c);
            return rc;
        }

        c->rx_len += (size_t)rc;

        if(c->rx_len < EIP_HEADER_SIZE) { return PLCTAG_STATUS_PENDING; }
    }

    uint16_t plen = 0;
    bytes_unpack(bytes_from_buf(c->rx_buf, c->rx_len), BYTES_LE, BYTES_SKIP(2), &plen);
    size_t total = EIP_HEADER_SIZE + plen;

    if(c->rx_len < total) { return PLCTAG_STATUS_PENDING; }

    eip_hdr_t hdr;
    Bytes payload;

    if(!eip_decode(bytes_from_buf(c->rx_buf, total), &hdr, &payload)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to decode EIP reply frame!");
        reset_connection(c);
        return PLCTAG_ERR_BAD_REPLY;
    }

    switch(c->resume_state) {
        case CONN_REGISTER: on_register_reply(c, &hdr); break;
        case CONN_IDENTITY: on_identity_reply(c, &hdr, payload); break;
        case CONN_OPEN: on_open_reply(c, &hdr, payload); break;
        case CONN_CLOSE: on_close_reply(c, &hdr, payload); break;
        case CONN_READY: handle_tag_reply(c, &hdr, payload); break;
        default:
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unexpected resume_state %d in CONN_WAITING!", c->resume_state);
            reset_connection(c);
            break;
    }

    return PLCTAG_STATUS_OK;
}

static void on_register_reply(enip_connection_t *c, eip_hdr_t *hdr) {
    if(hdr->status != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "RegisterSession failed, status 0x%08" PRIx32 ".", hdr->status);
        reset_connection(c);
        return;
    }

    c->session_handle = hdr->session_handle;

    /* Query identity once per device; an idle reconnect already knows it. */
    c->state = c->identity_valid ? CONN_OPEN : CONN_IDENTITY;
}

static void on_open_reply(enip_connection_t *c, eip_hdr_t *hdr, Bytes payload) {
    if(hdr->status != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ForwardOpen SendRRData failed, status 0x%08" PRIx32 ".", hdr->status);
        reset_connection(c);
        return;
    }

    uint16_t seq = 0;
    Bytes cip;

    if(!cpf_unwrap(payload, false, NULL, &seq, &cip)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to unwrap ForwardOpen CPF reply!");
        reset_connection(c);
        return;
    }

    int32_t rc = parse_forward_open_reply(c, cip);
    if(rc != PLCTAG_STATUS_OK) {
        /* A Large Forward Open (0x5B) rejected as unsupported is not a hard
         * failure: fall back to standard Forward Open (0x54) once, and
         * remember not to try Large again on this connection's later
         * reconnects (§16.4). Any other rejection, including the standard
         * attempt itself failing, is fatal to bring-up as before. */
        if(rc == PLCTAG_ERR_UNSUPPORTED && c->used_large_fo) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0,
                   "Target does not support Large Forward Open; falling back to standard Forward Open.");
            c->try_large_fo = false;
            c->state = CONN_OPEN;
            return;
        }

        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ForwardOpen rejected by target!");
        reset_connection(c);
        return;
    }

    c->state = CONN_READY;
}

/* ForwardClose reply during idle teardown: log any rejection but tear the
 * socket down regardless -- we are going idle either way. */
static void on_close_reply(enip_connection_t *c, eip_hdr_t *hdr, Bytes payload) {
    if(hdr->status != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ForwardClose SendRRData failed, status 0x%08" PRIx32 ".", hdr->status);
    } else {
        uint16_t seq = 0;
        Bytes cip;

        if(cpf_unwrap(payload, false, NULL, &seq, &cip)) {
            cip_reply_t reply;

            if(enip_cip_parse_reply(cip, &reply) && reply.status != 0) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ForwardClose rejected, CIP status 0x%02X (ext 0x%04X).", reply.status,
                       reply.ext_status);
            }
        }
    }

    idle_disconnect(c);
}

static void handle_tag_reply(enip_connection_t *c, eip_hdr_t *hdr, Bytes payload) {
    if(c->batch_count >= 2) {
        handle_batch_reply(c, hdr, payload);
        return;
    }

    enip_tag_p t = c->in_flight;

    if(!t) {
        c->state = CONN_READY;
        return;
    }

    if(mutex_try_lock(t->api_mutex) != PLCTAG_STATUS_OK) {
        /* Stay in CONN_WAITING with the full reply already buffered; retry
         * the lock next cycle without re-reading the socket. */
        return;
    }

    int8_t status;
    bool more_windows = false;

    if(hdr->status != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "SendUnitData failed, status 0x%08" PRIx32 ".", hdr->status);
        status = (int8_t)PLCTAG_ERR_BAD_REPLY;
    } else {
        uint16_t seq = 0;
        Bytes cip;

        if(!cpf_unwrap(payload, c->is_connected_path, NULL, &seq, &cip)) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to unwrap connected CPF reply!");
            status = (int8_t)PLCTAG_ERR_BAD_REPLY;
        } else if(t->kind == ENIP_TAG_KIND_LISTING || t->kind == ENIP_TAG_KIND_UDT) {
            /* @tags/@udt: a partial-transfer status (0x06) is continuation, not
             * an error; the dialect's apply_listing accumulates and sets
             * more_windows. build_listing being non-NULL (checked in
             * build_tag_request before a request for this tag kind is ever
             * sent) guarantees apply_listing is too. */
            cip_reply_t reply;
            if(!enip_cip_parse_reply(cip, &reply)) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to parse CIP reply!");
                status = (int8_t)PLCTAG_ERR_BAD_REPLY;
            } else if(reply.status != 0 && reply.status != CIP_STATUS_FRAG) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "CIP error 0x%02X (ext 0x%04X).", reply.status,
                       reply.ext_status);
                status = (int8_t)PLCTAG_ERR_REMOTE_ERR;
            } else {
                status = (int8_t)listing_dialect_for(c)->apply_listing(t, reply.status, reply.data, &more_windows);
            }
        } else {
            /* data tag: the dialect parses the reply, interprets status, copies
             * into t->data, and sets more_windows for the next round trip. PCCC
             * is selected per-tag (see enip_dialect.h); everything else uses the
             * connection's identity-selected dialect. */
            const enip_dialect_t *d = (t->kind == ENIP_TAG_KIND_PCCC) ? &enip_pccc_dialect : c->dialect;
            status = (int8_t)d->apply(c, t, cip, &more_windows);
        }
    }

    /* §11.2 abort check at the CONN_WAITING boundary, before building the
     * next OPEN_BULK window. */
    if(more_windows && !atomic_get_bool(&t->abort_requested)) {
        build_tag_request(c, t);

        mutex_unlock(t->api_mutex);

        return;
    }

    complete_tag(c, t, status);

    mutex_unlock(t->api_mutex);

    c->state = CONN_READY;
}

/* §11.5: number of elements in the current/next ENIP_OP_WRITE window, given
 * t->read_off (the write cursor) and t->write_window_elems (computed once at
 * OPEN_PROBE). Shared by build_tag_request (to size the outgoing request) and
 * apply_tag_reply (to advance the cursor by the same amount on success). */
extern uint32_t write_window_count(enip_tag_p t) {
    uint32_t n = t->elem_count - t->read_off;
    if(n > t->write_window_elems) { n = t->write_window_elems; }
    return n;
}

/* Discard any leftover t->data from a previous op and reset the
 * fragment-assembly cursor to empty. */
static void frag_reset(enip_tag_p t) {
    if(t->data) { mem_free(t->data); t->data = NULL; }
    t->size = 0;
    t->buf_cap = 0;
    t->frag_offset = 0;
}

/* Append `chunk` bytes at `src` to the end of t->data (grown by realloc;
 * realloc(NULL, n) is malloc(n), so this also covers the first fragment
 * after frag_reset), advancing t->size and t->frag_offset by chunk. The
 * shared "unknown total size yet, grow as fragments arrive" pattern used by
 * both legs of byte-fragmented single-element assembly (§16a.6): the first
 * fragment (apply_open_probe's CIP_STATUS_FRAG case) and every continuation
 * (apply_open_probe_frag). Returns false (t->data/size/frag_offset
 * unchanged) only on allocation failure. */
static bool frag_append(enip_tag_p t, const uint8_t *src, uint32_t chunk) {
    if(!tag_data_append((plc_tag_p)t, &t->buf_cap, bytes_from_buf(src, chunk))) { return false; }
    t->frag_offset += chunk;
    return true;
}

/* §11.2 count=1 probe: decode the reply's type header, learn elem_size, and
 * either (CIP_STATUS_FRAG) start byte-fragment assembly of one too-large
 * element, or allocate the full elem_count buffer and compute the
 * read/write windows (§11.3/§11.5) for the OPEN_BULK/READ/WRITE ops that
 * follow. enip_dialect_t-independent: every symbolic dialect's probe reply
 * has this same CIP Common Format header. op_traits[ENIP_OP_OPEN_PROBE].apply. */
static int32_t apply_open_probe(enip_connection_t *c, enip_tag_p t, uint8_t status, Bytes data) {
    uint8_t header_len = 0;
    uint32_t elem_size_hint = 0;
    tag_byte_order_t order;

    if(!enip_type_decode(data, &header_len, &elem_size_hint, &order)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to decode reply type header!");
        return PLCTAG_ERR_BAD_REPLY;
    }

    (void)elem_size_hint;

    if(data.len < (size_t)header_len) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Reply data shorter than type header!");
        return PLCTAG_ERR_BAD_REPLY;
    }

    if(header_len > sizeof(t->type_header)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Type header (%u bytes) is too large!", (unsigned int)header_len);
        return PLCTAG_ERR_BAD_REPLY;
    }

    bytes_pack_into(bytes_from_buf(t->type_header, sizeof(t->type_header)), BYTES_LE, bytes_from_buf(data.data, header_len));
    t->type_header_len = header_len;

    uint32_t chunk = (uint32_t)(data.len - (size_t)header_len);

    if(status == CIP_STATUS_FRAG) {
        /* §16a.6: the element does not fit one packet. Only a single
         * element (elem_count<=1) can be byte-fragmented this way -- an
         * array whose individual elements are each this large is a known
         * limitation (see the design doc's fragmentation section). */
        if(t->elem_count > 1) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id,
                   "Element exceeds the connection and elem_count>1 array fragmentation is not supported!");
            return PLCTAG_ERR_TOO_LARGE;
        }

        frag_reset(t);
        if(!frag_append(t, data.data + header_len, chunk)) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to allocate tag data buffer!");
            return PLCTAG_ERR_NO_MEM;
        }

        t->frag_more = true;
        t->op = ENIP_OP_OPEN_PROBE_FRAG;

        return PLCTAG_STATUS_OK;
    }

    uint32_t elem_size = chunk;

    t->elem_size = elem_size;

    size_t total_size = (size_t)elem_size * (size_t)t->elem_count;

    uint8_t *buf = mem_alloc((int)total_size);
    if(!buf) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to allocate tag data buffer!");
        return PLCTAG_ERR_NO_MEM;
    }

    if(t->data) { mem_free(t->data); }

    t->data = buf;
    t->size = (int32_t)total_size;
    t->buf_cap = total_size;

    bytes_pack_into(bytes_from_buf(t->data, (size_t)total_size), BYTES_LE, bytes_from_buf(data.data + header_len, elem_size));

    /* §11.3: window = clamp((cap - overhead - header_len) / elem_size, 1, elem_count) */
    uint32_t window = (uint32_t)(
        (c->max_cip_packet_size - c->cip_overhead - CIP_READ_REPLY_OVERHEAD - (size_t)header_len) / elem_size);
    if(window < 1) { window = 1; }
    if(window > t->elem_count) { window = t->elem_count; }
    t->window_elems = window;

    /* §11.5: write window is symmetric, but a write request also carries
     * the path inline (a read reply does not), so subtract the path
     * length too. For elem_count > 1, each window appends an array-index
     * segment to t->path; size that segment for the largest index
     * (elem_count - 1) so the window never shrinks mid-transfer. */
    size_t write_path_len = (size_t)t->path.len;
    if(t->elem_count > 1) {
        uint32_t max_index = t->elem_count - 1;
        write_path_len += (max_index <= 0xFFu) ? 2 : (max_index <= 0xFFFFu) ? 4 : 6;
    }

    size_t write_overhead = c->cip_overhead + CIP_WRITE_REQUEST_OVERHEAD + write_path_len + (size_t)header_len;

    uint32_t write_window = 1;
    if(write_overhead + (size_t)elem_size <= c->max_cip_packet_size) {
        write_window = (uint32_t)((c->max_cip_packet_size - write_overhead) / elem_size);
        if(write_window < 1) { write_window = 1; }
    }
    if(write_window > t->elem_count) { write_window = t->elem_count; }
    t->write_window_elems = write_window;

    t->read_off = 1;

    if(t->elem_count <= 1) {
        t->ready = 1;
    } else {
        t->op = ENIP_OP_OPEN_BULK;
    }

    return PLCTAG_STATUS_OK;
}

/* Continuation of a fragmented single-element OPEN_PROBE (§16a.6). Every
 * ReadFrag reply re-sends the type header (already known from the first
 * fragment); total size is not known upfront, so t->data grows as fragments
 * arrive via frag_append, matching the classic AB driver's
 * check_read_status_connected(). op_traits[ENIP_OP_OPEN_PROBE_FRAG].apply. */
static int32_t apply_open_probe_frag(enip_connection_t *c, enip_tag_p t, uint8_t status, Bytes data) {
    if(data.len < (size_t)t->type_header_len) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Reply data shorter than type header!");
        return PLCTAG_ERR_BAD_REPLY;
    }

    uint32_t chunk = (uint32_t)(data.len - (size_t)t->type_header_len);

    if(!frag_append(t, data.data + t->type_header_len, chunk)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to grow tag data buffer!");
        return PLCTAG_ERR_NO_MEM;
    }

    if(status == CIP_STATUS_FRAG) {
        t->frag_more = true;
        return PLCTAG_STATUS_OK;
    }

    /* final fragment: the element is now fully assembled. */
    t->elem_size = (uint32_t)t->size;
    t->elem_count = 1;
    t->window_elems = 1;
    t->write_window_elems = 1;
    t->frag_align = (t->elem_size < 8) ? (uint8_t)t->elem_size : (uint8_t)8;
    t->fragmented_elem = 1;
    t->frag_offset = 0;
    t->read_off = 1;
    t->ready = 1;
    t->op = ENIP_OP_OPEN_PROBE; /* cosmetic: complete_tag() keys CREATED off this */

    /* §16a.6: fixed per-request byte count for a fragmented WRITE, sized
     * from this connection's negotiated capacity; mirrors
     * write_window_elems's role for the array case (both build and
     * apply recompute the same chunk from fixed inputs, so they always
     * agree without passing state between them). */
    {
        size_t fixed = (size_t)1 /* service */ + 1 /* path_size_words */ + (size_t)t->path.len + (size_t)t->type_header_len
                     + 2 /* elem_count */ + 4 /* byte_offset */;
        size_t cap = c->max_cip_packet_size - c->cip_overhead;
        size_t usable = (cap > fixed) ? (cap - fixed) : 0;
        size_t frag_chunk = (usable / t->frag_align) * t->frag_align;
        t->frag_write_chunk = (frag_chunk > 0) ? (uint32_t)frag_chunk : (uint32_t)t->frag_align;
    }

    return PLCTAG_STATUS_OK;
}

/* §11.2 remaining elements after the probe, single shot for the MVP.
 * op_traits[ENIP_OP_OPEN_BULK].apply. */
static int32_t apply_open_bulk(enip_connection_t *c, enip_tag_p t, uint8_t status, Bytes data) {
    (void)c;
    (void)status;

    if(data.len < (size_t)t->type_header_len) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Reply data shorter than type header!");
        return PLCTAG_ERR_BAD_REPLY;
    }

    size_t data_len = data.len - (size_t)t->type_header_len;
    size_t returned = data_len / (size_t)t->elem_size;

    if(returned == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "OPEN_BULK reply contained no complete elements!");
        return PLCTAG_ERR_BAD_REPLY;
    }

    size_t remaining = (size_t)(t->elem_count - t->read_off);
    if(returned > remaining) { returned = remaining; }

    size_t copy_bytes = returned * (size_t)t->elem_size;
    size_t dest_off = (size_t)t->read_off * (size_t)t->elem_size;

    bytes_pack_into(bytes_from_buf(t->data + dest_off, copy_bytes), BYTES_LE,
                   bytes_from_buf(data.data + t->type_header_len, copy_bytes));

    t->read_off += (uint32_t)returned;

    if(t->read_off >= t->elem_count) { t->ready = 1; }

    return PLCTAG_STATUS_OK;
}

/* Post-probe read: a single already-fragmented element (t->fragmented_elem,
 * ReadFrag continuation), a lone element, or a windowed array chunk (same
 * accounting as OPEN_BULK). op_traits[ENIP_OP_READ].apply. */
static int32_t apply_read(enip_connection_t *c, enip_tag_p t, uint8_t status, Bytes data) {
    (void)c;

    if(data.len < (size_t)t->type_header_len) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Reply data shorter than type header!");
        return PLCTAG_ERR_BAD_REPLY;
    }

    if(t->fragmented_elem) {
        /* §16a.6: t->size is already fixed at the element's known total
         * (set once when OPEN_PROBE_FRAG finished assembling it); this walk
         * only refills it at t->frag_offset, so it deliberately does NOT use
         * frag_append (which would grow -- and mis-track t->size -- on
         * every call). The realloc here is a defensive fallback for a
         * frag_offset/size mismatch that should not happen in practice. */
        uint32_t chunk = (uint32_t)(data.len - (size_t)t->type_header_len);
        size_t dest_off = (size_t)t->frag_offset;
        size_t needed = dest_off + (size_t)chunk;

        if(needed > (size_t)t->size) {
            if(!tag_data_reserve((plc_tag_p)t, &t->buf_cap, needed)) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to grow tag data buffer!");
                return PLCTAG_ERR_NO_MEM;
            }
            t->size = (int32_t)needed;
            t->elem_size = (uint32_t)needed;
        }

        bytes_pack_into(bytes_from_buf(t->data + dest_off, (size_t)chunk), BYTES_LE,
                       bytes_from_buf(data.data + t->type_header_len, chunk));
        t->frag_offset += chunk;

        if(status == CIP_STATUS_FRAG) {
            t->frag_more = true;
        } else {
            t->frag_offset = 0;
        }

        return PLCTAG_STATUS_OK;
    }

    if(t->elem_count <= 1) {
        size_t copy_len = data.len - (size_t)t->type_header_len;
        if(copy_len > (size_t)t->size) { copy_len = (size_t)t->size; }

        bytes_pack_into(bytes_from_buf(t->data, copy_len), BYTES_LE, bytes_from_buf(data.data + t->type_header_len, copy_len));

        return PLCTAG_STATUS_OK;
    }

    /* §11.3: windowed read, same accounting as OPEN_BULK. */
    size_t data_len = data.len - (size_t)t->type_header_len;
    size_t returned = data_len / (size_t)t->elem_size;

    if(returned == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Read reply contained no complete elements!");
        return PLCTAG_ERR_BAD_REPLY;
    }

    size_t remaining = (size_t)(t->elem_count - t->read_off);
    if(returned > remaining) { returned = remaining; }

    size_t copy_bytes = returned * (size_t)t->elem_size;
    size_t dest_off = (size_t)t->read_off * (size_t)t->elem_size;

    bytes_pack_into(bytes_from_buf(t->data + dest_off, copy_bytes), BYTES_LE,
                   bytes_from_buf(data.data + t->type_header_len, copy_bytes));

    t->read_off += (uint32_t)returned;

    return PLCTAG_STATUS_OK;
}

/* CIP write replies carry no data -- just advance the byte-fragment or
 * element-window cursor by whatever build_tag_request just sent (recomputed
 * here, not passed in -- deterministic from fixed inputs, so build and
 * apply always agree without exchanging state). op_traits[ENIP_OP_WRITE].apply. */
static int32_t apply_write(enip_connection_t *c, enip_tag_p t, uint8_t status, Bytes data) {
    (void)c;
    (void)status;
    (void)data;

    if(t->fragmented_elem) {
        size_t remaining = (size_t)t->size - (size_t)t->frag_offset;
        size_t chunk = (remaining < (size_t)t->frag_write_chunk) ? remaining : (size_t)t->frag_write_chunk;

        t->frag_offset += (uint32_t)chunk;

        if(t->frag_offset < (uint32_t)t->size) {
            t->frag_more = true;
        } else {
            t->frag_offset = 0;
        }

        return PLCTAG_STATUS_OK;
    }

    /* advance the cursor by however many elements the just-sent request
     * covered (§11.5). */
    if(t->elem_count > 1) { t->read_off += write_window_count(t); }

    return PLCTAG_STATUS_OK;
}

/* Dispatch one already-CPF-unwrapped CIP reply to its op's handler (3.f;
 * split out of what used to be a single 311-line if/else-if chain -- the
 * exact ladder shape that caused last session's OMRON hang, a missing arm
 * for one op). Caller holds t->api_mutex. */
extern int32_t apply_tag_reply(enip_connection_t *c, enip_tag_p t, uint8_t status, Bytes data) {
    t->frag_more = false;

    if(!op_traits[t->op].apply) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unexpected op %d while processing reply!", t->op);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    return op_traits[t->op].apply(c, t, status, data);
}


/* §16a.4 dialect selection from the classified PLC family. PCCC families are
 * distinguished by name at tag create (ENIP_TAG_KIND_PCCC), not here -- see
 * enip_pccc_dialect's comment in enip_dialect.h. */
extern const enip_dialect_t *enip_dialect_select(enip_plc_type_t plc_type) {
    if(plc_type == ENIP_PLC_OMRON_NJNX) { return &enip_omron_dialect; }
    return &enip_logix_dialect;
}


/* @tags/@udt listing dialect for the connection: PCCC families (PLC-5/SLC/
 * MicroLogix) enumerate via File 0 (enip_pccc_dialect, dialects/pccc/
 * pccc_client.c) rather than c->dialect's regular symbolic build_listing/
 * apply_listing -- independent
 * of c->dialect itself, which stays Logix-shaped for every connection (PCCC
 * data ops are selected per-tag, not per-connection; see enip_dialect.h). */
static const enip_dialect_t *listing_dialect_for(enip_connection_t *c) {
    return enip_plc_is_pccc(c->plc_type) ? &enip_pccc_dialect : c->dialect;
}

/* Allocate a CIP-payload-budget dest from c->arena and ask the dialect to
 * encode t's current request into it (the dialect owns no buffer; the caller
 * does). Only used by build_tag_request's single-op path (batch_count < 2),
 * so `budget` is always the full per-connection CIP payload -- there is no
 * other sub-request to share it with. build_batch_request (below) does its
 * own inline dialect->build() per sub-request with the true remaining space
 * (budget - cursor), so the 0x0A packer already size-greedy-budgets each
 * sub; this helper never needs to. Returns the used slice or null. */
static Bytes dialect_build(enip_connection_t *c, enip_tag_p t, size_t budget) {
    /* PCCC is selected per-tag (see enip_dialect.h); everything else uses the
     * connection's identity-selected dialect. */
    const enip_dialect_t *d = (t->kind == ENIP_TAG_KIND_PCCC) ? &enip_pccc_dialect : c->dialect;
    uint8_t *buf = arena_alloc(&c->arena, budget);
    if(!buf) { return bytes_null(); }
    return d->build(c, t, bytes_from_buf(buf, budget));
}

/* Wrap one CIP message for unconnected delivery: with a configured route it
 * goes inside an Unconnected_Send (0x52) carrying that route, mirroring AB;
 * with no route the bare message is the Unconnected Data Item's contents.
 * Returns the complete SendRRData frame, or bytes_null() on arena exhaustion
 * or a bad route string. Used both during bring-up (Identity) and, on the
 * unconnected path, for every tag request. */
static Bytes wrap_unconnected_frame(enip_connection_t *c, Bytes embedded) {
    Arena *a = &c->arena;

    Bytes cip_payload;

    if(c->path[0] == '\0') {
        cip_payload = embedded;
    } else {
        Bytes route = enip_cip_encode_route(a, c->path);
        if(bytes_is_null(route)) { return bytes_null(); }

        uint8_t route_words = (uint8_t)(route.len / 2);

        /* Unconnected_Send: service, path size words + Connection Manager path,
         * priority/ticks, timeout ticks, embedded message length. */
        Bytes us_hdr = bytes_pack(a, BYTES_LE, (uint8_t)CIP_UNCONN_SEND, (uint8_t)0x02, (uint8_t)0x20, (uint8_t)0x06,
                                   (uint8_t)0x24, (uint8_t)0x01, (uint8_t)0x0A, (uint8_t)0x05, (uint16_t)embedded.len);
        if(bytes_is_null(us_hdr)) { return bytes_null(); }

        /* optional pad byte to word-align after an odd-length message, then
         * route path size (words) + reserved byte. */
        Bytes route_hdr = (embedded.len & 1)
                              ? bytes_pack(a, BYTES_LE, (uint8_t)0x00, route_words, (uint8_t)0x00)
                              : bytes_pack(a, BYTES_LE, route_words, (uint8_t)0x00);
        if(bytes_is_null(route_hdr)) { return bytes_null(); }

        cip_payload = bytes_concat(a, us_hdr, embedded, route_hdr, route);
        if(bytes_is_null(cip_payload)) { return bytes_null(); }
    }

    Bytes cpf = cpf_wrap_unconnected(a, cip_payload);
    if(bytes_is_null(cpf)) { return bytes_null(); }

    return eip_frame(a, EIP_CMD_UNCONNECTED_SEND, c->session_handle, cpf);
}

/* The single funnel every tag request leaves through, single or batched: a
 * Connected Data Item over SendUnitData when the connection has a CIP
 * connection, an Unconnected_Send/SendRRData frame when it does not. The
 * per-request budget both callers size against is
 * max_cip_packet_size - c->cip_overhead, which already accounts for whichever
 * envelope this picks. */
static Bytes wrap_tag_frame(enip_connection_t *c, Bytes cip_req) {
    if(!c->is_connected_path) { return wrap_unconnected_frame(c, cip_req); }

    Bytes cpf = cpf_wrap_connected(&c->arena, c->cip_conn_id, ++c->conn_seq, cip_req);

    return eip_frame(&c->arena, EIP_CMD_CONNECTED_SEND, c->session_handle, cpf);
}

/* Build the tx frame for c->in_flight's current op and transition to
 * CONN_SENDING (or, on a build error, complete_tag + CONN_READY).
 * Caller holds t->api_mutex. */
static int32_t build_tag_request(enip_connection_t *c, enip_tag_p t) {
    arena_reset(&c->arena);

    Bytes req = bytes_null();

    if(op_traits[t->op].listing) {
        /* @tags/@udt enumeration is dialect-specific (Rockwell and OMRON use
         * different CIP classes/services -- see enip_dialect_t's
         * build_listing doc). The device identity/dialect is known by the
         * time pick_batch dispatches (queried during bring-up before
         * CONN_READY). Reject when the connection's dialect has no listing
         * support. */
        const enip_dialect_t *ld = listing_dialect_for(c);
        if(!ld->build_listing) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "@tags/@udt are not supported by dialect \"%s\"!", ld->name);
            complete_tag(c, t, (int8_t)PLCTAG_ERR_UNSUPPORTED);
            c->state = CONN_READY;
            return PLCTAG_STATUS_OK;
        }
        req = ld->build_listing(&c->arena, t);
    } else if(t->op != ENIP_OP_IDLE) {
        /* Every other live op (OPEN_PROBE/OPEN_PROBE_FRAG/OPEN_BULK/READ/WRITE)
         * is a plain dialect-encoded data request. */
        req = dialect_build(c, t, c->max_cip_packet_size - c->cip_overhead);
    }

    if(bytes_is_null(req)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to build request for op %d!", t->op);
        complete_tag(c, t, (int8_t)PLCTAG_ERR_UNSUPPORTED);
        c->state = CONN_READY;
        return PLCTAG_STATUS_OK;
    }

    Bytes frame = wrap_tag_frame(c, req);

    if(bytes_is_null(frame)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to wrap request for op %d!", t->op);
        complete_tag(c, t, (int8_t)PLCTAG_ERR_NO_MEM);
        c->state = CONN_READY;
        return PLCTAG_STATUS_OK;
    }

    c->tx_buf = frame.data;
    c->tx_len = frame.len;
    c->tx_off = 0;
    c->resume_state = CONN_READY;
    c->state = CONN_SENDING;

    return PLCTAG_STATUS_OK;
}

static int32_t build_request(enip_connection_t *c) {
    if(c->batch_count >= 2) { return build_batch_request(c); }

    enip_tag_p t = c->in_flight;
    if(mutex_try_lock(t->api_mutex) != PLCTAG_STATUS_OK) { return PLCTAG_STATUS_PENDING; }
    int32_t rc = build_tag_request(c, t);
    mutex_unlock(t->api_mutex);
    return rc;
}

static int32_t build_batch_request(enip_connection_t *c) {
    arena_reset(&c->arena);

    /* Try to lock all batch tags atomically; back off if any lock fails. */
    enip_tag_p t = c->batch_head;
    uint16_t locked = 0;
    while(t != NULL && locked < c->batch_count) {
        if(mutex_try_lock(t->api_mutex) != PLCTAG_STATUS_OK) {
            enip_tag_p u = c->batch_head;
            for(uint16_t i = 0; i < locked; i++) { mutex_unlock(u->api_mutex); u = u->batch_next; }
            return PLCTAG_STATUS_PENDING;
        }
        locked++;
        t = t->batch_next;
    }

    /* Pack the Multiple Service (0x0A) request in place: the dialect encodes
     * each sub-request directly into its slot in this single buffer, so the
     * whole batch costs one budget-sized allocation instead of one per tag
     * (max_batch can reach ~54, which would blow the arena). */
    size_t budget = c->max_cip_packet_size - c->cip_overhead;
    size_t header_size = ENIP_MS_REQ_FIXED + (size_t)2 * c->batch_count;
    uint8_t *ms_buf = (uint8_t *)arena_alloc(&c->arena, budget);
    bool build_ok = (ms_buf != NULL && header_size <= budget);
    size_t cursor = header_size;
    t = c->batch_head;
    for(uint16_t i = 0; build_ok && i < c->batch_count; i++) {
        /* offset[i] is relative to the Number_of_Services field at buf[6]. */
        uint16_t off = (uint16_t)(cursor - 6);
        bytes_pack_into(bytes_from_buf(ms_buf + 8 + (size_t)2 * i, 2), BYTES_LE, off);

        Bytes used = c->dialect->build(c, t, bytes_from_buf(ms_buf + cursor, budget - cursor));
        if(bytes_is_null(used)) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "build_batch_request: failed at sub-request %u!", (unsigned)i);
            build_ok = false;
            break;
        }
        cursor += used.len;
        t = t->batch_next;
    }

    Bytes frame = bytes_null();
    if(build_ok) {
        /* Multiple Service header: service + Message Router path + count. */
        bytes_pack_into(bytes_from_buf(ms_buf, ENIP_MS_REQ_FIXED), BYTES_LE, (uint8_t)CIP_MULTI_SVC, (uint8_t)0x02,
                       (uint8_t)0x20, (uint8_t)0x02, (uint8_t)0x24, (uint8_t)0x01, c->batch_count);

        frame = wrap_tag_frame(c, bytes_from_buf(ms_buf, cursor));
    }

    t = c->batch_head;
    for(uint16_t i = 0; i < c->batch_count; i++) { mutex_unlock(t->api_mutex); t = t->batch_next; }

    if(bytes_is_null(frame)) {
        complete_batch(c, (int8_t)PLCTAG_ERR_NO_MEM);
        c->state = CONN_READY;
        return PLCTAG_STATUS_OK;
    }

    c->tx_buf = frame.data;
    c->tx_len = frame.len;
    c->tx_off = 0;
    c->resume_state = CONN_READY;
    c->state = CONN_SENDING;
    c->batch_complete_idx = 0;

    return PLCTAG_STATUS_OK;
}

static void complete_batch(enip_connection_t *c, int8_t status) {
    enip_tag_p t = c->batch_head;
    while(t != NULL) {
        enip_tag_p next = t->batch_next;
        t->batch_next = NULL;
        if(mutex_try_lock(t->api_mutex) == PLCTAG_STATUS_OK) {
            complete_tag(c, t, status);
            mutex_unlock(t->api_mutex);
        } else {
            critical_block(c->sched_mutex) { rc_dec(t); }
        }
        t = next;
    }
    c->batch_head = NULL;
    c->batch_count = 0;
    c->batch_complete_idx = 0;
}

static void handle_batch_reply(enip_connection_t *c, eip_hdr_t *hdr, Bytes payload) {
    if(hdr->status != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "handle_batch_reply: SendUnitData status 0x%08" PRIx32 ".", hdr->status);
        complete_batch(c, (int8_t)PLCTAG_ERR_BAD_REPLY);
        c->state = CONN_READY;
        return;
    }

    uint16_t seq = 0;
    Bytes cip;
    if(!cpf_unwrap(payload, c->is_connected_path, NULL, &seq, &cip)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "handle_batch_reply: unable to unwrap CPF!");
        complete_batch(c, (int8_t)PLCTAG_ERR_BAD_REPLY);
        c->state = CONN_READY;
        return;
    }

    cip_reply_t outer;
    if(!enip_cip_parse_reply(cip, &outer)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "handle_batch_reply: unable to parse outer CIP reply!");
        complete_batch(c, (int8_t)PLCTAG_ERR_BAD_REPLY);
        c->state = CONN_READY;
        return;
    }

    if(outer.status != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "handle_batch_reply: outer CIP error 0x%02X ext 0x%04X.", outer.status,
               outer.ext_status);
        complete_batch(c, (int8_t)PLCTAG_ERR_REMOTE_ERR);
        c->state = CONN_READY;
        return;
    }

    Bytes *sub_replies = (Bytes *)arena_alloc(&c->arena, (size_t)c->batch_count * sizeof(Bytes));
    if(!sub_replies) {
        complete_batch(c, (int8_t)PLCTAG_ERR_NO_MEM);
        c->state = CONN_READY;
        return;
    }
    uint16_t count = 0;
    if(!enip_cip_parse_multi_service_reply(outer.data, &count, sub_replies, c->batch_count)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "handle_batch_reply: unable to parse MS reply!");
        complete_batch(c, (int8_t)PLCTAG_ERR_BAD_REPLY);
        c->state = CONN_READY;
        return;
    }

    if(count != c->batch_count) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "handle_batch_reply: MS reply count %u != batch count %u!", (unsigned)count,
               (unsigned)c->batch_count);
        complete_batch(c, (int8_t)PLCTAG_ERR_BAD_REPLY);
        c->state = CONN_READY;
        return;
    }

    /* Complete each tag from batch_complete_idx; batch_head tracks the next
     * unprocessed tag.  On mutex_try_lock failure, save the index and return
     * without changing state so CONN_WAITING retries with the same rx buffer. */
    uint16_t i = c->batch_complete_idx;
    enip_tag_p t = c->batch_head;

    while(i < c->batch_count) {
        if(mutex_try_lock(t->api_mutex) != PLCTAG_STATUS_OK) {
            c->batch_complete_idx = i;
            return;
        }

        cip_reply_t sub;
        int8_t status;
        if(!enip_cip_parse_reply(sub_replies[i], &sub)) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "handle_batch_reply: bad sub-reply %u!", (unsigned)i);
            status = (int8_t)PLCTAG_ERR_BAD_REPLY;
        } else if(sub.status != 0) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "handle_batch_reply: sub-reply %u CIP error 0x%02X ext 0x%04X.",
                   (unsigned)i, sub.status, sub.ext_status);
            status = (int8_t)PLCTAG_ERR_REMOTE_ERR;
        } else {
            status = (int8_t)apply_tag_reply(c, t, (uint8_t)0, sub.data);
        }

        /* Advance batch_head before complete_tag (which calls rc_dec). */
        enip_tag_p next = t->batch_next;
        t->batch_next = NULL;
        c->batch_head = next;

        complete_tag(c, t, status);
        mutex_unlock(t->api_mutex);

        i++;
        c->batch_complete_idx = i;
        t = next;
    }

    c->batch_count = 0;
    c->batch_complete_idx = 0;
    c->state = CONN_READY;
}

static void complete_tag(enip_connection_t *c, enip_tag_p t, int8_t status) {
    /* caller holds t->api_mutex */
    bool aborted = false;

    critical_block(c->sched_mutex) {
        aborted = atomic_get_bool(&t->abort_requested);
        atomic_set_bool(&t->abort_requested, false);

        c->in_flight = rc_dec(t);
    }

    if(aborted) {
        t->op = ENIP_OP_IDLE;
        t->status = (int8_t)PLCTAG_ERR_ABORT;
        tag_raise_event((plc_tag_p)t, PLCTAG_EVENT_ABORTED, (int8_t)PLCTAG_ERR_ABORT);
    } else {
        t->status = status;

        if(op_traits[t->op].creates) {
            tag_raise_event((plc_tag_p)t, PLCTAG_EVENT_CREATED, status);
        } else if(op_traits[t->op].reads) {
            /* Clear read_in_flight here (the global tickler used to do it via the
             * read_complete handshake, but ENIP tags skip that tickler). Without
             * this, generic_tickler's !read_in_flight guard blocks every later
             * auto-sync read. (@tags/@udt complete on their terminal op --
             * Rockwell's is UDT_FIELDS, OMRON's stays UDT_META the whole walk,
             * §5.3, since it has no separate metadata/field-definition split.) */
            t->read_complete = 1;
            t->read_in_flight = 0;
            tag_raise_event((plc_tag_p)t, PLCTAG_EVENT_READ_COMPLETED, status);
        } else if(op_traits[t->op].writes) {
            t->write_complete = 1;
            t->write_in_flight = 0;
            t->auto_sync_next_write = 0;
            tag_raise_event((plc_tag_p)t, PLCTAG_EVENT_WRITE_COMPLETED, status);
        }

        t->op = ENIP_OP_IDLE;
    }

    /* Auto-sync tags stay in the active list as IDLE timers (service_due_tags
     * fires their next read/write); one-shot tags were already unlinked and are
     * done. */
    if(!aborted && (t->auto_sync_read_ms > 0 || t->auto_sync_write_ms > 0)) {
        critical_block(c->sched_mutex) {
            if(!t->scheduled) {
                t->op_time = rearm_time(t, time_ms());
                sched_insert_sorted(c, t);
            }
        }
    }

    plc_tag_generic_handle_event_callbacks((plc_tag_p)t);
    plc_tag_generic_wake_tag((plc_tag_p)t);
}

static void reset_connection(enip_connection_t *c) {
    if(c->batch_head != NULL) { complete_batch(c, (int8_t)PLCTAG_ERR_BAD_CONNECTION); }

    if(c->in_flight != NULL) {
        enip_tag_p t = c->in_flight;

        if(mutex_try_lock(t->api_mutex) == PLCTAG_STATUS_OK) {
            complete_tag(c, t, (int8_t)PLCTAG_ERR_BAD_CONNECTION);
            mutex_unlock(t->api_mutex);
        } else {
            critical_block(c->sched_mutex) { c->in_flight = rc_dec(t); }
        }
    }

    if(c->sock) { socket_close(c->sock); }

    c->session_handle = 0;
    c->cip_conn_id = 0;
    c->resume_state = CONN_CONNECT;
    c->rx_buf = NULL;
    c->rx_len = 0;
    c->tx_buf = NULL;
    c->tx_len = c->tx_off = 0;
    c->rx_cap = ENIP_BOOTSTRAP_PACKET;
    c->connect_started = false;
    c->reconnect_at_ms = time_ms() + ENIP_RECONNECT_DELAY_MS;
    set_conn_status(c, (uint8_t)PLCTAG_CONN_STATUS_DOWN);
    set_conn_status(c, (uint8_t)PLCTAG_CONN_STATUS_ERR_WAIT);
    c->state = CONN_CONNECT;
}

/* Best-effort UnregisterSession ahead of a graceful idle teardown. The target
 * sends no reply to this command (common/eip.c's handle_unregister_session
 * mirrors this), so this is fire-and-forget: a single non-blocking write
 * attempt, no retry loop, no error handling beyond "did the arena/encode
 * succeed" -- the socket is about to be closed either way, so a short write
 * or a send that never reaches the peer changes nothing observable to this
 * connection. Skipped when there is no session to unregister (session_handle
 * == 0, e.g. RegisterSession never completed). */
static void send_unregister_session(enip_connection_t *c) {
    if(!c->sock || c->session_handle == 0) { return; }

    arena_reset(&c->arena);

    Bytes frame = eip_frame(&c->arena, EIP_CMD_UNREGISTER_SESSION, c->session_handle, bytes_null());
    if(bytes_is_null(frame)) { return; }

    socket_write(c->sock, frame.data, (int)frame.len, 0);
}

/* Graceful idle teardown: like reset_connection but driven by the inactivity
 * timer rather than an error, and it parks in CONN_IDLE (no reconnect timer) so
 * the session stays down until a tag is scheduled. Only called when there is no
 * in-flight or batched work, so nothing needs to be completed/aborted. */
static void idle_disconnect(enip_connection_t *c) {
    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "Inactivity timeout reached; disconnecting session.");

    send_unregister_session(c);

    if(c->sock) { socket_close(c->sock); }

    c->session_handle = 0;
    c->cip_conn_id = 0;
    c->resume_state = CONN_CONNECT;
    c->rx_buf = NULL;
    c->rx_len = 0;
    c->tx_buf = NULL;
    c->tx_len = c->tx_off = 0;
    c->rx_cap = ENIP_BOOTSTRAP_PACKET;
    c->connect_started = false;
    set_conn_status(c, (uint8_t)PLCTAG_CONN_STATUS_DOWN);
    set_conn_status(c, (uint8_t)PLCTAG_CONN_STATUS_IDLE_WAIT);
    c->state = CONN_IDLE;
}

/* ============================================================================
 * ForwardOpen (§14.3 wire layout)
 * ============================================================================ */

/* Network Connection Parameters flag bits (redundant_owner=0, connection_type=
 * Point-to-Point, priority=Low, size_type=Variable), same values already
 * proven against real hardware by the classic AB driver (protocols/ab/defs.h
 * AB_EIP_CONN_PARAM / AB_EIP_CONN_PARAM_EX): standard is a 16-bit field with
 * a 9-bit size (max 511) OR'd in; Large is a 32-bit field with the same
 * flag bits shifted up 16 and a 16-bit size (max 65535) OR'd in. */
#define ENIP_CONN_PARAM ((uint16_t)0x4200)
#define ENIP_CONN_PARAM_EX ((uint32_t)0x42000000)
#define ENIP_FO_STD_MAX_SIZE ((size_t)511)
#define ENIP_FO_LG_MAX_SIZE ((size_t)65535)
#define CIP_ERR_SERVICE_NOT_SUPPORTED ((uint8_t)0x08)

/* use_large selects Large Forward Open (0x5B, 32-bit connection-size fields,
 * up to 65535 bytes) vs standard (0x54, 16-bit fields, up to 511 bytes).
 * §16.4: the caller (step_open) always tries Large first; on_open_reply
 * falls back to standard once if the target rejects 0x5B with CIP 0x08.
 * Requested size is the dialect's requested_cip_size (0 -> ENIP_FO_CIP_SIZE
 * default) for a Large attempt, or always ENIP_FO_CIP_SIZE for a standard
 * one -- a standard attempt exists only as the fallback for a target that
 * can't do more than that anyway. */
static Bytes build_forward_open(enip_connection_t *c, bool use_large) {
    Arena *a = &c->arena;

    Bytes route = enip_cip_encode_route(a, c->path);
    if(bytes_is_null(route)) { return bytes_null(); }

    Bytes mr_suffix = bytes_pack(a, BYTES_LE, (uint8_t)0x20, (uint8_t)0x02, (uint8_t)0x24, (uint8_t)0x01);
    if(bytes_is_null(mr_suffix)) { return bytes_null(); }

    Bytes connection_path = bytes_concat(a, route, mr_suffix);
    if(bytes_is_null(connection_path)) { return bytes_null(); }

    uint8_t conn_path_words = (uint8_t)(connection_path.len / 2);

    uint8_t fo_service = use_large ? CIP_FWD_OPEN_LG : CIP_FWD_OPEN;

    Bytes fo_prefix = bytes_pack(a, BYTES_LE, fo_service, (uint8_t)0x02, (uint8_t)0x20, (uint8_t)0x06, (uint8_t)0x24,
                                  (uint8_t)0x01, (uint8_t)0x0A, (uint8_t)0x0E, (uint32_t)0, c->our_conn_id, c->conn_serial,
                                  ENIP_VENDOR_ID, ENIP_ORIGINATOR_SERIAL, (uint8_t)0x03, (uint8_t)0x00, (uint8_t)0x00,
                                  (uint8_t)0x00);
    if(bytes_is_null(fo_prefix)) { return bytes_null(); }

    size_t want = use_large ? (c->dialect->requested_cip_size ? c->dialect->requested_cip_size : ENIP_FO_CIP_SIZE)
                             : ENIP_FO_CIP_SIZE;
    size_t max_size = use_large ? ENIP_FO_LG_MAX_SIZE : ENIP_FO_STD_MAX_SIZE;
    if(want > max_size) { want = max_size; }
    c->requested_cip_size = want;

    Bytes fo_params;
    if(use_large) {
        uint32_t params = ENIP_CONN_PARAM_EX | (uint32_t)want;
        fo_params = bytes_pack(a, BYTES_LE, (uint32_t)1000000, params, (uint32_t)1000000, params);
    } else {
        uint16_t params = (uint16_t)(ENIP_CONN_PARAM | (uint16_t)want);
        fo_params = bytes_pack(a, BYTES_LE, (uint32_t)1000000, params, (uint32_t)1000000, params);
    }
    if(bytes_is_null(fo_params)) { return bytes_null(); }

    Bytes fo_suffix = bytes_pack(a, BYTES_LE, (uint8_t)0xA3, conn_path_words);
    if(bytes_is_null(fo_suffix)) { return bytes_null(); }

    Bytes cip_payload = bytes_concat(a, fo_prefix, fo_params, fo_suffix, connection_path);
    if(bytes_is_null(cip_payload)) { return bytes_null(); }

    Bytes cpf = cpf_wrap_unconnected(a, cip_payload);
    if(bytes_is_null(cpf)) { return bytes_null(); }

    return eip_frame(a, EIP_CMD_UNCONNECTED_SEND, c->session_handle, cpf);
}

static int32_t parse_forward_open_reply(enip_connection_t *c, Bytes cip_reply_bytes) {
    cip_reply_t reply;

    if(!enip_cip_parse_reply(cip_reply_bytes, &reply)) { return PLCTAG_ERR_BAD_REPLY; }

    if(reply.status != 0) {
        /* CIP_ERR_SERVICE_NOT_SUPPORTED (0x08) on a Large attempt just means
         * this target doesn't implement 0x5B -- on_open_reply (the only
         * caller) recognizes that case via c->used_large_fo and retries with
         * standard Forward Open instead of tearing the connection down. */
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ForwardOpen failed, CIP status 0x%02X (ext 0x%04X).", reply.status,
               reply.ext_status);
        return (reply.status == CIP_ERR_SERVICE_NOT_SUPPORTED) ? PLCTAG_ERR_UNSUPPORTED : PLCTAG_ERR_REMOTE_ERR;
    }

    uint32_t o_to_t_conn_id = 0, t_to_o_conn_id = 0;
    uint32_t o_to_t_api = 0, t_to_o_api = 0;
    uint8_t app_data_size = 0;

    Bytes rest = bytes_unpack(reply.data, BYTES_LE, &o_to_t_conn_id, &t_to_o_conn_id, BYTES_SKIP(8), &o_to_t_api, &t_to_o_api,
                               &app_data_size);
    if(bytes_is_null(rest)) { return PLCTAG_ERR_BAD_REPLY; }

    (void)o_to_t_api;
    (void)t_to_o_api;
    (void)app_data_size;

    c->cip_conn_id = o_to_t_conn_id;
    (void)t_to_o_conn_id;

    /* A status-0 reply means the target accepted the size step_open() asked
     * for (Variable connection size, not restated in the reply -- see
     * build_forward_open); c->requested_cip_size was stashed there. */
    c->max_cip_packet_size = c->requested_cip_size ? c->requested_cip_size : ENIP_FO_CIP_SIZE;
    c->rx_cap = c->max_cip_packet_size + ENIP_FRAMING_OVERHEAD;

    set_batch_limit(c);

    return PLCTAG_STATUS_OK;
}

/* ForwardClose for the active connection. Identifies the connection by the same
 * serial / originator vendor / originator serial used in its ForwardOpen, and
 * carries the same connection path. */
static Bytes build_forward_close(enip_connection_t *c) {
    Arena *a = &c->arena;

    Bytes route = enip_cip_encode_route(a, c->path);
    if(bytes_is_null(route)) { return bytes_null(); }

    Bytes mr_suffix = bytes_pack(a, BYTES_LE, (uint8_t)0x20, (uint8_t)0x02, (uint8_t)0x24, (uint8_t)0x01);
    if(bytes_is_null(mr_suffix)) { return bytes_null(); }

    Bytes connection_path = bytes_concat(a, route, mr_suffix);
    if(bytes_is_null(connection_path)) { return bytes_null(); }

    uint8_t conn_path_words = (uint8_t)(connection_path.len / 2);

    /* service, path size + Connection Manager path, priority/tick + timeout
     * ticks, connection serial, originator vendor id + serial, path size word
     * count, reserved pad. */
    Bytes fc_prefix = bytes_pack(a, BYTES_LE, CIP_FWD_CLOSE, (uint8_t)0x02, (uint8_t)0x20, (uint8_t)0x06, (uint8_t)0x24,
                                  (uint8_t)0x01, (uint8_t)0x0A, (uint8_t)0x0E, c->conn_serial, ENIP_VENDOR_ID,
                                  ENIP_ORIGINATOR_SERIAL, conn_path_words, (uint8_t)0x00);
    if(bytes_is_null(fc_prefix)) { return bytes_null(); }

    Bytes cip_payload = bytes_concat(a, fc_prefix, connection_path);
    if(bytes_is_null(cip_payload)) { return bytes_null(); }

    Bytes cpf = cpf_wrap_unconnected(a, cip_payload);
    if(bytes_is_null(cpf)) { return bytes_null(); }

    return eip_frame(a, EIP_CMD_UNCONNECTED_SEND, c->session_handle, cpf);
}

/* ============================================================================
 * Identity (CIP Identity object, class 0x01 / instance 1, Get_Attributes_All)
 * ============================================================================ */

/* Build a Get_Attributes_All on the Identity object, sent unconnected during
 * bring-up (before any ForwardOpen, and the only pre-READY traffic on the
 * unconnected path). */
static Bytes build_identity_request(enip_connection_t *c) {
    Arena *a = &c->arena;

    /* service 0x01 (Get_Attributes_All), path = Identity class 0x01 / instance 1 */
    Bytes embedded = bytes_pack(a, BYTES_LE, (uint8_t)0x01, (uint8_t)0x02, (uint8_t)0x20, (uint8_t)0x01, (uint8_t)0x24,
                                 (uint8_t)0x01);
    if(bytes_is_null(embedded)) { return bytes_null(); }

    return wrap_unconnected_frame(c, embedded);
}

static int32_t step_identity(enip_connection_t *c) {
    arena_reset(&c->arena);

    Bytes frame = build_identity_request(c);
    if(bytes_is_null(frame)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to build Identity request!");
        reset_connection(c);
        return PLCTAG_ERR_NO_MEM;
    }

    c->tx_buf = frame.data;
    c->tx_len = frame.len;
    c->tx_off = 0;
    c->resume_state = CONN_IDENTITY;
    c->state = CONN_SENDING;

    return PLCTAG_STATUS_OK;
}

static void on_identity_reply(enip_connection_t *c, eip_hdr_t *hdr, Bytes payload) {
    if(hdr->status != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Identity SendRRData failed, status 0x%08" PRIx32 ".", hdr->status);
        reset_connection(c);
        return;
    }

    uint16_t seq = 0;
    Bytes cip;

    if(!cpf_unwrap(payload, false, NULL, &seq, &cip)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to unwrap Identity CPF reply!");
        reset_connection(c);
        return;
    }

    cip_reply_t reply;

    /* Unconnected_Send is transparent on success: the reply is the embedded
     * Get_Attributes_All reply. A non-zero status (including a 0x52 routing
     * failure) is fatal to bring-up. */
    if(!enip_cip_parse_reply(cip, &reply) || reply.status != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Identity Get_Attributes_All rejected (CIP status 0x%02X, ext 0x%04X).",
               reply.status, reply.ext_status);
        reset_connection(c);
        return;
    }

    if(reply.data.len > 0xFFFF) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Identity reply implausibly large (%zu bytes)!", reply.data.len);
        reset_connection(c);
        return;
    }

    uint8_t *buf = mem_alloc((int)reply.data.len);
    if(!buf) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to allocate identity buffer!");
        reset_connection(c);
        return;
    }

    bytes_pack_into(bytes_from_buf(buf, reply.data.len), BYTES_LE, reply.data);

    if(c->identity_data) { mem_free(c->identity_data); }
    c->identity_data = buf;
    c->identity_len = (uint16_t)reply.data.len;

    /* Shared decode (common/identity.c); product name (SHORT_STRING) stays
     * in buf -- only the fixed-layout scalar fields are kept on c. */
    identity_t id = {0};
    (void)identity_decode(reply.data, &id, NULL);
    c->ident_vendor_id = id.vendor_id;
    c->ident_device_type = id.device_type;
    c->ident_product_code = id.product_code;
    c->ident_rev_major = id.revision_major;
    c->ident_rev_minor = id.revision_minor;
    c->ident_status = id.status;
    c->ident_serial = id.serial;

    c->plc_type = enip_classify_plc(c->ident_vendor_id, reply.data);

    /* model= (enip_session_create) overrides the device's own reported
     * identity -- e.g. talking through a bridge/adapter whose identity does
     * not reflect the end device, or forcing a family against a device_sim
     * endpoint. An unrecognized model is a warning, not a hard error: keep
     * the discovered classification rather than falling back to UNKNOWN. */
    if(c->model) {
        enip_plc_type_t override_type = enip_classify_plc_by_name(c->model);
        if(override_type != ENIP_PLC_UNKNOWN) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "Identity: model=\"%s\" overrides discovered family %s -> %s.", c->model,
                   plc_classify_name(c->plc_type), plc_classify_name(override_type));
            c->plc_type = override_type;
        } else {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
                   "Identity: model=\"%s\" does not match any known catalog prefix; keeping discovered family %s.", c->model,
                   plc_classify_name(c->plc_type));
        }
    }

    c->dialect = enip_dialect_select(c->plc_type);
    c->identity_valid = true;

    /*
     * Transport choice, now that the family is known: unconnected unless this
     * family wants a CIP connection, or the caller said otherwise.
     */
    set_path_mode(c, c->connected_pref_set ? c->connected_pref : enip_plc_prefers_connected(c->plc_type));

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0,
           "Identity: vendor=0x%04X device_type=0x%04X product=0x%04X rev=%u.%u serial=0x%08X.", c->ident_vendor_id,
           c->ident_device_type, c->ident_product_code, c->ident_rev_major, c->ident_rev_minor, c->ident_serial);
    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "Identity: PLC family = %s.", plc_classify_name(c->plc_type));
    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "Identity: messaging = %s.", c->is_connected_path ? "connected" : "unconnected");

    if(c->is_connected_path) {
        c->state = CONN_OPEN;
        return;
    }

    /*
     * No ForwardOpen on the unconnected path, so nothing else will size the
     * receive buffer: max_cip_packet_size keeps its ENIP_FO_CIP_SIZE default,
     * which is what an unconnected request may carry.
     */
    c->rx_cap = c->max_cip_packet_size + ENIP_FRAMING_OVERHEAD;
    set_batch_limit(c);
    c->state = CONN_READY;
}

/* ============================================================================
 * Main loop
 * ============================================================================ */

static THREAD_FUNC(io_thread_func) {
    enip_connection_t *c = (enip_connection_t *)arg;

    while(!atomic_get_bool(&c->terminate)) {
        int64_t now = time_ms();
        int mask = SOCK_EVENT_DEFAULT_MASK;
        int64_t wait_ms = ENIP_IDLE_WAIT_MS;

        /* ENIP owns its own ticklering (all ENIP tags set skip_tickler): service
         * @connection/@identity tags and auto-sync timers in every state. Data
         * network ops are still dispatched by pick_batch in CONN_READY. */
        service_due_tags(c, now);

        switch(c->state) {
            case CONN_CONNECT: {
                int32_t rc = step_connect(c);

                if(c->state != CONN_CONNECT) {
                    wait_ms = 0;
                } else if(rc == PLCTAG_STATUS_PENDING && c->connect_started) {
                    mask |= SOCK_EVENT_CONNECT;
                    wait_ms = 100;
                } else {
                    int64_t remaining = c->reconnect_at_ms - now;
                    wait_ms = (remaining > 0) ? remaining : 100;
                }

                break;
            }

            case CONN_REGISTER:
                step_register(c);
                wait_ms = 0;
                break;

            case CONN_IDENTITY:
                step_identity(c);
                wait_ms = 0;
                break;

            case CONN_OPEN:
                step_open(c);
                wait_ms = 0;
                break;

            case CONN_READY: {
                set_conn_status(c, (uint8_t)PLCTAG_CONN_STATUS_UP);

                if(c->in_flight == NULL && c->batch_head == NULL) {
                    pick_batch(c, now, &wait_ms);
                }

                bool have_work = (c->in_flight != NULL || c->batch_head != NULL);

                pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0,
                       "CONN_READY now=%" PRId64 " have_work=%d batch=%u sched_head=%p in_flight=%p wait_ms=%" PRId64,
                       now, (int)have_work, (unsigned)c->batch_count, (void *)c->sched_head, (void *)c->in_flight, wait_ms);

                if(have_work) {
                    c->last_activity_ms = now;

                    if(build_request(c) == PLCTAG_STATUS_PENDING) {
                        wait_ms = 1;
                    } else {
                        wait_ms = 0;
                    }
                } else if(now - c->last_activity_ms >= c->inactivity_timeout_ms) {
                    /* gracefully ForwardClose the live connection before going
                     * idle; if there is none, just drop straight to idle. */
                    set_conn_status(c, (uint8_t)PLCTAG_CONN_STATUS_DISCONNECTING);
                    if(c->session_handle != 0 && c->cip_conn_id != 0) {
                        c->state = CONN_CLOSE;
                    } else {
                        idle_disconnect(c);
                    }
                    wait_ms = 0;
                }

                break;
            }

            case CONN_CLOSE:
                step_close(c);
                wait_ms = 0;
                break;

            case CONN_IDLE: {
                bool have_work = false;
                critical_block(c->sched_mutex) { have_work = (c->sched_head != NULL); }

                if(have_work) {
                    set_conn_status(c, (uint8_t)PLCTAG_CONN_STATUS_CONNECTING);
                    c->reconnect_at_ms = 0;
                    c->last_activity_ms = time_ms();
                    c->state = CONN_CONNECT;
                    wait_ms = 0;
                } else {
                    wait_ms = ENIP_IDLE_WAIT_MS;
                }

                break;
            }

            case CONN_SENDING:
                step_sending(c);
                mask |= SOCK_EVENT_CAN_WRITE;
                wait_ms = 0;
                break;

            case CONN_WAITING: {
                int32_t rc = step_waiting(c);

                mask |= SOCK_EVENT_CAN_READ;
                wait_ms = (rc == PLCTAG_STATUS_PENDING) ? ENIP_IDLE_WAIT_MS : 0;

                break;
            }

            case CONN_CLOSING:
            default: atomic_set_bool(&c->terminate, true); break;
        }

        /* Clamp the wait so @connection/@identity tags (incl. a status change
         * pushed during this iteration, or an @identity retry) are serviced on
         * time regardless of connection state. */
        if(wait_ms != 0) { wait_ms = next_special_wait(c, time_ms(), wait_ms); }

        if(wait_ms != 0 && !atomic_get_bool(&c->terminate)) {
            int timeout = (wait_ms > 1000) ? 1000 : (int)wait_ms;

            if(c->sock) { socket_wait_event(c->sock, mask, timeout); }
            else { sleep_ms(timeout); }
        }
    }

    THREAD_RETURN(0);
}
