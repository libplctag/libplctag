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
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include <stdbool.h>
#include <stddef.h>

#include <libplctag/lib/libplctag.h>
#include "platform.h"
#include "utils/debug.h"
#include "utils/rc.h"
#include "utils/vector.h"
#include "endpoint.h"

/* Initial capacity / growth increment for the endpoints vector — matches
 * ab/session.c's vector_create(25, 5) for its own global connection registry
 * (the precedent this registry mirrors); endpoint counts are expected to be
 * far smaller than session counts, but there's no reason to pick differently. */
#define ENDPOINTS_INITIAL_CAPACITY 25
#define ENDPOINTS_GROWTH_INCREMENT 5

/* ============================================================================
 * Registry entry — one per running endpoint (bind_addr,port). rc_alloc'd:
 * rc_inc/rc_dec (utils/rc.h) own the refcount instead of a hand-rolled int,
 * and rc_alloc's cleanup callback (endpoint_entry_cleanup, below) replaces
 * the manual "if refcount hits zero, tear down" branch.
 *
 * Held in a vector_p (utils/vector.h), the same container ab/session.c's
 * global session registry uses for the identical "list of shared, refcounted
 * connections" shape. Every access to the vector itself goes through
 * endpoint_registry_mutex (critical_block) — rc.h's count is atomic and
 * needs no extra lock of its own, but the vector (find/insert/remove) does.
 * ============================================================================ */

typedef struct {
    char          *bind_addr; /* owned copy; NULL means "any interface" */
    uint16_t       port;
    device_sim_t  *sim;
} endpoint_entry_t;

static mutex_p endpoint_registry_mutex = NULL;
static vector_p endpoints = NULL;

/* NULL and "" both mean "any interface"; treat them as the same key so
 * gateway=0.0.0.0 and an absent gateway attribute land on one endpoint. */
static bool addr_eq(const char *a, const char *b) {
    bool a_empty = (!a || *a == '\0');
    bool b_empty = (!b || *b == '\0');
    if(a_empty && b_empty) { return true; }
    if(a_empty != b_empty) { return false; }
    return str_cmp(a, b) == 0;
}

/*
 * rc_alloc cleanup callback — runs once the entry's refcount hits zero, on
 * rc.h's dedicated cleanup thread (or synchronously during the queue drain
 * in refcount_teardown() at library shutdown; never in an arbitrary caller's
 * thread — see rc.c). Unlinks the entry from the registry (under the same
 * mutex every other registry access uses, so a concurrent
 * endpoint_find_or_create() can never observe a half-removed entry) and
 * stops the endpoint. Must NOT free entry_data itself — rc.c's own
 * refcount_cleanup() does that immediately after this returns.
 */
static void endpoint_entry_cleanup(void *entry_data) {
    endpoint_entry_t *e = (endpoint_entry_t *)entry_data;

    if(endpoint_registry_mutex) {
        critical_block(endpoint_registry_mutex) {
            int idx = vector_find_index(endpoints, e);
            if(idx >= 0) { vector_remove(endpoints, idx); }
        }
    }

    pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_INFO, 0, "endpoint_entry_cleanup: stopping endpoint port=%u.", (unsigned)e->port);

    device_sim_destroy(e->sim);
    if(e->bind_addr) { mem_free(e->bind_addr); }
}


extern int32_t endpoint_registry_init(void) {
    if(endpoint_registry_mutex) { return PLCTAG_STATUS_OK; } /* already initialized */

    if(mutex_create(&endpoint_registry_mutex) != PLCTAG_STATUS_OK) { return PLCTAG_ERR_CREATE; }

    endpoints = vector_create(ENDPOINTS_INITIAL_CAPACITY, ENDPOINTS_GROWTH_INCREMENT);
    if(!endpoints) {
        mutex_destroy(&endpoint_registry_mutex);
        return PLCTAG_ERR_NO_MEM;
    }

    return PLCTAG_STATUS_OK;
}


extern void endpoint_registry_teardown(void) {
    if(!endpoint_registry_mutex) { return; }

    /* Safety net: release any endpoint whose server tags were not all
     * destroyed before library shutdown, so the process does not hang on
     * dangling listener threads. Under normal use this list is empty here —
     * every plc_tag_destroy already released its endpoint. rc_dec here only
     * QUEUES the actual teardown (endpoint_entry_cleanup, above); it is
     * guaranteed to have run by the time refcount_teardown() (called later
     * in destroy_modules()) returns, since that function synchronously
     * drains the cleanup queue before this library finishes shutting down. */
    critical_block(endpoint_registry_mutex) {
        int len = vector_length(endpoints);
        for(int i = 0; i < len; i++) {
            endpoint_entry_t *e = (endpoint_entry_t *)vector_get(endpoints, i);
            pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0,
                   "endpoint_registry_teardown: endpoint port=%u still registered at shutdown; releasing.",
                   (unsigned)e->port);
            rc_dec(e);
        }
        vector_destroy(endpoints);
        endpoints = NULL;
    }

    mutex_destroy(&endpoint_registry_mutex);
    endpoint_registry_mutex = NULL;
}


extern device_sim_t *endpoint_find_or_create(const char *bind_addr, uint16_t port, plc_type_t plc_type) {
    device_sim_t *result = NULL;

    if(!endpoint_registry_mutex) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_ERROR, 0, "endpoint_find_or_create: registry not initialized.");
        return NULL;
    }

    critical_block(endpoint_registry_mutex) {
        int len = vector_length(endpoints);
        for(int i = 0; i < len; i++) {
            endpoint_entry_t *e = (endpoint_entry_t *)vector_get(endpoints, i);
            if(e->port == port && addr_eq(e->bind_addr, bind_addr)) {
                /* rc_inc fails (returns NULL) only if the entry's count has
                 * already dropped to zero — a concurrent endpoint_release()
                 * that beat us here but whose endpoint_entry_cleanup() hasn't
                 * run yet (it also needs endpoint_registry_mutex, which we
                 * hold, so it cannot be running concurrently with this scan).
                 * Treat that as "not found" and fall through to create a
                 * fresh endpoint rather than resurrecting a dying one. */
                if(rc_inc(e)) {
                    pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_DETAIL, 0, "endpoint_find_or_create: joining existing endpoint port=%u.",
                           (unsigned)port);
                    result = e->sim;
                }
                break;
            }
        }

        if(!result) {
            device_sim_t *sim = device_sim_create(plc_type, bind_addr, port);
            if(!sim) {
                pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_ERROR, 0, "endpoint_find_or_create: device_sim_create failed.");
                break;
            }

            if(device_sim_start(sim) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_ERROR, 0, "endpoint_find_or_create: device_sim_start failed.");
                device_sim_destroy(sim);
                break;
            }

            endpoint_entry_t *e2 = (endpoint_entry_t *)rc_alloc((int)sizeof(endpoint_entry_t), endpoint_entry_cleanup);
            if(!e2) {
                pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_ERROR, 0, "endpoint_find_or_create: rc_alloc failed.");
                device_sim_destroy(sim);
                break;
            }

            e2->bind_addr = (bind_addr && *bind_addr) ? str_dup(bind_addr) : NULL;
            e2->port      = port;
            e2->sim       = sim;

            if(vector_insert(endpoints, vector_length(endpoints), e2) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_ERROR, 0, "endpoint_find_or_create: vector_insert failed.");
                /* Not yet in the vector, so endpoint_entry_cleanup's
                 * vector_find_index will simply not find it — still safe. */
                rc_dec(e2);
                break;
            }

            pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_INFO, 0, "endpoint_find_or_create: started new endpoint port=%u.", (unsigned)port);
            result = sim;
        }
    }

    return result;
}


extern void endpoint_release(device_sim_t *sim) {
    if(!sim || !endpoint_registry_mutex) { return; }

    /* Find-then-rc_dec inside one critical section, not as two separate
     * steps: releasing endpoint_registry_mutex between "found e" and
     * "rc_dec(e)" would let a concurrent endpoint_entry_cleanup() (which
     * also takes this mutex) free e out from under us. rc_dec itself only
     * ever touches rc.h's own cleanup_mutex, never endpoint_registry_mutex
     * directly, so calling it while already holding endpoint_registry_mutex
     * cannot deadlock. */
    critical_block(endpoint_registry_mutex) {
        int len = vector_length(endpoints);
        for(int i = 0; i < len; i++) {
            endpoint_entry_t *e = (endpoint_entry_t *)vector_get(endpoints, i);
            if(e->sim == sim) {
                rc_dec(e);
                break;
            }
        }
    }
}
