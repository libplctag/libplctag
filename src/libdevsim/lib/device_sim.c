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

#include <stddef.h>
#include <stdint.h>

#include <libplctag/lib/libplctag.h>
#include "platform.h"
#include "utils/debug.h"
#include "device.h"
#include "device_sim.h"
#include "identity.h"
#include "server.h"
#include "discovery.h"

#define DEBUG_MOD DEBUG_MODULE_UTILS

/* ============================================================================
 * Full struct definition (opaque to callers of device_sim.h)
 * ============================================================================ */

struct device_sim_s {
    device_t    dev;
    registry_t *registry;
    thread_p    listener_thread;
    thread_p    discovery_thread;
};

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

static size_t elem_size_for_type(tag_type_t t) {
    switch(t) {
        case TAG_CIP_TYPE_BOOL:
        case TAG_CIP_TYPE_SINT:    return 1;
        case TAG_CIP_TYPE_INT:     return 2;
        case TAG_CIP_TYPE_DINT:
        case TAG_CIP_TYPE_REAL:    return 4;
        case TAG_CIP_TYPE_LINT:
        case TAG_CIP_TYPE_LREAL:   return 8;
        case TAG_PCCC_TYPE_BIT:
        case TAG_PCCC_TYPE_INT:    return 2;
        case TAG_PCCC_TYPE_DINT:
        case TAG_PCCC_TYPE_REAL:   return 4;
        default:                   return 0;
    }
}

static tag_def_t *find_tag(device_t *dev, const char *name) {
    int32_t nlen = str_length(name);
    tag_def_t *tag = dev->tags;
    while(tag) {
        if(str_length(tag->name) == nlen
           && mem_cmp(tag->name, nlen, (void*)name, nlen) == 0) {
            return tag;
        }
        tag = tag->next_tag;
    }
    return NULL;
}

static void free_cip_objects(cip_obj_entry_t *entry) {
    while(entry) {
        cip_obj_entry_t *next = entry->next;
        mem_free(entry);
        entry = next;
    }
}

static void free_tags(tag_def_t *tag) {
    while(tag) {
        tag_def_t *next = tag->next_tag;
        if(tag->data_mutex) { mutex_destroy(&tag->data_mutex); }
        if(tag->data)       { mem_free(tag->data); }
        if(tag->name)       { mem_free(tag->name); }
        mem_free(tag);
        tag = next;
    }
}

static tag_def_t *alloc_tag(const char *name, tag_type_t type, size_t elem_size,
                             size_t elem_count, device_sim_tag_cb read_cb,
                             device_sim_tag_cb write_cb, void *user_data) {
    if(!name || elem_size == 0 || elem_count == 0) { return NULL; }

    int32_t name_len = str_length(name);
    if(name_len <= 0) { return NULL; }

    tag_def_t *tag = (tag_def_t *)mem_alloc((int)sizeof(tag_def_t));
    if(!tag) { return NULL; }
    mem_set(tag, 0, (int)sizeof(tag_def_t));

    tag->name = (char *)mem_alloc(name_len + 1);
    if(!tag->name) { mem_free(tag); return NULL; }
    mem_copy(tag->name, (void*)name, name_len);
    tag->name[name_len] = '\0';

    tag->tag_type   = type;
    tag->elem_size  = elem_size;
    tag->elem_count = elem_count;
    tag->read_cb    = read_cb;
    tag->write_cb   = write_cb;
    tag->user_data  = user_data;

    tag->data = (uint8_t *)mem_alloc((int)(elem_count * elem_size));
    if(!tag->data) { mem_free(tag->name); mem_free(tag); return NULL; }
    mem_set(tag->data, 0, (int)(elem_count * elem_size));

    if(mutex_create(&tag->data_mutex) != PLCTAG_STATUS_OK) {
        mem_free(tag->data); mem_free(tag->name); mem_free(tag);
        return NULL;
    }

    return tag;
}

static void append_tag(device_t *dev, tag_def_t *tag) {
    if(!dev->tags) {
        dev->tags = tag;
        return;
    }
    tag_def_t *t = dev->tags;
    while(t->next_tag) { t = t->next_tag; }
    t->next_tag = tag;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

extern device_sim_t *device_sim_create(plc_type_t plc_type, const char *bind_addr, uint16_t port) {
    device_sim_t *sim = (device_sim_t *)mem_alloc((int)sizeof(device_sim_t));
    if(!sim) { return NULL; }
    mem_set(sim, 0, (int)sizeof(device_sim_t));

    /* Required args plus defaults for the optional knobs (overridable via the
     * device_sim_set_* functions before start). */
    sim->dev.plc_type   = plc_type;
    sim->dev.port       = (port != 0) ? port : 44818;
    sim->dev.bind_addr  = bind_addr;
    sim->dev.local_ipv4 = 0x7F000001u;   /* fallback only; the real reply IP is derived per request from the arrival path */
    sim->dev.client_to_server_max_packet = 508;
    sim->dev.server_to_client_max_packet = 508;
    sim->dev.response_delay_ms = 0;

    sim->dev.sim = sim;
    atomic_init_bool(&sim->dev.terminate, false);

    /* Seed identity from built-in defaults for the chosen PLC type. */
    const identity_t *defaults = identity_for_plc_type(plc_type);
    sim->dev.identity = *defaults;

    if(mutex_create(&sim->dev.identity_mutex) != PLCTAG_STATUS_OK) {
        mem_free(sim);
        return NULL;
    }

    sim->registry = registry_create();
    if(!sim->registry) {
        mutex_destroy(&sim->dev.identity_mutex);
        mem_free(sim);
        return NULL;
    }

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_INFO, 0,
           "device_sim_create: port=%u plc_type=%d.", (unsigned)sim->dev.port, (int)sim->dev.plc_type);

    return sim;
}


extern int32_t device_sim_set_response_delay(device_sim_t *sim, uint32_t response_delay_ms) {
    if(!sim) { return PLCTAG_ERR_BAD_PARAM; }
    sim->dev.response_delay_ms = (int32_t)response_delay_ms;
    return PLCTAG_STATUS_OK;
}


extern int32_t device_sim_set_max_packet(device_sim_t *sim, uint32_t client_to_server, uint32_t server_to_client) {
    if(!sim) { return PLCTAG_ERR_BAD_PARAM; }
    if(client_to_server != 0) { sim->dev.client_to_server_max_packet = client_to_server; }
    if(server_to_client != 0) { sim->dev.server_to_client_max_packet = server_to_client; }
    return PLCTAG_STATUS_OK;
}


extern int32_t device_sim_start(device_sim_t *sim) {
    if(!sim) { return PLCTAG_ERR_NULL_PTR; }

    listener_ctx_t *lctx = (listener_ctx_t *)mem_alloc((int)sizeof(listener_ctx_t));
    if(!lctx) { return PLCTAG_ERR_NO_MEM; }
    lctx->device   = &sim->dev;
    lctx->registry = sim->registry;

    if(thread_create(&sim->listener_thread, server_listener, 131072, lctx) != PLCTAG_STATUS_OK) {
        mem_free(lctx);
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_ERROR, 0, "device_sim_start: listener thread create failed.");
        return PLCTAG_ERR_THREAD_CREATE;
    }

    discovery_ctx_t *dctx = (discovery_ctx_t *)mem_alloc((int)sizeof(discovery_ctx_t));
    if(!dctx) {
        device_sim_stop(sim);
        thread_join(sim->listener_thread);
        thread_destroy(&sim->listener_thread);
        return PLCTAG_ERR_NO_MEM;
    }
    dctx->device   = &sim->dev;
    dctx->registry = sim->registry;

    if(thread_create(&sim->discovery_thread, discovery_thread, 65536, dctx) != PLCTAG_STATUS_OK) {
        mem_free(dctx);
        device_sim_stop(sim);
        thread_join(sim->listener_thread);
        thread_destroy(&sim->listener_thread);
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_ERROR, 0, "device_sim_start: discovery thread create failed.");
        return PLCTAG_ERR_THREAD_CREATE;
    }

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_INFO, 0, "device_sim_start: threads running.");
    return PLCTAG_STATUS_OK;
}


extern int32_t device_sim_stop(device_sim_t *sim) {
    if(!sim) { return PLCTAG_ERR_NULL_PTR; }
    atomic_set_bool(&sim->dev.terminate, true);
    registry_wake_all(sim->registry);
    return PLCTAG_STATUS_OK;
}


extern void device_sim_destroy(device_sim_t *sim) {
    if(!sim) { return; }

    device_sim_stop(sim);

    if(sim->listener_thread) {
        thread_join(sim->listener_thread);
        thread_destroy(&sim->listener_thread);
    }
    if(sim->discovery_thread) {
        thread_join(sim->discovery_thread);
        thread_destroy(&sim->discovery_thread);
    }

    registry_destroy(sim->registry);
    free_cip_objects(sim->dev.cip_objects);
    free_tags(sim->dev.tags);
    mutex_destroy(&sim->dev.identity_mutex);
    mem_free(sim);
}

/* ============================================================================
 * Tag management
 * ============================================================================ */

extern int32_t device_sim_add_tag(device_sim_t *sim,
                                   const char *name, tag_type_t type,
                                   const uint32_t *dims, uint32_t num_dims,
                                   device_sim_tag_cb read_cb,
                                   device_sim_tag_cb write_cb,
                                   void *user_data) {
    if(!sim || !name || !dims || num_dims == 0 || num_dims > 3) {
        return PLCTAG_ERR_BAD_PARAM;
    }

    size_t elem_size = elem_size_for_type(type);
    if(elem_size == 0) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_ERROR, 0,
               "device_sim_add_tag: unknown type 0x%04x for tag '%s'.", (unsigned)type, name);
        return PLCTAG_ERR_BAD_PARAM;
    }

    size_t elem_count = 1;
    for(uint32_t d = 0; d < num_dims; d++) {
        if(dims[d] == 0) { return PLCTAG_ERR_BAD_PARAM; }
        elem_count *= dims[d];
    }

    tag_def_t *tag = alloc_tag(name, type, elem_size, elem_count, read_cb, write_cb, user_data);
    if(!tag) { return PLCTAG_ERR_NO_MEM; }

    tag->num_dimensions = num_dims;
    for(uint32_t d = 0; d < num_dims; d++) { tag->dimensions[d] = dims[d]; }

    append_tag(&sim->dev, tag);

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_INFO, 0,
           "Added CIP tag '%s' type=0x%04x elems=%zu.", name, (unsigned)type, elem_count);
    return PLCTAG_STATUS_OK;
}


extern int32_t device_sim_add_pccc_tag(device_sim_t *sim,
                                        const char *name, tag_type_t type,
                                        uint32_t file_num, uint32_t elem_count,
                                        device_sim_tag_cb read_cb,
                                        device_sim_tag_cb write_cb,
                                        void *user_data) {
    if(!sim || !name || elem_count == 0) { return PLCTAG_ERR_BAD_PARAM; }

    size_t elem_size = elem_size_for_type(type);
    if(elem_size == 0) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_ERROR, 0,
               "device_sim_add_pccc_tag: unknown type 0x%04x for tag '%s'.", (unsigned)type, name);
        return PLCTAG_ERR_BAD_PARAM;
    }

    tag_def_t *tag = alloc_tag(name, type, elem_size, elem_count, read_cb, write_cb, user_data);
    if(!tag) { return PLCTAG_ERR_NO_MEM; }

    tag->data_file_num  = file_num;
    tag->num_dimensions = 1;
    tag->dimensions[0]  = elem_count;

    append_tag(&sim->dev, tag);

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_INFO, 0,
           "Added PCCC tag '%s' file=%u elems=%u.", name, (unsigned)file_num, (unsigned)elem_count);
    return PLCTAG_STATUS_OK;
}

/* ============================================================================
 * CIP object registry
 * ============================================================================ */

extern int32_t device_sim_add_cip_object(device_sim_t *sim,
                                          uint32_t class_id, uint32_t instance_id,
                                          device_sim_cip_cb cb, void *user_data) {
    if(!sim || !cb) { return PLCTAG_ERR_BAD_PARAM; }

    cip_obj_entry_t *entry = (cip_obj_entry_t *)mem_alloc((int)sizeof(cip_obj_entry_t));
    if(!entry) { return PLCTAG_ERR_NO_MEM; }
    mem_set(entry, 0, (int)sizeof(cip_obj_entry_t));

    entry->class_id    = class_id;
    entry->instance_id = instance_id;
    entry->cb          = cb;
    entry->user_data   = user_data;

    /* Append to end of list to preserve registration order. */
    if(!sim->dev.cip_objects) {
        sim->dev.cip_objects = entry;
    } else {
        cip_obj_entry_t *t = sim->dev.cip_objects;
        while(t->next) { t = t->next; }
        t->next = entry;
    }

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_INFO, 0,
           "Registered CIP object class=0x%08x instance=0x%08x.",
           (unsigned)class_id, (unsigned)instance_id);
    return PLCTAG_STATUS_OK;
}

/* ============================================================================
 * Connect/disconnect callbacks
 * ============================================================================ */

extern int32_t device_sim_set_connect_cb(device_sim_t *sim,
                                          device_sim_conn_cb cb, void *user_data) {
    if(!sim) { return PLCTAG_ERR_NULL_PTR; }
    sim->dev.connect_cb        = cb;
    sim->dev.connect_user_data = user_data;
    return PLCTAG_STATUS_OK;
}


extern int32_t device_sim_set_disconnect_cb(device_sim_t *sim,
                                             device_sim_conn_cb cb, void *user_data) {
    if(!sim) { return PLCTAG_ERR_NULL_PTR; }
    sim->dev.disconnect_cb        = cb;
    sim->dev.disconnect_user_data = user_data;
    return PLCTAG_STATUS_OK;
}

/* ============================================================================
 * Identity access
 * ============================================================================ */

extern int32_t device_sim_get_identity(device_sim_t *sim, identity_t *out) {
    if(!sim || !out) { return PLCTAG_ERR_NULL_PTR; }
    mutex_lock(sim->dev.identity_mutex);
    *out = sim->dev.identity;
    mutex_unlock(sim->dev.identity_mutex);
    return PLCTAG_STATUS_OK;
}


extern int32_t device_sim_set_identity(device_sim_t *sim, const identity_t *id) {
    if(!sim || !id) { return PLCTAG_ERR_NULL_PTR; }
    mutex_lock(sim->dev.identity_mutex);
    sim->dev.identity = *id;
    mutex_unlock(sim->dev.identity_mutex);
    return PLCTAG_STATUS_OK;
}

/* ============================================================================
 * Direct tag data access
 * ============================================================================ */

extern int32_t device_sim_tag_get(device_sim_t *sim, const char *name,
                                   uint32_t offset, void *dst, uint32_t len) {
    if(!sim || !name || !dst || len == 0) { return PLCTAG_ERR_BAD_PARAM; }

    tag_def_t *tag = find_tag(&sim->dev, name);
    if(!tag) { return PLCTAG_ERR_NOT_FOUND; }

    size_t total = tag->elem_count * tag->elem_size;
    if(offset >= total) { return PLCTAG_ERR_OUT_OF_BOUNDS; }

    size_t avail = total - offset;
    size_t copy_len = (len < avail) ? len : avail;

    mutex_lock(tag->data_mutex);
    mem_copy(dst, tag->data + offset, (int)copy_len);
    mutex_unlock(tag->data_mutex);

    return PLCTAG_STATUS_OK;
}


extern int32_t device_sim_tag_set(device_sim_t *sim, const char *name,
                                   uint32_t offset, const void *src, uint32_t len) {
    if(!sim || !name || !src || len == 0) { return PLCTAG_ERR_BAD_PARAM; }

    tag_def_t *tag = find_tag(&sim->dev, name);
    if(!tag) { return PLCTAG_ERR_NOT_FOUND; }

    size_t total = tag->elem_count * tag->elem_size;
    if(offset >= total) { return PLCTAG_ERR_OUT_OF_BOUNDS; }

    size_t avail = total - offset;
    size_t copy_len = (len < avail) ? len : avail;

    mutex_lock(tag->data_mutex);
    mem_copy(tag->data + offset, (void*)src, (int)copy_len);
    mutex_unlock(tag->data_mutex);

    return PLCTAG_STATUS_OK;
}
