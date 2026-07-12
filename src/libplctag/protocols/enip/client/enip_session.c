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
 * by enip_cip_read / enip_cpf_wrap_* / enip_eip_* (each an arena allocation),
 * rather than being assembled byte-by-byte into a pre-sized tx_buf.  The
 * arena is reset before building and the resulting Bytes is used in place
 * as tx_buf/tx_len.
 */

#include <inttypes.h>
#include <string.h>

#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/protocols/enip/client/enip_cip.h>
#include <libplctag/protocols/enip/client/enip_cpf.h>
#include <libplctag/protocols/enip/client/enip_dialect.h>
#include <libplctag/protocols/enip/client/enip_eip.h>
#include <libplctag/protocols/enip/client/enip_session.h>
#include <libplctag/protocols/enip/client/enip_tag.h>
#include <libplctag/protocols/enip/client/enip_type.h>
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

/* EIP header (24) + connected CPF overhead (22). */
#define ENIP_FRAMING_OVERHEAD (ENIP_EIP_HEADER_SIZE + ENIP_CPF_CONNECTED_OVERHEAD)

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

/* Connection-status event ring (§ @connection tag). Single producer (IO
 * thread), multiple consumers (each @connection tag keeps its own read idx). */
#define ENIP_CONN_STATUS_RING_SIZE ((int32_t)16)
#define ENIP_CONN_STATUS_RING_MASK (ENIP_CONN_STATUS_RING_SIZE - 1)
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

static const char *identity_plc_type_name(enip_plc_type_t plc_type) {
    switch(plc_type) {
        case ENIP_PLC_PLC5: return "PLC-5";
        case ENIP_PLC_SLC: return "SLC-500";
        case ENIP_PLC_MLGX: return "MicroLogix";
        case ENIP_PLC_LGX: return "ControlLogix-class";
        case ENIP_PLC_MICRO800: return "Micro800";
        case ENIP_PLC_OMRON_NJNX: return "OMRON NJ/NX";
        case ENIP_PLC_UNKNOWN: default: return "unknown";
    }
}

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

/* §4 + §14.3 connection structure. */
struct enip_connection_t {
    enip_connection_t *next; /* registry singly-linked list, under s_registry_mutex */

    char *gateway;
    char *path;
    char *model; /* optional; overrides identity-based classification (§ model=) */
    int tcp_port;
    bool is_connected_path;

    /* Large Forward Open (0x5B) try/fallback (ENIP-SESSION-DESIGN.md §16.4):
     * try_large_fo is what the *next* step_open() attempt should use, reset
     * true at connection creation; on_open_reply clears it (remembered for
     * this connection's later reconnects) the first time a Large attempt is
     * rejected with CIP 0x08 (Service Not Supported). used_large_fo and
     * requested_cip_size record what the in-flight attempt actually asked
     * for, so on_open_reply/parse_forward_open_reply don't have to re-derive
     * it from the dialect. */
    bool try_large_fo;
    bool used_large_fo;
    size_t requested_cip_size;

    sock_p sock;
    thread_p thread;
    bool connect_started;

    uint32_t session_handle;
    uint32_t cip_conn_id;
    uint16_t conn_seq;

    uint32_t our_conn_id;
    uint16_t conn_serial;

    mutex_p sched_mutex;
    enip_tag_p sched_head, sched_tail;
    enip_tag_p in_flight;
    enip_tag_p sched_cursor; /* service_special_tags walk position; sched_mutex */

    enip_tag_p batch_head;
    uint16_t batch_count;
    uint16_t batch_complete_idx;

    Arena arena;

    size_t max_cip_packet_size;
    size_t rx_cap;
    uint16_t max_batch;

    int64_t reconnect_at_ms;

    /* idle disconnect (§ test_idle_disconnect); plain scalars, best-effort
     * cross-thread access -- the IO thread writes conn_status/last_activity_ms,
     * the API thread reads status and reads/writes inactivity_timeout_ms. */
    int64_t inactivity_timeout_ms;
    int64_t last_activity_ms;
    uint8_t conn_status; /* PLCTAG_CONN_STATUS_* */

    /* conn-status event ring; IO thread writes via set_conn_status, @connection
     * tags drain via enip_session_next_conn_status. */
    uint8_t conn_status_ring[ENIP_CONN_STATUS_RING_SIZE];
    atomic_int32_t conn_status_ring_write_idx;

    /* CIP Identity object, queried once during bring-up (§ @identity). Raw
     * Get_Attributes_All payload cached for @identity tags; parsed fields kept
     * for device detection. identity_data is mem_alloc'd, freed in destructor. */
    bool identity_valid;
    uint8_t *identity_data;
    uint16_t identity_len;
    uint16_t ident_vendor_id, ident_device_type, ident_product_code;
    uint8_t ident_rev_major, ident_rev_minor;
    uint16_t ident_status;
    uint32_t ident_serial;
    enip_plc_type_t plc_type; /* auto-detected PLC family; drives feature selection */

    /* Manufacturer dialect (§16a.4): build/apply function pointers + the two
     * sizing numbers. Defaults to &enip_logix_dialect at creation; reselected
     * from the Identity reply at the end of bring-up. Never NULL. */
    const enip_dialect_t *dialect;

    uint8_t state;
    uint8_t resume_state;

    uint8_t *tx_buf;
    size_t tx_len, tx_off;

    uint8_t *rx_buf;
    size_t rx_len;

    atomic_bool terminate;
};

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
static Bytes enip_logix_build(enip_connection_t *c, enip_tag_p t, Bytes dest);
static int32_t build_tag_request(enip_connection_t *c, enip_tag_p t);
static int32_t build_batch_request(enip_connection_t *c);
static void complete_tag(enip_connection_t *c, enip_tag_p t, int8_t status);
static void complete_batch(enip_connection_t *c, int8_t status);
static void handle_batch_reply(enip_connection_t *c, enip_eip_hdr_t *hdr, Bytes payload);
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

static void on_register_reply(enip_connection_t *c, enip_eip_hdr_t *hdr);
static void on_open_reply(enip_connection_t *c, enip_eip_hdr_t *hdr, Bytes payload);
static void on_close_reply(enip_connection_t *c, enip_eip_hdr_t *hdr, Bytes payload);
static void handle_tag_reply(enip_connection_t *c, enip_eip_hdr_t *hdr, Bytes payload);
static int32_t apply_tag_reply(enip_connection_t *c, enip_tag_p t, uint8_t status, Bytes data);
static Bytes build_listing_request(Arena *a, enip_tag_p t);
static int32_t apply_listing_reply(enip_tag_p t, uint8_t cip_status, Bytes data, bool *more);

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
static Bytes build_forward_close(enip_connection_t *c);
static int32_t step_identity(enip_connection_t *c);
static Bytes build_identity_request(enip_connection_t *c);
static void on_identity_reply(enip_connection_t *c, enip_eip_hdr_t *hdr, Bytes payload);

/* ============================================================================
 * Registry / lifecycle
 * ============================================================================ */

static bool conn_key_matches(enip_connection_t *c, const char *gateway, const char *path, int port) {
    return c->tcp_port == port && str_cmp(c->gateway, gateway) == 0 && str_cmp(c->path, path) == 0;
}

static enip_connection_t *create_connection(const char *gateway, const char *path, const char *model, int port) {
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
    c->is_connected_path = true;
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

enip_connection_t *enip_session_create(attr attribs, bool *is_new_out) {
    const char *gateway = attr_get_str(attribs, "gateway", NULL);
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
    int port = attr_get_int(attribs, "port", ENIP_DEFAULT_PORT);

    if(is_new_out) { *is_new_out = false; }

    if(!gateway || str_length(gateway) == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Missing required \"gateway\" attribute.");
        return NULL;
    }

    enip_connection_t *result = NULL;

    critical_block(s_registry_mutex) {
        enip_connection_t *c = s_conns;

        while(c != NULL) {
            if(conn_key_matches(c, gateway, path, port)) {
                result = rc_inc(c);
                break;
            }
            c = c->next;
        }

        if(!result) {
            result = create_connection(gateway, path, model, port);
            if(result && is_new_out) { *is_new_out = true; }
        }
    }

    return result;
}

int32_t enip_session_module_init(void) {
    if(mutex_create(&s_registry_mutex) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to create registry mutex!");
        return PLCTAG_ERR_CREATE;
    }

    s_conns = NULL;

    return PLCTAG_STATUS_OK;
}

void enip_session_module_teardown(void) {
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

static bool is_batch_eligible(enip_tag_p t) {
    if(t->kind != ENIP_TAG_KIND_DATA) { return false; }
    if(atomic_get_bool(&t->abort_requested)) { return false; }
    if(!t->ready) { return false; }
    /* §16a.6: a fragmented element needs its own exclusive Read/WriteFrag
     * continuation loop; it can never be folded into a Multiple Service
     * Packet alongside other tags. */
    if(t->fragmented_elem) { return false; }
    if(t->op == ENIP_OP_READ) { return t->elem_count <= t->window_elems; }
    if(t->op == ENIP_OP_WRITE) { return t->elem_count <= t->write_window_elems; }
    return false;
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

        size_t req_budget = c->max_cip_packet_size - CIP_CONNECTED_ITEM_OVERHEAD - ENIP_MS_REQ_FIXED;
        size_t resp_budget = c->max_cip_packet_size - CIP_CONNECTED_ITEM_OVERHEAD - ENIP_MS_RESP_FIXED;

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

int32_t enip_session_schedule(enip_connection_t *c, enip_tag_p t, uint8_t op, int64_t op_time) {
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

int32_t enip_session_unschedule(enip_connection_t *c, enip_tag_p t) {
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

void enip_session_tag_detach(enip_connection_t *c, enip_tag_p t) {
    critical_block(c->sched_mutex) {
        if(t->scheduled) { sched_unlink(c, t); }
    }
}

size_t enip_session_max_cip(enip_connection_t *c) { return c->max_cip_packet_size; }

int enip_session_get_status(enip_connection_t *c) { return (int)c->conn_status; }

/* Cached CIP Identity payload (raw Get_Attributes_All response). Returns false
 * until the bring-up identity query has completed. */
bool enip_session_get_identity(enip_connection_t *c, uint8_t **data_out, uint16_t *len_out) {
    if(!c || !c->identity_valid) { return false; }
    if(data_out) { *data_out = c->identity_data; }
    if(len_out) { *len_out = c->identity_len; }
    return true;
}

int enip_session_get_inactivity_timeout(enip_connection_t *c) { return (int)c->inactivity_timeout_ms; }

/* Clamp to [ENIP_MIN, ENIP_MAX]; returns PLCTAG_ERR_OUT_OF_BOUNDS (and still
 * stores the clamped value) if the request was out of range. */
int enip_session_set_inactivity_timeout(enip_connection_t *c, int new_value) {
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

int32_t enip_session_conn_status_idx(enip_connection_t *c) { return atomic_get_int32(&c->conn_status_ring_write_idx); }

bool enip_session_next_conn_status(enip_connection_t *c, int32_t *read_idx, int32_t *status_out) {
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

    Bytes frame = enip_eip_register_session(&c->arena);
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

    if(c->rx_len < ENIP_EIP_HEADER_SIZE) {
        needed = ENIP_EIP_HEADER_SIZE - c->rx_len;
    } else {
        uint16_t plen = (uint16_t)((uint16_t)c->rx_buf[2] | (uint16_t)((uint16_t)c->rx_buf[3] << 8));
        size_t total = ENIP_EIP_HEADER_SIZE + plen;

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

        if(c->rx_len < ENIP_EIP_HEADER_SIZE) { return PLCTAG_STATUS_PENDING; }
    }

    uint16_t plen = (uint16_t)((uint16_t)c->rx_buf[2] | (uint16_t)((uint16_t)c->rx_buf[3] << 8));
    size_t total = ENIP_EIP_HEADER_SIZE + plen;

    if(c->rx_len < total) { return PLCTAG_STATUS_PENDING; }

    enip_eip_hdr_t hdr;
    Bytes payload;

    if(!enip_eip_decode(bytes_from_buf(c->rx_buf, total), &hdr, &payload)) {
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

static void on_register_reply(enip_connection_t *c, enip_eip_hdr_t *hdr) {
    if(hdr->status != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "RegisterSession failed, status 0x%08" PRIx32 ".", hdr->status);
        reset_connection(c);
        return;
    }

    c->session_handle = hdr->session_handle;

    /* Query identity once per device; an idle reconnect already knows it. */
    c->state = c->identity_valid ? CONN_OPEN : CONN_IDENTITY;
}

static void on_open_reply(enip_connection_t *c, enip_eip_hdr_t *hdr, Bytes payload) {
    if(hdr->status != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ForwardOpen SendRRData failed, status 0x%08" PRIx32 ".", hdr->status);
        reset_connection(c);
        return;
    }

    uint16_t seq = 0;
    Bytes cip;

    if(!enip_cpf_unwrap(payload, false, &seq, &cip)) {
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
static void on_close_reply(enip_connection_t *c, enip_eip_hdr_t *hdr, Bytes payload) {
    if(hdr->status != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ForwardClose SendRRData failed, status 0x%08" PRIx32 ".", hdr->status);
    } else {
        uint16_t seq = 0;
        Bytes cip;

        if(enip_cpf_unwrap(payload, false, &seq, &cip)) {
            cip_reply_t reply;

            if(enip_cip_parse_reply(cip, &reply) && reply.status != 0) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ForwardClose rejected, CIP status 0x%02X (ext 0x%04X).", reply.status,
                       reply.ext_status);
            }
        }
    }

    idle_disconnect(c);
}

static void handle_tag_reply(enip_connection_t *c, enip_eip_hdr_t *hdr, Bytes payload) {
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

        if(!enip_cpf_unwrap(payload, true, &seq, &cip)) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to unwrap connected CPF reply!");
            status = (int8_t)PLCTAG_ERR_BAD_REPLY;
        } else if(t->kind == ENIP_TAG_KIND_LISTING || t->kind == ENIP_TAG_KIND_UDT) {
            /* @tags/@udt: a partial-transfer status (0x06) is continuation, not
             * an error; apply_listing_reply accumulates and sets more_windows. */
            cip_reply_t reply;
            if(!enip_cip_parse_reply(cip, &reply)) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to parse CIP reply!");
                status = (int8_t)PLCTAG_ERR_BAD_REPLY;
            } else if(reply.status != 0 && reply.status != CIP_STATUS_FRAG) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "CIP error 0x%02X (ext 0x%04X).", reply.status,
                       reply.ext_status);
                status = (int8_t)PLCTAG_ERR_REMOTE_ERR;
            } else {
                status = (int8_t)apply_listing_reply(t, reply.status, reply.data, &more_windows);
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
static uint32_t write_window_count(enip_tag_p t) {
    uint32_t n = t->elem_count - t->read_off;
    if(n > t->write_window_elems) { n = t->write_window_elems; }
    return n;
}

static int32_t apply_tag_reply(enip_connection_t *c, enip_tag_p t, uint8_t status, Bytes data) {
    t->frag_more = false;

    if(t->op == ENIP_OP_OPEN_PROBE) {
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

        memcpy(t->type_header, data.data, header_len);
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

            uint8_t *buf = mem_alloc((int)chunk);
            if(!buf) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to allocate tag data buffer!");
                return PLCTAG_ERR_NO_MEM;
            }

            if(t->data) { mem_free(t->data); }

            t->data = buf;
            t->size = (int32_t)chunk;
            memcpy(t->data, data.data + header_len, chunk);

            t->frag_offset = chunk;
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

        memcpy(t->data, data.data + header_len, elem_size);

        /* §11.3: window = clamp((cap - overhead - header_len) / elem_size, 1, elem_count) */
        uint32_t window = (uint32_t)((c->max_cip_packet_size - CIP_CONNECTED_ITEM_OVERHEAD - CIP_READ_REPLY_OVERHEAD
                                       - (size_t)header_len)
                                      / elem_size);
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

        size_t write_overhead = CIP_CONNECTED_ITEM_OVERHEAD + CIP_WRITE_REQUEST_OVERHEAD + write_path_len + (size_t)header_len;

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
    } else if(t->op == ENIP_OP_OPEN_PROBE_FRAG) {
        /* §16a.6: continuation of a fragmented single-element OPEN_PROBE.
         * Every ReadFrag reply re-sends the type header (already known from
         * the first fragment); total size is not known upfront, so grow
         * t->data as fragments arrive, matching the classic AB driver's
         * check_read_status_connected(). */
        if(data.len < (size_t)t->type_header_len) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Reply data shorter than type header!");
            return PLCTAG_ERR_BAD_REPLY;
        }

        uint32_t chunk = (uint32_t)(data.len - (size_t)t->type_header_len);
        size_t new_size = (size_t)t->size + (size_t)chunk;

        uint8_t *buf = mem_realloc(t->data, (int)new_size);
        if(!buf) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to grow tag data buffer!");
            return PLCTAG_ERR_NO_MEM;
        }

        memcpy(buf + t->size, data.data + t->type_header_len, chunk);
        t->data = buf;
        t->size = (int32_t)new_size;
        t->frag_offset += chunk;

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
            size_t fixed = (size_t)1 /* service */ + 1 /* path_size_words */ + (size_t)t->path.len
                         + (size_t)t->type_header_len + 2 /* elem_count */ + 4 /* byte_offset */;
            size_t cap = c->max_cip_packet_size - CIP_CONNECTED_ITEM_OVERHEAD;
            size_t usable = (cap > fixed) ? (cap - fixed) : 0;
            size_t frag_chunk = (usable / t->frag_align) * t->frag_align;
            t->frag_write_chunk = (frag_chunk > 0) ? (uint32_t)frag_chunk : (uint32_t)t->frag_align;
        }

        return PLCTAG_STATUS_OK;
    } else if(t->op == ENIP_OP_OPEN_BULK) {
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

        memcpy(t->data + dest_off, data.data + t->type_header_len, copy_bytes);

        t->read_off += (uint32_t)returned;

        if(t->read_off >= t->elem_count) { t->ready = 1; }

        return PLCTAG_STATUS_OK;
    } else if(t->op == ENIP_OP_READ) {
        if(data.len < (size_t)t->type_header_len) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Reply data shorter than type header!");
            return PLCTAG_ERR_BAD_REPLY;
        }

        if(t->fragmented_elem) {
            /* §16a.6: same growth strategy as OPEN_PROBE_FRAG, cursor reset
             * to 0 by enip_tag_read() at the start of this read. */
            uint32_t chunk = (uint32_t)(data.len - (size_t)t->type_header_len);
            size_t dest_off = (size_t)t->frag_offset;
            size_t needed = dest_off + (size_t)chunk;

            if(needed > (size_t)t->size) {
                uint8_t *buf = mem_realloc(t->data, (int)needed);
                if(!buf) {
                    pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to grow tag data buffer!");
                    return PLCTAG_ERR_NO_MEM;
                }
                t->data = buf;
                t->size = (int32_t)needed;
                t->elem_size = (uint32_t)needed;
            }

            memcpy(t->data + dest_off, data.data + t->type_header_len, chunk);
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

            memcpy(t->data, data.data + t->type_header_len, copy_len);

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

        memcpy(t->data + dest_off, data.data + t->type_header_len, copy_bytes);

        t->read_off += (uint32_t)returned;

        return PLCTAG_STATUS_OK;
    } else if(t->op == ENIP_OP_WRITE) {
        /* CIP write replies carry no data. */
        if(t->fragmented_elem) {
            /* §16a.6: advance by the same chunk size build's WriteFrag just
             * sent (recomputed here, not passed in -- deterministic from
             * fixed inputs, mirrors write_window_count() for the array
             * case). Cursor reset to 0 by enip_tag_write() at the start of
             * this write. */
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

    pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unexpected op %d while processing reply!", t->op);

    return PLCTAG_ERR_UNSUPPORTED;
}

/* Logix/Micro800 apply (enip_dialect_t.apply): parse one CIP reply, treat any
 * non-zero CIP status other than CIP_STATUS_FRAG (§16a.6: fragmentation
 * continuation, not an error) as a remote error, copy into t->data via
 * apply_tag_reply, and set *more when another read/write window (or
 * fragment) is due. Caller holds api_mutex. */
static int32_t enip_logix_apply(enip_connection_t *c, enip_tag_p t, Bytes cip_reply, bool *more) {
    *more = false;

    cip_reply_t reply;
    if(!enip_cip_parse_reply(cip_reply, &reply)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to parse CIP reply!");
        return PLCTAG_ERR_BAD_REPLY;
    }

    if(reply.status != 0 && reply.status != CIP_STATUS_FRAG) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "CIP error 0x%02X (ext 0x%04X).", reply.status, reply.ext_status);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    int32_t rc = apply_tag_reply(c, t, reply.status, reply.data);

    if(rc == PLCTAG_STATUS_OK) {
        if(t->frag_more) {
            *more = true;
        } else if((t->op == ENIP_OP_OPEN_BULK || t->op == ENIP_OP_READ || t->op == ENIP_OP_WRITE)
                  && t->read_off < t->elem_count) {
            *more = true;
        }
    }

    return rc;
}

/* requested_cip_size 4002: the Large Forward Open size ControlLogix/
 * CompactLogix/GuardLogix (5580/5380/5370) and Micro800 "E" (2080-L50E/L70E)
 * grant, per vendor documentation (unverified against real hardware in this
 * tree). §16.4: the connection always tries Large Forward Open (0x5B) first
 * with this size and falls back to a plain 504-byte standard Forward Open
 * (0x54) if the target rejects 0x5B with CIP 0x08 -- so a target that can't
 * actually do 4002 (older Micro800, or anything else) just costs one extra
 * round trip on first connect, not a wrong value. */
const enip_dialect_t enip_logix_dialect = {
    .name = "logix",
    .requested_cip_size = 4002,
    .max_batch_cap = 0, /* no cap: Multiple Service Packet (0x0A) supported */
    .build = enip_logix_build,
    .apply = enip_logix_apply,
};

/* OMRON-SPECIFIC-DESIGN.md §6: same build/apply as Logix (§1 -- identical path
 * encoding, Read/Write Tag services, and CIP Common Format reply framing);
 * only the requested Forward Open size differs (§2.2 family default). */
/* requested_cip_size 1892: "modern Sysmac standard" per vendor documentation
 * (unverified against real hardware in this tree) -- covers the NX1/NJ
 * mainline (NX102, NX1P2, NJ501, NJ301). The flagship NX7-series (NX701) is
 * documented as negotiating up to 9600, and legacy CJ1W-EIP21/early-NJ
 * bridges as low as 1444; this dialect doesn't distinguish those from the
 * mainline (no sub-family classification -- common/plc_classify.c only
 * resolves to ENIP_PLC_OMRON_NJNX, not a specific catalog line), so 1892 is
 * the safer common denominator: Forward Open is accept/reject, not a
 * negotiate-down, so a target that can't honor 1892 rejects the connection
 * outright rather than silently granting less. A target that can't do Large
 * Forward Open at all still recovers via the 0x08 fallback (§16.4); a
 * target that supports 0x5B but rejects this specific size for some other
 * reason is a hard failure, same as an oversized standard-FO request always
 * was. */
const enip_dialect_t enip_omron_dialect = {
    .name = "omron-njnx",
    .requested_cip_size = 1892,
    .max_batch_cap = 0,
    .build = enip_logix_build,
    .apply = enip_logix_apply,
};

/* §16a.4 dialect selection from the classified PLC family. PCCC families are
 * distinguished by name at tag create (ENIP_TAG_KIND_PCCC), not here -- see
 * enip_pccc_dialect's comment in enip_dialect.h. */
const enip_dialect_t *enip_dialect_select(enip_plc_type_t plc_type) {
    if(plc_type == ENIP_PLC_OMRON_NJNX) { return &enip_omron_dialect; }
    return &enip_logix_dialect;
}

/* Logix/Micro800 symbolic build (enip_dialect_t.build): encode the CIP
 * sub-request for t's current data op into the caller-owned `dest` region.
 * Returns the used prefix of dest, or bytes_null() if it does not fit / on
 * error. Uses c->arena as scratch (the existing CIP encoders allocate there).
 * Caller holds t->api_mutex. */
static Bytes enip_logix_build(enip_connection_t *c, enip_tag_p t, Bytes dest) {
    Arena *a = &c->arena;
    Bytes req = bytes_null();

    switch(t->op) {
        /* §16a.6: always ReadFrag (offset 0), not plain ReadTag -- for an
         * element that fits, the reply is identical (status 0, all the
         * data); for one that doesn't, the device signals CIP_STATUS_FRAG
         * and apply_tag_reply switches to ENIP_OP_OPEN_PROBE_FRAG instead of
         * this needing a client-side "will it fit" pre-check. */
        case ENIP_OP_OPEN_PROBE: req = enip_cip_read_frag(a, t->path, 1, 0); break;

        case ENIP_OP_OPEN_PROBE_FRAG: req = enip_cip_read_frag(a, t->path, 1, t->frag_offset); break;

        case ENIP_OP_OPEN_BULK: {
            uint32_t n = t->elem_count - t->read_off;
            if(n > t->window_elems) { n = t->window_elems; }
            Bytes path = enip_cip_encode_path_at(a, t->path, t->read_off);
            req = enip_cip_read(a, path, (uint16_t)n);
            break;
        }

        case ENIP_OP_READ:
            if(t->fragmented_elem) {
                req = enip_cip_read_frag(a, t->path, 1, t->frag_offset);
            } else if(t->elem_count <= 1) {
                req = enip_cip_read(a, t->path, (uint16_t)t->elem_count);
            } else {
                uint32_t n = t->elem_count - t->read_off;
                if(n > t->window_elems) { n = t->window_elems; }
                Bytes path = enip_cip_encode_path_at(a, t->path, t->read_off);
                req = enip_cip_read(a, path, (uint16_t)n);
            }
            break;

        case ENIP_OP_WRITE: {
            Bytes type_header = bytes_from_buf(t->type_header, t->type_header_len);
            if(t->fragmented_elem) {
                /* §16a.6: fixed-size aligned chunk, computed once at
                 * fragmentation-detection time (see apply_tag_reply's
                 * OPEN_PROBE_FRAG completion) so build and apply always
                 * agree on the chunk boundary without exchanging state. */
                size_t remaining = (size_t)t->size - (size_t)t->frag_offset;
                size_t chunk = (remaining < (size_t)t->frag_write_chunk) ? remaining : (size_t)t->frag_write_chunk;
                Bytes data = bytes_from_buf(t->data + t->frag_offset, chunk);
                req = enip_cip_write_frag(a, t->path, type_header, 1, t->frag_offset, data);
            } else if(t->elem_count <= 1) {
                Bytes data = bytes_from_buf(t->data, (size_t)t->size);
                req = enip_cip_write(a, t->path, type_header, (uint16_t)t->elem_count, data);
            } else {
                uint32_t n = write_window_count(t);
                Bytes path = enip_cip_encode_path_at(a, t->path, t->read_off);
                Bytes data = bytes_from_buf(t->data + (size_t)t->read_off * (size_t)t->elem_size, (size_t)n * (size_t)t->elem_size);
                req = enip_cip_write(a, path, type_header, (uint16_t)n, data);
            }
            break;
        }

        default: return bytes_null();
    }

    if(bytes_is_null(req) || req.len > dest.len) { return bytes_null(); }

    memcpy(dest.data, req.data, req.len);
    return bytes_from_buf(dest.data, req.len);
}

/* ============================================================================
 * PCCC dialect: PLC-5 / SLC500 / MicroLogix (Execute-PCCC, CIP service 0x4B).
 * The CIP request rides the same connected CPF+EIP wrap as the symbolic dialect;
 * build/apply just speak PCCC. Reuses ab/pccc.c's pure address encoders.
 * ========================================================================= */

#define PCCC_EXECUTE_SVC ((uint8_t)0x4B)
#define PCCC_TYPED_CMD ((uint8_t)0x0F)
#define PCCC_PLC5_READ_FNC ((uint8_t)0x01)
#define PCCC_PLC5_WRITE_FNC ((uint8_t)0x00)
#define PCCC_SLC_READ_FNC ((uint8_t)0xA2)
#define PCCC_SLC_WRITE_FNC ((uint8_t)0xAA)
#define PCCC_PLC5_RMW_FNC ((uint8_t)0x26)
#define PCCC_SLC_RMW_FNC ((uint8_t)0xAB)
#define PCCC_VENDOR_ID ((uint16_t)0xF33D)     /* matches ab/defs.h AB_EIP_VENDOR_ID */
#define PCCC_VENDOR_SN ((uint32_t)0x21504345) /* matches ab/defs.h AB_EIP_VENDOR_SN */
/* In a parsed reply.data: requestor id (7) + PCCC cmd(1)+sts(1)+tns(2) = 11. */
#define PCCC_REPLY_HDR ((size_t)11)
#define PCCC_REPLY_STS_OFF ((size_t)8)

/* Write the CIP/PCCC header + requestor id shared by every Execute-PCCC
 * request (13 bytes), then the PCCC command fields up to and including FNC.
 * Returns the new position. */
static size_t pccc_write_header(uint8_t *p, uint16_t tns, uint8_t fnc) {
    size_t pos = 0;
    p[pos++] = PCCC_EXECUTE_SVC;
    p[pos++] = 0x02; /* path size in 16-bit words */
    p[pos++] = 0x20; p[pos++] = 0x67; p[pos++] = 0x24; p[pos++] = 0x01; /* PCCC object 0x67 inst 1 */
    p[pos++] = 0x07; /* requestor id size = vendor_id(2) + serial(4) + this byte */
    p[pos++] = (uint8_t)(PCCC_VENDOR_ID & 0xFFu);
    p[pos++] = (uint8_t)(PCCC_VENDOR_ID >> 8);
    p[pos++] = (uint8_t)(PCCC_VENDOR_SN & 0xFFu);
    p[pos++] = (uint8_t)((PCCC_VENDOR_SN >> 8) & 0xFFu);
    p[pos++] = (uint8_t)((PCCC_VENDOR_SN >> 16) & 0xFFu);
    p[pos++] = (uint8_t)((PCCC_VENDOR_SN >> 24) & 0xFFu);
    p[pos++] = PCCC_TYPED_CMD;
    p[pos++] = 0x00;
    p[pos++] = (uint8_t)(tns & 0xFFu);
    p[pos++] = (uint8_t)(tns >> 8);
    p[pos++] = fnc;
    return pos;
}

/* PLC-5 masked bit write (Execute-PCCC function 0x26, "Protected Typed Logical
 * Read/Write with mask"): AND-mask/OR-mask pair, one byte per element byte.
 * The remote never learns the current value of the word -- only the target
 * bit's byte gets a non-0xFF/0x00 mask entry -- so unrelated bits are never
 * clobbered. Mirrors ab/pccc.c:plc5_tag_write_bit_start; server side is
 * dialects/pccc/pccc.c:handle_plc5_rmw. */
static Bytes enip_pccc_build_plc5_bit_write(enip_connection_t *c, enip_tag_p t, Bytes dest) {
    uint8_t addr_buf[32];
    pccc_addr_t addr = t->pccc_addr;
    Bytes encoded = enip_pccc_encode_plc5_address(&addr, bytes_from_buf(addr_buf, sizeof(addr_buf)));
    if(bytes_is_null(encoded) || encoded.len == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to encode PCCC logical address!");
        return bytes_null();
    }

    size_t need = 13 + 5 + encoded.len + 2u * (size_t)t->elem_size;
    if(need > dest.len) { return bytes_null(); }

    uint8_t *p = dest.data;
    size_t pos = pccc_write_header(p, (uint16_t)(c->conn_seq + 1), PCCC_PLC5_RMW_FNC);

    memcpy(p + pos, encoded.data, encoded.len);
    pos += encoded.len;

    size_t byte_idx = (size_t)(t->bit / 8);
    uint8_t bit_mask = (uint8_t)(1u << (t->bit % 8));
    bool bit_set = (t->data[byte_idx] & bit_mask) != 0;

    for(uint32_t i = 0; i < t->elem_size; i++) {
        p[pos++] = ((size_t)i == byte_idx) ? (bit_set ? (uint8_t)0xFF : (uint8_t)~bit_mask) : (uint8_t)0xFF;
    }
    for(uint32_t i = 0; i < t->elem_size; i++) {
        p[pos++] = ((size_t)i == byte_idx) ? (bit_set ? bit_mask : (uint8_t)0x00) : (uint8_t)0x00;
    }

    return bytes_from_buf(dest.data, pos);
}

/* SLC/MicroLogix masked bit write (Execute-PCCC function 0xAB, "SLC Range
 * Write with mask"): a single 16-bit mask/set pair -- the mask is transmitted
 * as 2 bytes regardless of element size, so this only applies to 2-byte (B/N)
 * data files. A 32-bit L-file bit is not maskable this way (matches AB: real
 * hardware and run_enip_tests.sh's MicroLogix L-bit-write test both expect
 * failure). Mirrors ab/pccc.c:slc_tag_write_bit_start; server side is
 * dialects/pccc/pccc.c:handle_slc_rmw. */
static Bytes enip_pccc_build_slc_bit_write(enip_connection_t *c, enip_tag_p t, Bytes dest) {
    if(t->elem_size != 2 || t->size != 2) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id,
               "SLC/MicroLogix masked bit write requires a 2-byte element (mask is 16 bits); got %u bytes.",
               (unsigned)t->elem_size);
        return bytes_null();
    }

    uint8_t addr_buf[32];
    pccc_addr_t addr = t->pccc_addr;
    Bytes encoded = enip_pccc_encode_slc_address(&addr, bytes_from_buf(addr_buf, sizeof(addr_buf)));
    if(bytes_is_null(encoded) || encoded.len == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to encode PCCC logical address!");
        return bytes_null();
    }

    size_t need = 13 + 5 + 1 + encoded.len + 2 + 2;
    if(need > dest.len) { return bytes_null(); }

    uint8_t *p = dest.data;
    size_t pos = pccc_write_header(p, (uint16_t)(c->conn_seq + 1), PCCC_SLC_RMW_FNC);

    p[pos++] = (uint8_t)t->size; /* transfer size in bytes, fixed at 2 */

    memcpy(p + pos, encoded.data, encoded.len);
    pos += encoded.len;

    uint8_t mask[2] = {0, 0};
    mask[t->bit / 8] = (uint8_t)(1u << (t->bit % 8));
    memcpy(p + pos, mask, 2);
    pos += 2;

    /* set bytes: only the masked bit is honored remotely, so t->data's other
     * bits (whatever they happen to hold locally) are harmless. */
    memcpy(p + pos, t->data, 2);
    pos += 2;

    return bytes_from_buf(dest.data, pos);
}

static Bytes enip_pccc_build(enip_connection_t *c, enip_tag_p t, Bytes dest) {
    if(t->op != ENIP_OP_READ && t->op != ENIP_OP_WRITE) { return bytes_null(); }

    /* A single-bit write must go out as a masked RMW, never a plain word
     * write: t->data only ever holds the bit this tag cares about (it is
     * never populated by a real read of the sibling bits), so overwriting the
     * whole word would clobber them on real hardware. Bit reads need no
     * special handling -- they fall through to the plain word read below and
     * the generic plc_tag_get_bit() extracts the bit locally. */
    if(t->is_bit && t->op == ENIP_OP_WRITE) {
        return t->pccc_plc5 ? enip_pccc_build_plc5_bit_write(c, t, dest) : enip_pccc_build_slc_bit_write(c, t, dest);
    }

    bool is_write = (t->op == ENIP_OP_WRITE);
    size_t data_len = is_write ? (size_t)t->size : 0;

    /* SLC/MicroLogix carry the transfer size in a single byte. */
    if(!t->pccc_plc5 && (size_t)t->size > 0xFFu) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "PCCC SLC transfer size %d exceeds 255 bytes!", (int)t->size);
        return bytes_null();
    }

    /* Encode the logical address with the family encoder (copy: it may adjust). */
    uint8_t addr_buf[32];
    pccc_addr_t addr = t->pccc_addr;
    Bytes encoded = t->pccc_plc5 ? enip_pccc_encode_plc5_address(&addr, bytes_from_buf(addr_buf, sizeof(addr_buf)))
                                 : enip_pccc_encode_slc_address(&addr, bytes_from_buf(addr_buf, sizeof(addr_buf)));
    if(bytes_is_null(encoded) || encoded.len == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to encode PCCC logical address!");
        return bytes_null();
    }
    size_t addr_len = encoded.len;

    /* CIP/PCCC header(13) + PCCC cmd fixed(plc5:9, slc:6) + addr + [plc5 read size byte] + write data. */
    size_t need = 13 + (t->pccc_plc5 ? 9u : 6u) + addr_len + (t->pccc_plc5 && !is_write ? 1u : 0u) + data_len;
    if(need > dest.len) { return bytes_null(); }

    uint16_t tns = (uint16_t)(c->conn_seq + 1);
    uint8_t *p = dest.data;
    size_t pos = 0;

    /* CIP Execute-PCCC header + requestor id. */
    p[pos++] = PCCC_EXECUTE_SVC;
    p[pos++] = 0x02; /* path size in 16-bit words */
    p[pos++] = 0x20; p[pos++] = 0x67; p[pos++] = 0x24; p[pos++] = 0x01; /* PCCC object 0x67 inst 1 */
    p[pos++] = 0x07; /* requestor id size = vendor_id(2) + serial(4) + this byte */
    p[pos++] = (uint8_t)(PCCC_VENDOR_ID & 0xFFu);
    p[pos++] = (uint8_t)(PCCC_VENDOR_ID >> 8);
    p[pos++] = (uint8_t)(PCCC_VENDOR_SN & 0xFFu);
    p[pos++] = (uint8_t)((PCCC_VENDOR_SN >> 8) & 0xFFu);
    p[pos++] = (uint8_t)((PCCC_VENDOR_SN >> 16) & 0xFFu);
    p[pos++] = (uint8_t)((PCCC_VENDOR_SN >> 24) & 0xFFu);

    /* PCCC command: CMD, STS=0, TNS, FNC, ... */
    p[pos++] = PCCC_TYPED_CMD;
    p[pos++] = 0x00;
    p[pos++] = (uint8_t)(tns & 0xFFu);
    p[pos++] = (uint8_t)(tns >> 8);

    if(t->pccc_plc5) {
        p[pos++] = is_write ? PCCC_PLC5_WRITE_FNC : PCCC_PLC5_READ_FNC;
        p[pos++] = 0x00; p[pos++] = 0x00;                   /* offset = 0 */
        uint16_t words = (uint16_t)((size_t)t->size / 2u);  /* transfer size in words */
        p[pos++] = (uint8_t)(words & 0xFFu);
        p[pos++] = (uint8_t)(words >> 8);
        memcpy(p + pos, addr_buf, (size_t)addr_len);
        pos += (size_t)addr_len;
        if(!is_write) { p[pos++] = (uint8_t)t->size; } /* PLC-5 read appends total byte size */
    } else {
        p[pos++] = is_write ? PCCC_SLC_WRITE_FNC : PCCC_SLC_READ_FNC;
        p[pos++] = (uint8_t)t->size; /* transfer size in bytes */
        memcpy(p + pos, addr_buf, (size_t)addr_len);
        pos += (size_t)addr_len;
    }

    if(is_write) {
        memcpy(p + pos, t->data, data_len);
        pos += data_len;
    }

    return bytes_from_buf(dest.data, pos);
}

static int32_t enip_pccc_apply(enip_connection_t *c, enip_tag_p t, Bytes cip_reply, bool *more) {
    (void)c;
    *more = false; /* PCCC has no fragmentation: one round trip per op. */

    cip_reply_t reply;
    if(!enip_cip_parse_reply(cip_reply, &reply)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to parse PCCC CIP reply!");
        return PLCTAG_ERR_BAD_REPLY;
    }

    if(reply.status != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "PCCC CIP error 0x%02X (ext 0x%04X).", reply.status, reply.ext_status);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    if(reply.data.len < PCCC_REPLY_HDR) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "PCCC reply too short (%zu bytes)!", reply.data.len);
        return PLCTAG_ERR_BAD_REPLY;
    }

    /* PCCC-level STS. 0xF0 carries an extended code in the next byte; treating
     * any non-zero as a remote error is enough for the data path. */
    uint8_t pccc_sts = reply.data.data[PCCC_REPLY_STS_OFF];
    if(pccc_sts != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "PCCC status error 0x%02X.", pccc_sts);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    if(t->op == ENIP_OP_WRITE) { return PLCTAG_STATUS_OK; }

    size_t avail = reply.data.len - PCCC_REPLY_HDR;
    size_t n = (avail < (size_t)t->size) ? avail : (size_t)t->size;
    memcpy(t->data, reply.data.data + PCCC_REPLY_HDR, n);
    return PLCTAG_STATUS_OK;
}

const enip_dialect_t enip_pccc_dialect = {
    .name = "pccc",
    .requested_cip_size = 0,
    .max_batch_cap = 1, /* single in-flight: no Multiple Service (0x0A) packing */
    .build = enip_pccc_build,
    .apply = enip_pccc_apply,
};

/* Allocate a CIP-payload-budget dest from c->arena and ask the dialect to
 * encode t's current request into it (the dialect owns no buffer; the caller
 * does). `budget` bounds one sub-request: the full CIP payload for a single op,
 * or the space the 0x0A packer has left. Returns the used slice or null.
 * ponytail: batch passes the full budget per sub (count-limited by pick_batch);
 * a size-greedy 0x0A packer can pass the true remaining space later. */
static Bytes dialect_build(enip_connection_t *c, enip_tag_p t, size_t budget) {
    /* PCCC is selected per-tag (see enip_dialect.h); everything else uses the
     * connection's identity-selected dialect. */
    const enip_dialect_t *d = (t->kind == ENIP_TAG_KIND_PCCC) ? &enip_pccc_dialect : c->dialect;
    uint8_t *buf = arena_alloc(&c->arena, budget);
    if(!buf) { return bytes_null(); }
    return d->build(c, t, bytes_from_buf(buf, budget));
}

/* Build the tx frame for c->in_flight's current op and transition to
 * CONN_SENDING (or, on a build error, complete_tag + CONN_READY).
 * Caller holds t->api_mutex. */
static int32_t build_tag_request(enip_connection_t *c, enip_tag_p t) {
    arena_reset(&c->arena);

    Bytes req = bytes_null();

    switch(t->op) {
        case ENIP_OP_OPEN_PROBE:
        case ENIP_OP_OPEN_PROBE_FRAG:
        case ENIP_OP_OPEN_BULK:
        case ENIP_OP_READ:
        case ENIP_OP_WRITE:
            req = dialect_build(c, t, c->max_cip_packet_size - CIP_CONNECTED_ITEM_OVERHEAD);
            break;

        case ENIP_OP_LIST:
        case ENIP_OP_UDT_META:
        case ENIP_OP_UDT_FIELDS:
            /* @tags/@udt are ControlLogix-class only (CIP class 0x6B/0x6C); the
             * device identity is known by the time pick_batch dispatches (queried
             * during bring-up before CONN_READY). Reject on anything else. */
            if(c->plc_type != ENIP_PLC_LGX) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "@tags/@udt are only supported on ControlLogix-class devices!");
                complete_tag(c, t, (int8_t)PLCTAG_ERR_UNSUPPORTED);
                c->state = CONN_READY;
                return PLCTAG_STATUS_OK;
            }
            req = build_listing_request(&c->arena, t);
            break;

        default: break;
    }

    if(bytes_is_null(req)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, t->tag_id, "Unable to build request for op %d!", t->op);
        complete_tag(c, t, (int8_t)PLCTAG_ERR_UNSUPPORTED);
        c->state = CONN_READY;
        return PLCTAG_STATUS_OK;
    }

    Bytes cpf = enip_cpf_wrap_connected(&c->arena, c->cip_conn_id, ++c->conn_seq, req);
    Bytes frame = enip_eip_send_unit_data(&c->arena, c->session_handle, cpf);

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

/* @tags/@udt request for t's current op. read_off is the byte cursor into the
 * accumulated buffer; list_next_id is the symbol instance id (@tags) or template
 * id (@udt); list_total is the @udt field-definition byte target. */
static Bytes build_listing_request(Arena *a, enip_tag_p t) {
    switch(t->op) {
        case ENIP_OP_LIST: return enip_cip_list_tags(a, t->path, (uint16_t)t->list_next_id);

        case ENIP_OP_UDT_META: return enip_cip_udt_meta(a, (uint16_t)t->list_next_id);

        case ENIP_OP_UDT_FIELDS: {
            uint32_t remaining = (t->list_total > t->read_off) ? (t->list_total - t->read_off) : 0;
            return enip_cip_udt_fields(a, (uint16_t)t->list_next_id, t->read_off, (uint16_t)remaining);
        }

        default: return bytes_null();
    }
}

/* Accumulate one @tags/@udt reply into t->data and advance the continuation
 * cursor. Sets *more when another request is needed (FRAG status, or the UDT
 * metadata->fields transition). Caller holds t->api_mutex. */
static int32_t apply_listing_reply(enip_tag_p t, uint8_t cip_status, Bytes data, bool *more) {
    *more = false;

    if(t->op == ENIP_OP_LIST) {
        if(data.len > 0) {
            size_t need = (size_t)t->read_off + data.len;
            uint8_t *buf = mem_realloc(t->data, (int)need);
            if(!buf) { return PLCTAG_ERR_NO_MEM; }
            t->data = buf;
            memcpy(t->data + t->read_off, data.data, data.len);
            t->read_off = (uint32_t)need;
            t->size = (int32_t)need;

            /* Walk this packet's entries to find the highest instance id. Each
             * entry is a 22-byte fixed prefix (instance_id u32, symbol_type u16,
             * element_length u16, array_dims 3xu32, string_len u16) + name. */
            size_t off = 0;
            while(off + 22 <= data.len) {
                uint32_t inst = 0;
                uint16_t name_len = 0;
                bytes_unpack(bytes_from_buf(data.data + off, data.len - off), BYTES_LE, &inst, BYTES_SKIP(16), &name_len);
                t->list_next_id = inst + 1;
                off += (size_t)22 + name_len;
            }
        }

        if(cip_status == CIP_STATUS_FRAG) { *more = true; }

        return PLCTAG_STATUS_OK;
    }

    if(t->op == ENIP_OP_UDT_META) {
        /* The Get_Attribute_List reply packs count(2) then per-attribute
         * {id(2), status(2), value}. Mirror the AB driver's fixed offsets to pull
         * the four values and build a 14-byte synthetic header (udt id, field
         * definition words, instance size, member count, handle). */
        if(data.len < 30) { return PLCTAG_ERR_BAD_REPLY; }

        uint32_t desc_words = 0, inst_size = 0;
        uint16_t num_members = 0, handle = 0;
        bytes_unpack(bytes_from_buf(data.data + 6, 4), BYTES_LE, &desc_words);
        bytes_unpack(bytes_from_buf(data.data + 14, 4), BYTES_LE, &inst_size);
        bytes_unpack(bytes_from_buf(data.data + 22, 2), BYTES_LE, &num_members);
        bytes_unpack(bytes_from_buf(data.data + 28, 2), BYTES_LE, &handle);

        uint8_t *buf = mem_realloc(t->data, 14);
        if(!buf) { return PLCTAG_ERR_NO_MEM; }
        t->data = buf;
        Bytes hdr = bytes_from_buf(t->data, 14);
        bytes_pack_into(hdr, BYTES_LE, (uint16_t)t->list_next_id, (uint32_t)desc_words, (uint32_t)inst_size,
                        (uint16_t)num_members, (uint16_t)handle);
        t->size = 14;

        /* field-definition byte target (per the template docs), rounded up to 4. */
        uint32_t total = (4 * desc_words) - 23;
        t->list_total = (total + 3) & ~(uint32_t)3;
        t->read_off = 0;

        /* transition to reading the field definition bytes. */
        t->op = ENIP_OP_UDT_FIELDS;
        *more = true;

        return PLCTAG_STATUS_OK;
    }

    if(t->op == ENIP_OP_UDT_FIELDS) {
        if(data.len > 0) {
            size_t need = (size_t)14 + t->read_off + data.len;
            uint8_t *buf = mem_realloc(t->data, (int)need);
            if(!buf) { return PLCTAG_ERR_NO_MEM; }
            t->data = buf;
            memcpy(t->data + 14 + t->read_off, data.data, data.len);
            t->read_off += (uint32_t)data.len;
            t->size = (int32_t)need;
        }

        if(cip_status == CIP_STATUS_FRAG) { *more = true; }

        return PLCTAG_STATUS_OK;
    }

    return PLCTAG_ERR_UNSUPPORTED;
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
    size_t budget = c->max_cip_packet_size - CIP_CONNECTED_ITEM_OVERHEAD;
    size_t header_size = ENIP_MS_REQ_FIXED + (size_t)2 * c->batch_count;
    uint8_t *ms_buf = (uint8_t *)arena_alloc(&c->arena, budget);
    bool build_ok = (ms_buf != NULL && header_size <= budget);
    size_t cursor = header_size;
    t = c->batch_head;
    for(uint16_t i = 0; build_ok && i < c->batch_count; i++) {
        /* offset[i] is relative to the Number_of_Services field at buf[6]. */
        uint16_t off = (uint16_t)(cursor - 6);
        ms_buf[8 + (size_t)2 * i]     = (uint8_t)(off & 0xFFu);
        ms_buf[8 + (size_t)2 * i + 1] = (uint8_t)(off >> 8);

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
        ms_buf[0] = CIP_MULTI_SVC;
        ms_buf[1] = 0x02; ms_buf[2] = 0x20; ms_buf[3] = 0x02; ms_buf[4] = 0x24; ms_buf[5] = 0x01;
        ms_buf[6] = (uint8_t)(c->batch_count & 0xFFu);
        ms_buf[7] = (uint8_t)(c->batch_count >> 8);

        Bytes ms = bytes_from_buf(ms_buf, cursor);
        Bytes cpf = enip_cpf_wrap_connected(&c->arena, c->cip_conn_id, ++c->conn_seq, ms);
        frame = enip_eip_send_unit_data(&c->arena, c->session_handle, cpf);
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

static void handle_batch_reply(enip_connection_t *c, enip_eip_hdr_t *hdr, Bytes payload) {
    if(hdr->status != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "handle_batch_reply: SendUnitData status 0x%08" PRIx32 ".", hdr->status);
        complete_batch(c, (int8_t)PLCTAG_ERR_BAD_REPLY);
        c->state = CONN_READY;
        return;
    }

    uint16_t seq = 0;
    Bytes cip;
    if(!enip_cpf_unwrap(payload, true, &seq, &cip)) {
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

        if(t->op == ENIP_OP_OPEN_PROBE || t->op == ENIP_OP_OPEN_PROBE_FRAG || t->op == ENIP_OP_OPEN_BULK) {
            tag_raise_event((plc_tag_p)t, PLCTAG_EVENT_CREATED, status);
        } else if(t->op == ENIP_OP_READ || t->op == ENIP_OP_LIST || t->op == ENIP_OP_UDT_FIELDS) {
            /* Clear read_in_flight here (the global tickler used to do it via the
             * read_complete handshake, but ENIP tags skip that tickler). Without
             * this, generic_tickler's !read_in_flight guard blocks every later
             * auto-sync read. (@tags/@udt complete on their terminal op.) */
            t->read_complete = 1;
            t->read_in_flight = 0;
            tag_raise_event((plc_tag_p)t, PLCTAG_EVENT_READ_COMPLETED, status);
        } else if(t->op == ENIP_OP_WRITE) {
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

/* Graceful idle teardown: like reset_connection but driven by the inactivity
 * timer rather than an error, and it parks in CONN_IDLE (no reconnect timer) so
 * the session stays down until a tag is scheduled. Only called when there is no
 * in-flight or batched work, so nothing needs to be completed/aborted. */
static void idle_disconnect(enip_connection_t *c) {
    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "Inactivity timeout reached; disconnecting session.");

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

    Bytes cpf = enip_cpf_wrap_unconnected(a, cip_payload);
    if(bytes_is_null(cpf)) { return bytes_null(); }

    return enip_eip_send_rr_data(a, c->session_handle, cpf);
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

    size_t cip_payload = c->max_cip_packet_size - CIP_CONNECTED_ITEM_OVERHEAD;
    size_t sub_space = (cip_payload > ENIP_MS_REQ_FIXED) ? (cip_payload - ENIP_MS_REQ_FIXED) : 0;
    size_t computed_max = sub_space / ENIP_MS_MIN_SUB_REQ_SIZE;
    if(computed_max > ENIP_BATCH_ARRAY_SIZE) { computed_max = ENIP_BATCH_ARRAY_SIZE; }
    c->max_batch = (uint16_t)computed_max;

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

    Bytes cpf = enip_cpf_wrap_unconnected(a, cip_payload);
    if(bytes_is_null(cpf)) { return bytes_null(); }

    return enip_eip_send_rr_data(a, c->session_handle, cpf);
}

/* ============================================================================
 * Identity (CIP Identity object, class 0x01 / instance 1, Get_Attributes_All)
 * ============================================================================ */

/* Build a Get_Attributes_All on the Identity object. With no routing path the
 * bare CIP request goes in the Unconnected Data Item; with a path it is wrapped
 * in an Unconnected_Send (0x52) carrying the route, mirroring AB. */
static Bytes build_identity_request(enip_connection_t *c) {
    Arena *a = &c->arena;

    /* service 0x01 (Get_Attributes_All), path = Identity class 0x01 / instance 1 */
    Bytes embedded = bytes_pack(a, BYTES_LE, (uint8_t)0x01, (uint8_t)0x02, (uint8_t)0x20, (uint8_t)0x01, (uint8_t)0x24,
                                 (uint8_t)0x01);
    if(bytes_is_null(embedded)) { return bytes_null(); }

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

    Bytes cpf = enip_cpf_wrap_unconnected(a, cip_payload);
    if(bytes_is_null(cpf)) { return bytes_null(); }

    return enip_eip_send_rr_data(a, c->session_handle, cpf);
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

static void on_identity_reply(enip_connection_t *c, enip_eip_hdr_t *hdr, Bytes payload) {
    if(hdr->status != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Identity SendRRData failed, status 0x%08" PRIx32 ".", hdr->status);
        reset_connection(c);
        return;
    }

    uint16_t seq = 0;
    Bytes cip;

    if(!enip_cpf_unwrap(payload, false, &seq, &cip)) {
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

    mem_copy(buf, reply.data.data, (int)reply.data.len);

    if(c->identity_data) { mem_free(c->identity_data); }
    c->identity_data = buf;
    c->identity_len = (uint16_t)reply.data.len;

    /* parse the fixed-layout prefix; product name (SHORT_STRING) stays in buf. */
    (void)bytes_unpack(reply.data, BYTES_LE, &c->ident_vendor_id, &c->ident_device_type, &c->ident_product_code,
                       &c->ident_rev_major, &c->ident_rev_minor, &c->ident_status, &c->ident_serial);

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
                   identity_plc_type_name(c->plc_type), identity_plc_type_name(override_type));
            c->plc_type = override_type;
        } else {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
                   "Identity: model=\"%s\" does not match any known catalog prefix; keeping discovered family %s.", c->model,
                   identity_plc_type_name(c->plc_type));
        }
    }

    c->dialect = enip_dialect_select(c->plc_type);
    c->identity_valid = true;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0,
           "Identity: vendor=0x%04X device_type=0x%04X product=0x%04X rev=%u.%u serial=0x%08X.", c->ident_vendor_id,
           c->ident_device_type, c->ident_product_code, c->ident_rev_major, c->ident_rev_minor, c->ident_serial);
    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "Identity: PLC family = %s.", identity_plc_type_name(c->plc_type));

    c->state = CONN_OPEN;
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
