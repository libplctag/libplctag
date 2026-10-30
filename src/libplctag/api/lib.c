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

#define LIBPLCTAGDLL_EXPORTS (1)

#include <libplctag/api/libplctag.h>

#include <ctype.h>
#include <float.h>
#include <inttypes.h>
#include <libplctag/lib/byte_order.h>
#include <libplctag/lib/init.h>
#include <libplctag/lib/tag_registry.h>
#include <libplctag/lib/tag.h>
#include <libplctag/lib/tag_driver.h>
#include <libplctag/lib/tag_registry.h>
#include <libplctag/lib/tag_string.h>
#include <libplctag/lib/version.h>
#include <libplctag/modules/cip/cip_tag_create.h>
#include <libplctag/modules/modbus/modbus.h>
#include <limits.h>
#include <platform.h>
#include <stdlib.h>
#include <utils/atomic_utils.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/hash.h>
#include <utils/hashtable.h>
#include <utils/random_utils.h>
#include <utils/rc.h>
#include <utils/vector.h>


/* these are only internal to the file */


/* helper functions. */
static int plc_tag_abort_impl(plc_tag_p tag);
static int plc_tag_status_impl(plc_tag_p tag);
static int wait_for_status(plc_tag_p tag, int timeout, int (*status)(plc_tag_p tag, uint32_t seq), uint32_t seq);
static int create_status(plc_tag_p tag, uint32_t seq);
static int op_status(plc_tag_p tag, uint32_t seq);
static int wait_for_op(plc_tag_p tag, int timeout, uint32_t seq);
static int32_t plc_tag_create_impl(const char *attrib_str,
                                   void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                   void *userdata, int timeout, plc_tag_p src_tag);


#ifdef LIBPLCTAGDLL_EXPORTS
#    if defined(_WIN32) || defined(_WIN64)
#        include <process.h>
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch(fdwReason) {
        case DLL_PROCESS_ATTACH: break;

        case DLL_PROCESS_DETACH:
            /*
             * Only tear down on an explicit FreeLibrary() (lpvReserved == NULL),
             * where the process is still alive and our worker threads can be
             * joined. When lpvReserved != NULL the process is terminating: the
             * loader has already killed every other thread, so plc_tag_shutdown()
             * would block forever waiting on them. Applications should still call
             * plc_tag_shutdown() explicitly during orderly shutdown rather than
             * rely on this path.
             */
            if(lpvReserved == NULL) { plc_tag_shutdown(); }
            break;

        case DLL_THREAD_ATTACH: break;

        case DLL_THREAD_DETACH: break;

        default: break;
    }

    return TRUE;
}
#    endif
#endif


int plc_tag_generic_wake_tag_impl(const char *func, int line_num, plc_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, tag->tag_id, "Starting. Called from %s:%d.", func, line_num);

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "Called from %s:%d when tag is NULL!", func, line_num);
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!tag->tag_cond_wait) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "Called from %s:%d when tag condition var is NULL!", func, line_num);
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = cond_signal(tag->tag_cond_wait);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "Error %s trying to signal condition variable in call from %s:%d",
               plc_tag_decode_error(rc), func, line_num);
        return rc;
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, tag->tag_id, "Done. Called from %s:%d.", func, line_num);

    return rc;
}


void tag_event_push(plc_tag_p tag, int event, int status) {
    /* do not stack up events if there is no callback. */
    if(!tag->callback) { return; }

    /* the last slot is kept for DESTROYED, which releases the callback. */
    if(tag->event_count >= ((event == PLCTAG_EVENT_DESTROYED) ? TAG_EVENT_RING_SIZE : TAG_EVENT_RING_SIZE - 1)) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_ERROR, tag->tag_id, "Event ring full, dropping event %d with status %s!", event,
               plc_tag_decode_error(status));
        return;
    }

    tag->events[(tag->event_head + tag->event_count) & TAG_EVENT_RING_MASK] =
        (tag_event_t){.event = (uint8_t)event, .status = (int8_t)status};
    tag->event_count++;
}


void tag_created(plc_tag_p tag, int rc) {
    if(tag->created) { return; }

    tag->created = true;
    tag_event_push(tag, PLCTAG_EVENT_CREATED, rc);
}


void tag_op_start(plc_tag_p tag, bool is_read) {
    tag->op_seq++;
    tag_op_set(tag, (is_read ? TAG_OP_READ_REQUEST : TAG_OP_WRITE_REQUEST));

    if(tag->created) { tag_event_push(tag, (is_read ? PLCTAG_EVENT_READ_STARTED : PLCTAG_EVENT_WRITE_STARTED), PLCTAG_STATUS_OK); }
}


void tag_op_end(plc_tag_p tag, int rc) {
    bool is_read = tag_op_is_read(tag);

    if(!tag_op_busy(tag)) { return; }

    tag->op_end_seq = tag->op_seq;
    tag->op_end_status = (int8_t)rc;
    tag->status = (int8_t)rc;
    tag_op_set(tag, TAG_OP_IDLE);

    if(!is_read) { tag->auto_sync_next_write = 0; }

    if(tag->created) {
        tag_event_push(tag, (is_read ? PLCTAG_EVENT_READ_COMPLETED : PLCTAG_EVENT_WRITE_COMPLETED), rc);
    } else {
        tag_created(tag, rc);
    }

    cond_signal(tag->tag_cond_wait);
}


void tag_op_abort(plc_tag_p tag) {
    bool was_busy = tag_op_busy(tag);
    bool is_read = tag_op_is_read(tag);

    if(was_busy) {
        tag->op_end_seq = tag->op_seq;
        tag->op_end_status = (int8_t)PLCTAG_ERR_ABORT;
    }

    tag_op_set(tag, TAG_OP_IDLE);

    if(!tag->created) {
        tag_created(tag, PLCTAG_ERR_ABORT);
    } else if(was_busy) {
        tag_event_push(tag, (is_read ? PLCTAG_EVENT_READ_COMPLETED : PLCTAG_EVENT_WRITE_COMPLETED), PLCTAG_ERR_ABORT);
    }

    tag_event_push(tag, PLCTAG_EVENT_ABORTED, PLCTAG_ERR_ABORT);

    cond_signal(tag->tag_cond_wait);
}


/* Run the callback for each queued event, oldest first. */
void plc_tag_generic_handle_event_callbacks(plc_tag_p tag) {
    if(!tag) { return; }

    critical_block(tag->api_mutex) {
        /*
         * Under the API mutex, not before: plc_tag_destroy() clears the callback under this same
         * mutex, and a handler thread holding its own reference can get here after that.
         * A callback may queue more events, or unregister itself; the loop sees both.
         */
        /* called from a callback: the outer loop delivers these after it returns. */
        if(tag->dispatching) { break; }

        tag->dispatching = true;

        while(tag->event_count > 0) {
            tag_event_t event = tag->events[tag->event_head];

            if(!tag->callback) {
                tag->event_count = 0;
                break;
            }

            tag->event_head = (uint8_t)((tag->event_head + 1) & TAG_EVENT_RING_MASK);
            tag->event_count--;

            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Dispatching event %d with status %s.", (int)event.event,
                   plc_tag_decode_error(event.status));

            tag->callback(tag->tag_id, (int)event.event, (int)event.status, tag->userdata);

            /* DESTROYED is last; userdata may already be freed. */
            if(event.event == PLCTAG_EVENT_DESTROYED) {
                tag->callback = NULL;
                atomic_set_bool(&tag->has_callback, false);
                tag->userdata = NULL;
                tag->event_count = 0;
            }
        }

        tag->dispatching = false;
    }
}


int plc_tag_generic_init_tag(plc_tag_p tag, attr attribs,
                             void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata), void *userdata) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag->tag_id, "Starting.");

    /* get the connection group ID here rather than in each PLC specific tag type. */
    tag->connection_group_id = attr_get_int(attribs, "connection_group_id", 0);
    if(tag->connection_group_id < 0 || tag->connection_group_id > 32767) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id,
               "Connection group ID must be between 0 and 32767, inclusive, but was %d!", tag->connection_group_id);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    rc = mutex_create(&(tag->ext_mutex));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "Unable to create tag external mutex!");
        return PLCTAG_ERR_CREATE;
    }

    rc = mutex_create(&(tag->api_mutex));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "Unable to create tag API mutex!");
        return PLCTAG_ERR_CREATE;
    }

    rc = cond_create(&(tag->tag_cond_wait));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "Unable to create tag condition variable!");
        return PLCTAG_ERR_CREATE;
    }

    /* do this early so that events can be raised early. */
    tag->callback = tag_callback_func;
    atomic_set_bool(&tag->has_callback, (tag_callback_func != NULL));
    tag->userdata = userdata;

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag->tag_id, "Done.");

    return rc;
}


/* takes the API mutex itself. */
static int plc_tag_abort_impl(plc_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag->tag_id, "Starting.");

    /*
     * Flag the abort before waiting on the mutex, so that a handler working on the tag sees
     * it and lets go.  Otherwise this waits until any long running operation completes.
     */
    atomic_set_bool(&tag->abort_requested, true);

    /* wake the tag's handler so it can act on the flag. */
    if(tag->vtable && tag->vtable->wake_plc) { rc = tag->vtable->wake_plc(tag); }

    critical_block(tag->api_mutex) {
        atomic_set_int64(&tag->read_cache_expire, 0);

        /* the handler may have done the abort already, and a new op may have started since. */
        if(atomic_get_bool(&tag->abort_requested)) {
            atomic_set_bool(&tag->abort_requested, false);

            /* the events first: the protocol abort changes nothing they report. */
            tag_op_abort(tag);

            if(tag->vtable && tag->vtable->abort) {
                rc = tag->vtable->abort(tag);
            } else {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "Tag does not have an abort function.");
                rc = PLCTAG_ERR_NOT_IMPLEMENTED;
            }
        } else {
            pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag->tag_id, "Abort flag is not set, so abort already completed.");
            rc = PLCTAG_STATUS_OK;
        }
    }

    plc_tag_generic_handle_event_callbacks(tag);

    return rc;
}


static int plc_tag_status_impl(plc_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, tag->tag_id, "Starting.");

    critical_block(tag->api_mutex) {
        if(tag->vtable && tag->vtable->tickler) { tag->vtable->tickler(tag); }

        if(tag->vtable && tag->vtable->status) {
            rc = tag->vtable->status(tag);
        } else {
            rc = PLCTAG_ERR_NOT_IMPLEMENTED;
        }

        if(rc == PLCTAG_STATUS_OK && tag_op_busy(tag)) { rc = PLCTAG_STATUS_PENDING; }

        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "status rc=%s, op state %d.", plc_tag_decode_error(rc),
               (int)tag_op_state(tag));
    }

    plc_tag_generic_handle_event_callbacks(tag);

    return rc;
}


/**************************************************************************
 ***************************  API Functions  ******************************
 **************************************************************************/


/*
 * plc_tag_decode_error()
 *
 * This takes an integer error value and turns it into a printable string.
 *
 * Returns a pointer to a static string.
 */


LIB_EXPORT const char *plc_tag_decode_error(int rc) {
    switch(rc) {
        case PLCTAG_STATUS_PENDING: return "PLCTAG_STATUS_PENDING";
        case PLCTAG_STATUS_OK: return "PLCTAG_STATUS_OK";
        case PLCTAG_ERR_ABORT: return "PLCTAG_ERR_ABORT";
        case PLCTAG_ERR_BAD_CONFIG: return "PLCTAG_ERR_BAD_CONFIG";
        case PLCTAG_ERR_BAD_CONNECTION: return "PLCTAG_ERR_BAD_CONNECTION";
        case PLCTAG_ERR_BAD_DATA: return "PLCTAG_ERR_BAD_DATA";
        case PLCTAG_ERR_BAD_DEVICE: return "PLCTAG_ERR_BAD_DEVICE";
        case PLCTAG_ERR_BAD_GATEWAY: return "PLCTAG_ERR_BAD_GATEWAY";
        case PLCTAG_ERR_BAD_PARAM: return "PLCTAG_ERR_BAD_PARAM";
        case PLCTAG_ERR_BAD_REPLY: return "PLCTAG_ERR_BAD_REPLY";
        case PLCTAG_ERR_BAD_STATUS: return "PLCTAG_ERR_BAD_STATUS";
        case PLCTAG_ERR_CLOSE: return "PLCTAG_ERR_CLOSE";
        case PLCTAG_ERR_CREATE: return "PLCTAG_ERR_CREATE";
        case PLCTAG_ERR_DUPLICATE: return "PLCTAG_ERR_DUPLICATE";
        case PLCTAG_ERR_ENCODE: return "PLCTAG_ERR_ENCODE";
        case PLCTAG_ERR_MUTEX_DESTROY: return "PLCTAG_ERR_MUTEX_DESTROY";
        case PLCTAG_ERR_MUTEX_INIT: return "PLCTAG_ERR_MUTEX_INIT";
        case PLCTAG_ERR_MUTEX_LOCK: return "PLCTAG_ERR_MUTEX_LOCK";
        case PLCTAG_ERR_MUTEX_UNLOCK: return "PLCTAG_ERR_MUTEX_UNLOCK";
        case PLCTAG_ERR_NOT_ALLOWED: return "PLCTAG_ERR_NOT_ALLOWED";
        case PLCTAG_ERR_NOT_FOUND: return "PLCTAG_ERR_NOT_FOUND";
        case PLCTAG_ERR_NOT_IMPLEMENTED: return "PLCTAG_ERR_NOT_IMPLEMENTED";
        case PLCTAG_ERR_NO_DATA: return "PLCTAG_ERR_NO_DATA";
        case PLCTAG_ERR_NO_MATCH: return "PLCTAG_ERR_NO_MATCH";
        case PLCTAG_ERR_NO_MEM: return "PLCTAG_ERR_NO_MEM";
        case PLCTAG_ERR_NO_RESOURCES: return "PLCTAG_ERR_NO_RESOURCES";
        case PLCTAG_ERR_NULL_PTR: return "PLCTAG_ERR_NULL_PTR";
        case PLCTAG_ERR_OPEN: return "PLCTAG_ERR_OPEN";
        case PLCTAG_ERR_OUT_OF_BOUNDS: return "PLCTAG_ERR_OUT_OF_BOUNDS";
        case PLCTAG_ERR_READ: return "PLCTAG_ERR_READ";
        case PLCTAG_ERR_REMOTE_ERR: return "PLCTAG_ERR_REMOTE_ERR";
        case PLCTAG_ERR_THREAD_CREATE: return "PLCTAG_ERR_THREAD_CREATE";
        case PLCTAG_ERR_THREAD_JOIN: return "PLCTAG_ERR_THREAD_JOIN";
        case PLCTAG_ERR_TIMEOUT: return "PLCTAG_ERR_TIMEOUT";
        case PLCTAG_ERR_TOO_LARGE: return "PLCTAG_ERR_TOO_LARGE";
        case PLCTAG_ERR_TOO_SMALL: return "PLCTAG_ERR_TOO_SMALL";
        case PLCTAG_ERR_UNSUPPORTED: return "PLCTAG_ERR_UNSUPPORTED";
        case PLCTAG_ERR_WINSOCK: return "PLCTAG_ERR_WINSOCK";
        case PLCTAG_ERR_WRITE: return "PLCTAG_ERR_WRITE";
        case PLCTAG_ERR_PARTIAL: return "PLCTAG_ERR_PARTIAL";
        case PLCTAG_ERR_BUSY: return "PLCTAG_ERR_BUSY";

        default: return "Unknown error."; break;
    }

    return "Unknown error.";
}


/*
 * Set the debug level.
 *
 * This function takes values from the defined debug levels.  It sets
 * the debug level to the passed value.  Higher numbers output increasing amounts
 * of information.   Input values not defined will be ignored.
 */

LIB_EXPORT void plc_tag_set_debug_level(int debug_level) {
    if(debug_level >= PLCTAG_DEBUG_NONE && debug_level <= PLCTAG_DEBUG_SPEW) { set_debug_level(debug_level); }
}


LIB_EXPORT int plc_tag_set_debug_module_level(plctag_debug_module_t module, int debug_level) {
    if(debug_level < PLCTAG_DEBUG_NONE || debug_level > PLCTAG_DEBUG_SPEW) { return PLCTAG_ERR_BAD_PARAM; }
    debug_module_set_level((debug_module_t)module, debug_level);
    return PLCTAG_STATUS_OK;
}


LIB_EXPORT int plc_tag_get_debug_module_level(plctag_debug_module_t module) {
    return debug_module_get_level((debug_module_t)module);
}


LIB_EXPORT int plc_tag_get_debug_level(void) { return get_debug_level(); }


/*
 * Check that the library supports the required API version.
 *
 * PLCTAG_STATUS_OK is returned if the version matches.  If it does not,
 * PLCTAG_ERR_UNSUPPORTED is returned.
 */

LIB_EXPORT int plc_tag_check_lib_version(int req_major, int req_minor, int req_patch) {
    /* encode these with 16-bits per version part. */
    uint64_t lib_encoded_version =
        (((uint64_t)version_major) << 32u) + (((uint64_t)version_minor) << 16u) + (uint64_t)version_patch;

    uint64_t req_encoded_version = (((uint64_t)req_major) << 32u) + (((uint64_t)req_minor) << 16u) + (uint64_t)req_patch;

    if(version_major == (uint64_t)req_major && lib_encoded_version >= req_encoded_version) {
        return PLCTAG_STATUS_OK;
    } else {
        return PLCTAG_ERR_UNSUPPORTED;
    }
}


/*
 * plc_tag_create()
 *
 * This is where the dispatch occurs to the protocol specific implementation.
 */

LIB_EXPORT int32_t plc_tag_create(const char *attrib_str, int timeout) {
    return plc_tag_create_impl(attrib_str, NULL, NULL, timeout, NULL);
}


LIB_EXPORT int32_t plc_tag_create_ex(const char *attrib_str,
                                     void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                     void *userdata, int timeout) {
    return plc_tag_create_impl(attrib_str, tag_callback_func, userdata, timeout, NULL);
}


LIB_EXPORT int32_t plc_tag_create_from_tag(int32_t src_tag_id, const char *attrib_str,
                                           void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                           void *userdata, int timeout) {
    plc_tag_p src_tag = NULL;
    int32_t rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, src_tag_id, "Starting create-from-tag.");

    if(src_tag_id <= 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, src_tag_id, "Source tag ID is invalid.");
        return PLCTAG_ERR_BAD_PARAM;
    }

    src_tag = lookup_tag(src_tag_id);
    if(!src_tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, src_tag_id, "Source tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    switch(src_tag->protocol_type) {
        case TAG_PROTOCOL_SYSTEM:
        case TAG_PROTOCOL_UNKNOWN:
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, src_tag_id, "Source tag protocol type %d does not support connection sharing.",
                   src_tag->protocol_type);
            rc_dec(src_tag);
            return PLCTAG_ERR_NOT_ALLOWED;
        default: break;
    }

    rc = plc_tag_create_impl(attrib_str, tag_callback_func, userdata, timeout, src_tag);
    rc_dec(src_tag);

    return rc;
}


static int32_t plc_tag_create_impl(const char *attrib_str,
                                   void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                   void *userdata, int timeout, plc_tag_p src_tag) {
    plc_tag_p tag = PLC_TAG_P_NULL;
    int id = PLCTAG_ERR_OUT_OF_BOUNDS;
    attr attribs = NULL;
    int rc = PLCTAG_STATUS_OK;
    int read_cache_ms = 0;
    tag_create_function tag_constructor;
    int debug_level = -1;
    tag_registry_p inst = NULL;

    /* we are creating a tag, there is no ID yet. */
    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Starting");


    /* make sure that all modules are initialized. */
    if((rc = initialize_modules()) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_ERROR, 0, "Unable to initialize the internal library state!");
        return rc;
    }

    /* check the arguments */

    if(timeout < 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "Timeout must not be negative!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(!attrib_str) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "Tag attribute string is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(str_length(attrib_str) == 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "Tag attribute string is zero length!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    attribs = attr_create_from_str(attrib_str);
    if(!attribs) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "Unable to parse attribute string!");
        return PLCTAG_ERR_BAD_DATA;
    }

    /* set debug level */
    debug_level = attr_get_int(attribs, "debug", -1);
    if(debug_level > DEBUG_NONE) { set_debug_level(debug_level); }

    /* Acquire a reference to the running instance and transfer it into tag->instance
     * below once the tag exists. initialize_modules() above guarantees one was
     * published, but a shutdown can race in between -- tag_registry_acquire()
     * returning NULL here means exactly that. */
    inst = tag_registry_acquire();
    if(!inst) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "Library is shutting down, cannot create tag.");
        attr_destroy(attribs);
        return PLCTAG_ERR_NOT_ALLOWED;
    }

    /*
     * create the tag, this is protocol specific.
     *
     * If this routine wants to keep the attributes around, it needs
     * to clone them.
     *
     * A cloned tag names no protocol attribute, so it is made by whatever made its source.
     */
    tag_constructor = (src_tag ? src_tag->create_func : find_tag_create_func(attribs));

    if(!tag_constructor) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "Tag creation failed, no tag constructor found for tag type!");
        attr_destroy(attribs);
        rc_dec(inst);
        return src_tag ? PLCTAG_ERR_NOT_ALLOWED : PLCTAG_ERR_BAD_PARAM;
    }

    rc = tag_constructor(attribs, tag_callback_func, userdata, src_tag, &tag);

    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "Warning, %s error found while creating tag!", plc_tag_decode_error(rc));

        /*
         * A constructor that failed after allocating still hands the tag back so it can be
         * released here.  rc_dec() tolerates NULL, so the two cases need no separate paths,
         * and the instance reference was never transferred, so it is released here too.
         */
        attr_destroy(attribs);
        rc_dec(tag);
        rc_dec(inst);

        return rc;
    }

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "Tag creation failed, skipping mutex creation and other generic setup.");
        attr_destroy(attribs);
        rc_dec(inst);
        return PLCTAG_ERR_CREATE;
    }

    tag->create_func = tag_constructor;

    /* Transfer the acquired reference into the tag; the tag's own destructor
     * releases it (see each protocol destructor's rc_dec(tag->instance)). Every
     * failure path below this point already routes through rc_dec(tag), so it does
     * not need its own rc_dec(inst) -- that would double-release. */
    tag->instance = inst;
    inst = NULL;

    /* set up the read cache config. */
    read_cache_ms = attr_get_int(attribs, "read_cache_ms", 0);
    if(read_cache_ms < 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "read_cache_ms value must be positive, using zero.");
        read_cache_ms = 0;
    }

    atomic_set_int64(&tag->read_cache_expire, 0);
    atomic_set_int64(&tag->read_cache_ms, (int64_t)read_cache_ms);

    /* set up any automatic read/write */
    tag->auto_sync_read_ms = attr_get_int(attribs, "auto_sync_read_ms", 0);
    if(tag->auto_sync_read_ms < 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "auto_sync_read_ms value must be positive!");
        attr_destroy(attribs);
        rc_dec(tag);
        return PLCTAG_ERR_BAD_PARAM;
    } else if(tag->auto_sync_read_ms > 0) {
        /* how many periods did we already pass? */
        /* start some time in the future, but with random jitter. */
        tag->auto_sync_next_read = time_ms() + (int64_t)(random_u64((uint64_t)tag->auto_sync_read_ms));
    }

    tag->auto_sync_write_ms = attr_get_int(attribs, "auto_sync_write_ms", 0);
    if(tag->auto_sync_write_ms < 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "auto_sync_write_ms value must be positive!");
        attr_destroy(attribs);
        rc_dec(tag);
        return PLCTAG_ERR_BAD_PARAM;
    } else {
        tag->auto_sync_next_write = 0;
    }

    tag->destroy_idle_sec = attr_get_int(attribs, "destroy_tag_after_idle_sec", 0);
    if(tag->destroy_idle_sec < 0 || (tag->destroy_idle_sec > 0 && tag->destroy_idle_sec < 10)) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "destroy_tag_after_idle_sec must be zero or at least 10!");
        attr_destroy(attribs);
        rc_dec(tag);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    if(tag->destroy_idle_sec > 0 && (tag->auto_sync_read_ms > 0 || tag->auto_sync_write_ms > 0)) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "destroy_tag_after_idle_sec cannot be used with auto-sync!");
        attr_destroy(attribs);
        rc_dec(tag);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* See if we are allowed to resize fields */
    tag->allow_field_resize = (uint8_t)(attr_get_int(attribs, "allow_field_resize", 0) ? 1 : 0);

    /*
     * Set up the tag byte order if there are any overrides.  A tag still waiting to learn its
     * PLC has no byte order to override yet; its module applies them when it does.
     */
    rc = (tag->byte_order ? set_tag_byte_order(tag, attribs) : PLCTAG_STATUS_OK);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "Unable to correctly set tag data byte order: %s!", plc_tag_decode_error(rc));
        attr_destroy(attribs);
        rc_dec(tag);
        return rc;
    }

    /*
     * Release memory for attributes
     */
    attr_destroy(attribs);

    /* map the tag to a tag ID */
    id = add_tag_lookup(tag);

    /* if the mapping failed, then punt */
    if(id < 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_ERROR, 0, "Unable to map tag %p to lookup table entry, rc=%s", tag,
               plc_tag_decode_error(id));
        rc_dec(tag);
        return id;
    }

    /* tag->tag_id is already set (add_tag_lookup() sets it under tag_lookup_mutex,
     * in the same critical section that publishes the tag into the hashtable --
     * see the comment there). */
    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag->tag_id, "Returning mapped tag ID %d", id);

    /* Let protocols with their own worker-thread publishing step (e.g. Modbus) do it
     * now that every generic field above is set. See the comment on activate in
     * tag.h for why this can't happen earlier, inside the protocol's tag_create_function.
     *
     * The tag is already published into the hashtable (add_tag_lookup() above), so the
     * PLC handler thread can already find and tickle it -- activate() must take api_mutex
     * like status()/abort() below do, or its reads/writes of tag fields (e.g. tag->op in
     * mb_activate/mb_read_start) race the handler thread's tickle_tag(), which changes
     * tag->op under api_mutex + plc->mutex (see modbus.c tickle_all_tags). */
    if(tag->vtable && tag->vtable->activate) {
        critical_block(tag->api_mutex) { tag->vtable->activate(tag); }
    }

    /* wake up tag's PLC here. */
    if(tag->vtable && tag->vtable->wake_plc) { tag->vtable->wake_plc(tag); }

    /* get the tag status. */
    if(tag->vtable && tag->vtable->status) {
        critical_block(tag->api_mutex) { rc = tag->vtable->status(tag); }
    }

    /* check to see if there was an error during tag creation. */
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "Error %s while trying to create tag!", plc_tag_decode_error(rc));
        if(tag->vtable && tag->vtable->abort) {
            critical_block(tag->api_mutex) { tag->vtable->abort(tag); }
        }

        /*
         * If shutdown already took the tag out of the registry, it raises DESTROYED for this id,
         * so hand the id back: every event the caller sees is for an id it holds.
         */
        if(!remove_tag_lookup(tag)) {
            rc_dec(tag);
            return id;
        }

        /*
         * The caller may free userdata once this returns an error, and a handler thread can
         * still hold a reference with events queued.  Dispatch runs under api_mutex and drops
         * events once callback is NULL, so this also waits out a callback already running.
         */
        critical_block(tag->api_mutex) {
            tag->callback = NULL;
            atomic_set_bool(&tag->has_callback, false);
            tag->userdata = NULL;
        }

        rc_dec(tag);
        return rc;
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Tag status after creation is %s.", plc_tag_decode_error(rc));

    /* a tag with nothing left to do is created now; one still working queues CREATED when it ends. */
    if(rc == PLCTAG_STATUS_OK) {
        critical_block(tag->api_mutex) { tag_created(tag, PLCTAG_STATUS_OK); }
    }

    /*
     * if there is a timeout, then wait until we get
     * an error or we timeout.
     */
    if(timeout > 0 && rc == PLCTAG_STATUS_PENDING) {
        int64_t start_time = time_ms();

        rc = wait_for_status(tag, timeout, create_status, 0);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "Error %s while trying to create tag!", plc_tag_decode_error(rc));

            /* the vtable abort() expects the caller to hold api_mutex, and takes plc->mutex itself. */
            if(tag->vtable && tag->vtable->abort) {
                critical_block(tag->api_mutex) { tag->vtable->abort(tag); }
            }

            /* as above: if shutdown took the tag, it raises DESTROYED for this id. */
            if(!remove_tag_lookup(tag)) {
                rc_dec(tag);
                return id;
            }

            /* as above: no callback after an error return. */
            critical_block(tag->api_mutex) {
                tag->callback = NULL;
                atomic_set_bool(&tag->has_callback, false);
                tag->userdata = NULL;
            }

            rc_dec(tag);
            return rc;
        }

        /* the module queued CREATED when its create finished; this covers one that did not. */
        critical_block(tag->api_mutex) { tag_created(tag, rc); }

        pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag->tag_id, "tag set up elapsed time %" PRId64 "ms", (time_ms() - start_time));
    }

    /* dispatch any outstanding events. */
    plc_tag_generic_handle_event_callbacks(tag);

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag->tag_id, "Done.");

    return id;
}


/*
 * plc_tag_shutdown
 *
 * Some systems may not be able to call atexit() handlers.  In those cases, wrappers should
 * call this function before unloading the library or terminating.   Most OSes will cleanly
 * recover all system resources when a process is terminated and this will not be necessary.
 */

LIB_EXPORT void plc_tag_shutdown(void) {
    tag_registry_p inst = NULL;

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Starting.");

    /* Atomically close the gate: exactly one caller -- concurrent or repeated --
     * can ever observe current_instance non-NULL and clear it. Every other caller
     * sees NULL and returns immediately. This replaces the old two-load
     * "if(!tags || !lib_active)" guard, which could let concurrent or repeated
     * plc_tag_shutdown() calls both proceed. */
    inst = tag_registry_close();

    if(!inst) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0,
               "plc_tag_shutdown() called after previous shutdown, or library not running. Ignoring.");
        return;
    }

    /* terminate anything waiting on the library and prevent any tags from being created. */
    atomic_set_bool(&lib_active, false);

    /* close all tags. */
    tag_registry_destroy_all_tags(inst);

    /* Stage the registry for tag_registry_teardown() (called via destroy_modules() below) to
     * drop the last ("genesis") reference, once refcount_teardown() has drained
     * every tag's own reference too. */
    tag_registry_stage_shutdown(inst);

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Cleaning up library resources.");

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "About to destroy modules.");
    destroy_modules();
    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Modules destroyed successfully.");

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Done.");
}


/*
 * plc_tag_register_callback
 *
 * This function registers the passed callback function with the tag.  Only one callback function
 * may be registered on a tag at a time!
 *
 * Once registered, any of the following operations on or in the tag will result in the callback
 * being called:
 *
 *      * a tag handle finishing creation.
 *      * starting a tag read operation.
 *      * a tag read operation ending.
 *      * a tag read being aborted.
 *      * starting a tag write operation.
 *      * a tag write operation ending.
 *      * a tag write being aborted.
 *      * a tag being destroyed
 *
 * The callback is called INSIDE the tag's internal mutex.  The mutex is recursive, so the callback may call
 * tag functions on its own tag.  The callback may be called on a library connection thread, or on an
 * application thread that is inside a library call.  YOU are responsible for making sure that all client
 * application data structures the callback function touches are safe to access by the callback!
 *
 * Because the tag's mutex is held, in the callback do not:
 *
 *      * call a function with a timeout (a synchronous read or write) on the same tag.  The operation
 *        cannot complete while the mutex is held, so the call always times out.
 *      * wait for another thread that may call a function on this tag, for instance plc_tag_destroy().
 *        That thread waits for the mutex and the callback waits for that thread: deadlock.
 *      * call functions on another tag whose callback may call functions on this tag.  Two callbacks
 *        running at once on different threads then each wait for the other's mutex: deadlock.
 *
 * Do not do any operations in the callback that block for any significant time.   This will cause library
 * performance to be poor or even to start failing!
 *
 * When the callback is called with PLCTAG_EVENT_DESTROYED, do not call any tag functions on that tag.  It is
 * not guaranteed that they will work and they will possibly hang or fail.
 *
 * Return values:
 *void (*tag_callback_func)(int32_t tag_id, uint32_t event, int status)
 * If there is already a callback registered, the function will return PLCTAG_ERR_DUPLICATE.   Only one callback
 * function may be registered at a time on each tag.
 *
 * If all is successful, the function will return PLCTAG_STATUS_OK.
 *
 * Also see plc_tag_register_callback_ex.
 */


/* there needs to be a better way to make the cast clean than this! */
#ifndef _MSC_VER
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wcast-function-type"
#endif

LIB_EXPORT int plc_tag_register_callback(int32_t tag_id, tag_callback_func callback_func) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag_id, "Starting.");

    rc = plc_tag_register_callback_ex(tag_id, (tag_extended_callback_func)callback_func, NULL);

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag_id, "Done.");

    return rc;
}

#ifndef _MSC_VER
#    pragma GCC diagnostic pop
#endif

/*
 * plc_tag_register_callback_ex
 *
 * This function registers the passed callback function and user data with the tag.  Only one callback function
 * may be registered on a tag at a time!
 *
 * Once registered, any of the following operations on or in the tag will result in the callback
 * being called:
 *
 *      * a tag handle finishing creation.
 *      * starting a tag read operation.
 *      * a tag read operation ending.
 *      * a tag read being aborted.
 *      * starting a tag write operation.
 *      * a tag write operation ending.
 *      * a tag write being aborted.
 *      * a tag being destroyed
 *
 * The callback is called INSIDE the tag's internal mutex.  The mutex is recursive, so the callback may call
 * tag functions on its own tag.  The callback may be called on a library connection thread, or on an
 * application thread that is inside a library call.  YOU are responsible for making sure that all client
 * application data structures the callback function touches are safe to access by the callback!
 *
 * Because the tag's mutex is held, in the callback do not:
 *
 *      * call a function with a timeout (a synchronous read or write) on the same tag.  The operation
 *        cannot complete while the mutex is held, so the call always times out.
 *      * wait for another thread that may call a function on this tag, for instance plc_tag_destroy().
 *        That thread waits for the mutex and the callback waits for that thread: deadlock.
 *      * call functions on another tag whose callback may call functions on this tag.  Two callbacks
 *        running at once on different threads then each wait for the other's mutex: deadlock.
 *
 * Do not do any operations in the callback that block for any significant time.   This will cause library
 * performance to be poor or even to start failing!
 *
 * When the callback is called with PLCTAG_EVENT_DESTROYED, do not call any tag functions on that tag.  It is
 * not guaranteed that they will work and they will possibly hang or fail.
 *
 * Return values:
 *
 * If there is already a callback registered, the function will return PLCTAG_ERR_DUPLICATE.   Only one callback
 * function may be registered at a time on each tag.
 *
 * If all is successful, the function will return PLCTAG_STATUS_OK.
 *
 * Also see plc_tag_register_callback.
 */

LIB_EXPORT int plc_tag_register_callback_ex(int32_t tag_id, tag_extended_callback_func callback_func, void *userdata) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(tag_id);

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag_id, "Starting.");

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        if(tag->callback) {
            rc = PLCTAG_ERR_DUPLICATE;
        } else {
            if(callback_func) {
                tag->callback = callback_func;
                tag->userdata = userdata;
            } else {
                tag->callback = NULL;
                tag->userdata = NULL;
            }

            atomic_set_bool(&tag->has_callback, (callback_func != NULL));
        }
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag->tag_id, "Done.");

    return rc;
}


/*
 * plc_tag_unregister_callback
 *
 * This function removes the callback already registered on the tag.
 *
 * Return values:
 *
 * The function returns PLCTAG_STATUS_OK if there was a registered callback and removing it went well.
 * An error of PLCTAG_ERR_NOT_FOUND is returned if there was no registered callback.
 */

LIB_EXPORT int plc_tag_unregister_callback(int32_t tag_id) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(tag_id);

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag_id, "Starting.");

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        if(tag->callback) {
            rc = PLCTAG_STATUS_OK;
            tag->callback = NULL;
            atomic_set_bool(&tag->has_callback, false);
            tag->userdata = NULL;
        } else {
            rc = PLCTAG_ERR_NOT_FOUND;
        }
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag->tag_id, "Done.");

    return rc;
}


/*
 * plc_tag_register_logger
 *
 * This function registers the passed callback function with the library.  Only one callback function
 * may be registered with the library at a time!
 *
 * Once registered, the function will be called with any logging message that is normally printed due
 * to the current log level setting.
 *
 * WARNING: the callback will usually be called when the internal tag API mutex is held.   You cannot
 * call any tag functions within the callback!
 *
 * Return values:
 *
 * If there is already a callback registered, the function will return PLCTAG_ERR_DUPLICATE.   Only one callback
 * function may be registered at a time on each tag.
 *
 * If all is successful, the function will return PLCTAG_STATUS_OK.
 */

LIB_EXPORT int plc_tag_register_logger(void (*log_callback_func)(int32_t tag_id, int debug_level, const char *message)) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_LIB, 0, DEBUG_DETAIL, "Starting.");

    rc = debug_register_logger(log_callback_func);

    pdebug(DEBUG_MODULE_LIB, 0, DEBUG_DETAIL, "Done.");

    return rc;
}


/*
 * plc_tag_unregister_logger
 *
 * This function removes the logger callback already registered for the library.
 *
 * Return values:
 *
 * The function returns PLCTAG_STATUS_OK if there was a registered callback and removing it went well.
 * An error of PLCTAG_ERR_NOT_FOUND is returned if there was no registered callback.
 */

LIB_EXPORT int plc_tag_unregister_logger(void) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_LIB, 0, DEBUG_DETAIL, "Starting");

    rc = debug_unregister_logger();

    pdebug(DEBUG_MODULE_LIB, 0, DEBUG_DETAIL, "Done.");

    return rc;
}


/*
 * plc_tag_lock
 *
 * Lock the tag against use by other threads.  Because operations on a tag are
 * very much asynchronous, actions like getting and extracting the data from
 * a tag take more than one API call.  If more than one thread is using the same tag,
 * then the internal state of the tag will get broken and you will probably experience
 * a crash.
 *
 * This should be used to initially lock a tag when starting operations with it
 * followed by a call to plc_tag_unlock when you have everything you need from the tag.
 */

LIB_EXPORT int plc_tag_lock(int32_t id) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, id, "Starting.");

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* FIXME - there has to be a better way that this! */
    /* we cannot nest the mutexes otherwise we will deadlock. */
    do {
        critical_block(tag->api_mutex) { rc = mutex_try_lock(tag->ext_mutex); }

        /* if the mutex is already locked then we get a mutex lock error. */
        if(rc == PLCTAG_ERR_MUTEX_LOCK) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, id, "Mutex already locked, wait and retry.");
            sleep_ms(10);
        }
    } while(rc == PLCTAG_ERR_MUTEX_LOCK);

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, id, "External mutex locked.");
    } else {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Error %s trying to lock external mutex!", plc_tag_decode_error(rc));
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, id, "Done.");

    return rc;
}


/*
 * plc_tag_unlock
 *
 * The opposite action of plc_tag_unlock.  This allows other threads to access the
 * tag.
 */

LIB_EXPORT int plc_tag_unlock(int32_t id) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, id, "Starting.");

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) { rc = mutex_unlock(tag->ext_mutex); }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, id, "Done.");

    return rc;
}


/*
 * plc_tag_abort()
 *
 * This function calls through the vtable in the passed tag to call
 * the protocol-specific implementation.
 *
 * The implementation must do whatever is necessary to abort any
 * ongoing IO.
 *
 * The status of the operation is returned.
 */

LIB_EXPORT int plc_tag_abort(int32_t id) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, id, "Starting.");

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    rc = plc_tag_abort_impl(tag);

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, id, "Done.");

    return rc;
}


/*
 * plc_tag_destroy()
 *
 * Remove all implementation specific details about a tag and clear its
 * memory.
 */


/* Common tail of tag destruction, given a tag already found and already removed
 * from the tag hashtable. Factored out so plc_tag_shutdown()'s own tag-destroy
 * loop -- which already holds the instance and does its own hashtable_remove --
 * does not have to go through plc_tag_destroy()'s fresh tag_registry_acquire(),
 * which by that point in shutdown correctly (and unhelpfully) returns NULL: the
 * gate is deliberately closed before that loop runs, to stop new tags being
 * created, but tags already found by the loop are not "new". */
void destroy_tag_common(plc_tag_p tag) {
    int32_t tag_id = tag->tag_id;

    /* abort anything in flight */
    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "Aborting any in-flight operations.");

    plc_tag_abort_impl(tag);

    /*
     * Delivering DESTROYED releases the callback.  From inside a callback the delivery waits
     * for that callback to return.  A handler thread can still hold a reference and queue an
     * event after this; with no callback it is dropped.
     */
    critical_block(tag->api_mutex) {
        tag_event_push(tag, PLCTAG_EVENT_DESTROYED, PLCTAG_STATUS_OK);
        plc_tag_generic_handle_event_callbacks(tag);
    }

    /* release the reference outside the mutex. */
    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "rc_dec: Releasing reference to tag %" PRId32 " and tag mutex not locked.",
           tag_id);
    rc_dec(tag);

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag_id, "Done.");
}


LIB_EXPORT int plc_tag_destroy(int32_t tag_id) {
    plc_tag_p tag = NULL;
    tag_registry_p inst = NULL;

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag_id, "Starting.");

    if(tag_id <= 0 || tag_id > TAG_ID_MASK) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Called with zero or invalid tag!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* No tag in hand yet to borrow tag->instance from, so this is a per-call
     * acquire/release, same as lookup_tag(). */
    inst = tag_registry_acquire();
    if(!inst) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Library not running, cannot destroy tag.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    tag = remove_tag_lookup_by_id(inst, tag_id);
    rc_dec(inst);

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Called with non-existent tag!");
        return PLCTAG_ERR_NOT_FOUND;
    }

    destroy_tag_common(tag);

    return PLCTAG_STATUS_OK;
}


/*
 * Wait until status() stops answering PENDING, up to timeout ms.  Returns its last answer,
 * PLCTAG_ERR_TIMEOUT, or why the wait itself failed.
 */
static int wait_for_status(plc_tag_p tag, int timeout, int (*status)(plc_tag_p tag, uint32_t seq), uint32_t seq) {
    int rc = PLCTAG_STATUS_PENDING;
    int64_t end_time = time_ms() + timeout;

    do {
        int64_t timeout_left = end_time - time_ms();

        /* cond_wait() rejects a timeout that is not positive; that is the timeout itself. */
        if(timeout_left <= 0) {
            rc = PLCTAG_ERR_TIMEOUT;
            break;
        }

        if(timeout_left > INT_MAX) { timeout_left = 100; /* MAGIC, only wait 100ms in this weird case. */ }

        rc = cond_wait(tag->tag_cond_wait, (int)timeout_left);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Wait ended with %s.", plc_tag_decode_error(rc));
            break;
        }

        rc = status(tag, seq);
    } while(rc == PLCTAG_STATUS_PENDING);

    return (rc == PLCTAG_STATUS_PENDING) ? PLCTAG_ERR_TIMEOUT : rc;
}


/*
 * A creating tag's status, straight from its module: no tickler and no event dispatch, because a
 * create that fails removes the tag before the application has its id.
 */
static int create_status(plc_tag_p tag, uint32_t seq) {
    int rc = PLCTAG_ERR_NOT_IMPLEMENTED;

    (void)seq;

    critical_block(tag->api_mutex) {
        if(tag->vtable && tag->vtable->status) { rc = tag->vtable->status(tag); }
    }

    return rc;
}


/*
 * Op seq's result once it has ended, even if a callback has started the next op since.
 * ponytail: if a later op has also ended, that op's result is returned; keep a result per op
 * if that matters.
 */
static int op_status(plc_tag_p tag, uint32_t seq) {
    int rc = plc_tag_status_impl(tag);

    critical_block(tag->api_mutex) {
        if((int32_t)(tag->op_end_seq - seq) >= 0) { rc = tag->op_end_status; }
    }

    return rc;
}


/*
 * Wait for op seq to end, up to timeout ms.  A timeout aborts that op, never a later one.
 * Returns the op's result, or PLCTAG_ERR_TIMEOUT.
 */
static int wait_for_op(plc_tag_p tag, int timeout, uint32_t seq) {
    int64_t start_time = time_ms();
    int rc = wait_for_status(tag, timeout, op_status, seq);

    /*
     * The protocol layer may still be working on the request.  Without an abort the op stays in
     * flight after the call returns, and the next one fails with PLCTAG_ERR_BUSY.  Checked and
     * aborted under one lock, so the op cannot end and another start in between.
     */
    critical_block(tag->api_mutex) {
        if(tag->op_seq == seq && tag_op_busy(tag)) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "Operation still in flight after %s, aborting it.",
                   plc_tag_decode_error(rc));
            atomic_set_int64(&tag->read_cache_expire, 0);
            tag_op_abort(tag);
            if(tag->vtable && tag->vtable->abort) { tag->vtable->abort(tag); }
        } else if((int32_t)(tag->op_end_seq - seq) >= 0) {
            /* it ended as the wait timed out. */
            rc = tag->op_end_status;
        }
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag->tag_id, "elapsed time %" PRId64 "ms", (time_ms() - start_time));

    return rc;
}


/*
 * plc_tag_read()
 *
 * This function calls through the vtable in the passed tag to call
 * the protocol-specific implementation.  That starts the read operation.
 * If there is a timeout passed, then this routine waits for either
 * a timeout or an error.
 *
 * The status of the operation is returned.
 */

LIB_EXPORT int plc_tag_read(int32_t id, int timeout) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);
    bool is_done = false;
    uint32_t seq = 0;
    int64_t now = 0;

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, id, "Starting.");

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    if(timeout < 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Timeout must not be negative!");
        rc_dec(tag);
        return PLCTAG_ERR_BAD_PARAM;
    }

    now = time_ms();

    /*
     * Short cuts without the API mutex: the cache, op state and callback flag are atomic, and
     * the reference keeps the tag.  A cache hit on a tag with a callback has events to queue, so
     * it takes the mutex below.
     */
    if(atomic_get_int64(&tag->read_cache_expire) > now && !atomic_get_bool(&tag->has_callback)) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, id, "Returning cached data.");

        /* as the locked path does: a hit restarts the cache period. */
        atomic_set_int64(&tag->read_cache_expire, now + atomic_get_int64(&tag->read_cache_ms));
        rc_dec(tag);
        return PLCTAG_STATUS_OK;
    }

    if(tag_op_busy(tag) && atomic_get_int64(&tag->read_cache_expire) <= now) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "An operation is already in flight!");
        rc_dec(tag);
        return PLCTAG_ERR_BUSY;
    }

    critical_block(tag->api_mutex) {
        /* a cached read is a read that finished at once. */
        if(atomic_get_int64(&tag->read_cache_expire) > time_ms()) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, id, "Returning cached data.");
            tag_event_push(tag, PLCTAG_EVENT_READ_STARTED, PLCTAG_STATUS_OK);
            tag_event_push(tag, PLCTAG_EVENT_READ_COMPLETED, PLCTAG_STATUS_OK);
            rc = PLCTAG_STATUS_OK;
            is_done = true;
            break;
        }

        if(tag_op_busy(tag)) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "An operation is already in flight!");
            rc = PLCTAG_ERR_BUSY;
            is_done = true;
            break;
        }

        if(tag->tag_is_dirty) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag has locally updated data that will be overwritten!");
            rc = PLCTAG_ERR_BUSY;
            is_done = true;
            break;
        }

        tag->status = PLCTAG_STATUS_PENDING;

        /* cleared before the op starts, so an abort from the callback below is not missed. */
        cond_clear(tag->tag_cond_wait);

        tag_op_start(tag, true);
        seq = tag->op_seq;
        plc_tag_generic_handle_event_callbacks(tag);

        /* the callback aborted or destroyed the tag, so the read must not start. */
        if(!tag_op_busy(tag)) {
            rc = PLCTAG_ERR_ABORT;
            is_done = true;
            break;
        }

        /* the protocol implementation does not do the timeout. */
        if(tag->vtable && tag->vtable->read) {
            rc = tag->vtable->read(tag);
        } else {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Attempt to call read on a tag that does not support reads.");
            rc = PLCTAG_ERR_NOT_IMPLEMENTED;
        }

        if(rc != PLCTAG_STATUS_PENDING) {
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Response from read command returned error %s!", plc_tag_decode_error(rc));
            }

            tag_op_end(tag, rc);
            is_done = true;
        }
    }

    /* queued and woken only now: woken under the API mutex, the handler would find the tag locked. */
    if(!is_done && tag->vtable && tag->vtable->wake_plc) { tag->vtable->wake_plc(tag); }

    if(!is_done && timeout > 0) { rc = wait_for_op(tag, timeout, seq); }

    if(rc == PLCTAG_STATUS_OK) {
        /* set up the cache time.  This works when read_cache_ms is zero as it is already expired. */
        atomic_set_int64(&tag->read_cache_expire, time_ms() + atomic_get_int64(&tag->read_cache_ms));
    }

    plc_tag_generic_handle_event_callbacks(tag);

    rc_dec(tag);

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, id, "Done");

    return rc;
}


/*
 * plc_tag_status
 *
 * Return the current status of the tag.  This will be PLCTAG_STATUS_PENDING if there is
 * an uncompleted IO operation.  It will be PLCTAG_STATUS_OK if everything is fine.  Other
 * errors will be returned as appropriate.
 *
 * This is a function provided by the underlying protocol implementation.
 */

LIB_EXPORT int plc_tag_status(int32_t id) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, id, "Starting.");

    /* check the ID.  It might be an error status from creating the tag. */
    if(!tag) {
        if(id < 0) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Called with an error status %s!", plc_tag_decode_error(id));
            return id;
        } else {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag not found.");
            return PLCTAG_ERR_NOT_FOUND;
        }
    }

    /*
     * An op in flight is all a poll needs, and the op state is atomic, so this skips the API
     * mutex.  Nothing under it has work then: a kind's tickler only finishes a create, and no
     * op starts until it has, and the op's events are queued when it ends.
     */
    if(tag_op_busy(tag)) {
        rc = PLCTAG_STATUS_PENDING;
    } else {
        rc = plc_tag_status_impl(tag);
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, id, "Done with rc=%s.", plc_tag_decode_error(rc));

    return rc;
}


/*
 * plc_tag_write()
 *
 * This function calls through the vtable in the passed tag to call
 * the protocol-specific implementation.  That starts the write operation.
 * If there is a timeout passed, then this routine waits for either
 * a timeout or an error.
 *
 * The status of the operation is returned.
 */

LIB_EXPORT int plc_tag_write(int32_t id, int timeout) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);
    bool is_done = false;
    uint32_t seq = 0;

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, id, "Starting.");

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    if(timeout < 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Timeout must not be negative!");
        rc_dec(tag);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* an op in flight makes this BUSY without the API mutex: the op state is atomic, and the reference keeps the tag. */
    if(tag_op_busy(tag)) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag already has an operation in flight!");
        rc_dec(tag);
        return PLCTAG_ERR_BUSY;
    }

    critical_block(tag->api_mutex) {
        if(tag_op_busy(tag)) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag already has an operation in flight!");
            rc = PLCTAG_ERR_BUSY;
            is_done = true;
            break;
        }

        tag->status = PLCTAG_STATUS_OK;

        /* cleared before the op starts, so an abort from the callback below is not missed. */
        cond_clear(tag->tag_cond_wait);

        /* dispatched before the write starts, so the application can fill the buffer in it. */
        tag_op_start(tag, false);
        seq = tag->op_seq;
        plc_tag_generic_handle_event_callbacks(tag);

        /* the callback aborted or destroyed the tag, so the write must not start. */
        if(!tag_op_busy(tag)) {
            rc = PLCTAG_ERR_ABORT;
            is_done = true;
            break;
        }

        /* the protocol implementation does not do the timeout. */
        if(tag->vtable && tag->vtable->write) {
            rc = tag->vtable->write(tag);
        } else {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Attempt to call write on a tag that does not support writes.");
            rc = PLCTAG_ERR_NOT_IMPLEMENTED;
        }

        if(rc != PLCTAG_STATUS_PENDING) {
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Response from write command returned error %s!", plc_tag_decode_error(rc));
            }

            tag_op_end(tag, rc);
            is_done = true;
        }
    }

    /* queued and woken only now: woken under the API mutex, the handler would find the tag locked. */
    if(!is_done && tag->vtable && tag->vtable->wake_plc) { tag->vtable->wake_plc(tag); }

    if(!is_done && timeout > 0) { rc = wait_for_op(tag, timeout, seq); }

    plc_tag_generic_handle_event_callbacks(tag);

    rc_dec(tag);

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, id, "Done: status = %s.", plc_tag_decode_error(rc));

    return rc;
}


/*
 * Tag data accessors.
 */


LIB_EXPORT int plc_tag_get_int_attribute(int32_t id, const char *attrib_name, int default_value) {
    int res = default_value;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, id, "Starting.");

    /* FIXME - this should set the tag status if there is a tag. */
    if(!attrib_name || str_length(attrib_name) == 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Attribute name must not be null or zero-length!");
        return default_value;
    }

    /* get library attributes */
    if(id == 0) {
        const attr_def_t *def = attr_find_lib(attrib_name);

        if(def && def->type == ATTR_TYPE_INT && def->get_int) {
            int32_t value = 0;

            res = (def->get_int(NULL, &value) == PLCTAG_STATUS_OK ? (int)value : default_value);
        } else {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Attribute \"%s\" is not supported at the library level!", attrib_name);
            res = default_value;
        }
    } else {
        tag = lookup_tag(id);

        if(!tag) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag not found.");
            return default_value;
        }

        critical_block(tag->api_mutex) {
            const attr_def_t *def = attr_find(tag, attrib_name);

            if(def) {
                if(def->type != ATTR_TYPE_INT) {
                    pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Attribute \"%s\" does not hold an integer value!", attrib_name);
                    tag->status = PLCTAG_ERR_UNSUPPORTED;
                    res = default_value;
                } else if(def->get_int) {
                    int32_t value = 0;
                    int32_t rc = def->get_int(tag, &value);

                    tag->status = (int8_t)rc;
                    res = (rc == PLCTAG_STATUS_OK ? (int)value : default_value);
                } else {
                    /* the name exists but is write only, or the protocol suppressed it. */
                    tag->status = PLCTAG_ERR_UNSUPPORTED;
                    res = default_value;
                }
            } else {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Unsupported attribute \"%s\"!", attrib_name);
                res = default_value;
                tag->status = PLCTAG_ERR_UNSUPPORTED;
            }
        }

        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, id, "Done.");

    return res;
}


LIB_EXPORT int plc_tag_set_int_attribute(int32_t id, const char *attrib_name, int new_value) {
    int res = PLCTAG_ERR_NOT_FOUND;
    plc_tag_p tag = NULL;

    if(!attrib_name || str_length(attrib_name) == 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Attribute name must not be null or zero-length!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "Starting for int attribute %s.", attrib_name);

    /* set library attributes */
    if(id == 0) {
        const attr_def_t *def = attr_find_lib(attrib_name);

        if(!def || def->type != ATTR_TYPE_INT || !def->set_int) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Attribute \"%s\" is not supported at the library level!", attrib_name);
            return PLCTAG_ERR_UNSUPPORTED;
        }

        /* there is no tag at library scope, so this must not fall through to the rc_dec() below. */
        return (int)def->set_int(NULL, (int32_t)new_value);
    } else {
        tag = lookup_tag(id);

        if(!tag) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag not found.");
            return PLCTAG_ERR_NOT_FOUND;
        }

        critical_block(tag->api_mutex) {
            const attr_def_t *def = attr_find(tag, attrib_name);

            if(def) {
                if(def->type != ATTR_TYPE_INT) {
                    pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Attribute \"%s\" does not hold an integer value!", attrib_name);
                    res = PLCTAG_ERR_UNSUPPORTED;
                } else if(def->set_int) {
                    res = (int)def->set_int(tag, (int32_t)new_value);
                } else {
                    /* the name exists but is read only, or the protocol suppressed it. */
                    res = PLCTAG_ERR_UNSUPPORTED;
                }

                tag->status = (int8_t)res;
            } else {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Unsupported attribute \"%s\"!", attrib_name);
                res = PLCTAG_ERR_UNSUPPORTED;
                tag->status = (int8_t)res;
            }
        }
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, id, "Done.");

    return res;
}


LIB_EXPORT int plc_tag_get_byte_array_attribute(int32_t id, const char *attrib_name, uint8_t *buffer, int buffer_length) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "Starting.");

    if(!attrib_name || str_length(attrib_name) == 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Attribute name must not be null or zero-length!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(!buffer) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Host data buffer pointer must not be null!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(buffer_length <= 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Buffer length must not be negative or zero!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(id == 0) {
        const attr_def_t *def = attr_find_lib(attrib_name);

        if(def && (def->type == ATTR_TYPE_BYTES || def->type == ATTR_TYPE_STRING) && def->get_bytes) {
            return (int)def->get_bytes(NULL, buffer, (int32_t)buffer_length);
        }

        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Attribute \"%s\" is not supported at the library level!", attrib_name);

        return PLCTAG_ERR_UNSUPPORTED;
    }

    tag = lookup_tag(id);

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        const attr_def_t *def = attr_find(tag, attrib_name);

        if(def) {
            if(def->type != ATTR_TYPE_BYTES && def->type != ATTR_TYPE_STRING) {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Attribute \"%s\" does not hold a byte array value!", attrib_name);
                rc = PLCTAG_ERR_UNSUPPORTED;
            } else if(def->get_bytes) {
                rc = (int)def->get_bytes(tag, buffer, (int32_t)buffer_length);
            } else {
                /* the name exists but is write only, or the protocol suppressed it. */
                rc = PLCTAG_ERR_UNSUPPORTED;
            }
        } else {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Unsupported attribute \"%s\"!", attrib_name);
            rc = PLCTAG_ERR_UNSUPPORTED;
        }
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, id, "Done.");

    return rc;
}


LIB_EXPORT int plc_tag_get_attribute_size(int32_t id, const char *attrib_name) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "Starting.");

    if(!attrib_name || str_length(attrib_name) == 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Attribute name must not be null or zero-length!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(id == 0) {
        const attr_def_t *def = attr_find_lib(attrib_name);

        if(!def) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Attribute \"%s\" is not supported at the library level!", attrib_name);
            return PLCTAG_ERR_UNSUPPORTED;
        }

        if(def->type == ATTR_TYPE_INT) { return (int)sizeof(int32_t); }

        return (def->get_bytes_size ? (int)def->get_bytes_size(NULL) : PLCTAG_ERR_UNSUPPORTED);
    }

    tag = lookup_tag(id);

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        const attr_def_t *def = attr_find(tag, attrib_name);

        if(!def) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Attribute \"%s\" is not known to the library!", attrib_name);
            rc = PLCTAG_ERR_UNSUPPORTED;
        } else if(def->type != ATTR_TYPE_INT) {
            if(def->get_bytes_size) {
                rc = (int)def->get_bytes_size(tag);
            } else {
                rc = PLCTAG_ERR_UNSUPPORTED;
            }
        } else {
            rc = (int)sizeof(int32_t);
        }

        tag->status = (int8_t)(rc < 0 ? rc : PLCTAG_STATUS_OK);
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, id, "Done.");

    return rc;
}


LIB_EXPORT int plc_tag_get_size(int32_t id) {
    int result = 0;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, id, "Starting.");

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        result = tag->size;
        tag->status = PLCTAG_STATUS_OK;
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, id, "Done.");

    return result;
}


LIB_EXPORT int plc_tag_set_size(int32_t id, int new_size) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "Starting with new size %d.", new_size);

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    if(new_size < 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Illegal new size %d bytes for tag is illegal.  Tag size must be positive.");
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* the I/O thread uses the size while an op is in flight. */
    critical_block(tag->api_mutex) { rc = tag_op_busy(tag) ? PLCTAG_ERR_BUSY : resize_tag_buffer_unsafe(tag, new_size); }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    if(rc >= 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "Done with old size %d.", rc);
    } else {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag buffer resize failed with error %s!", plc_tag_decode_error(rc));
    }

    return rc;
}


static int plc_tag_get_bit_impl(plc_tag_p tag, int offset_bit) {
    int res = 0;
    int real_offset = 0;

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, tag->tag_id, "Starting.");

    do {
        if(!tag->data) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            res = PLCTAG_ERR_NO_DATA;
            break;
        }

        /* if this is a single bit, then make sure the offset is the tag bit. */
        if(tag->is_bit) {
            real_offset = tag->bit;
        } else {
            real_offset = offset_bit;
        }

        /* note: do not log tag->data[real_offset / 8] until the offset has been checked below. */
        pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, tag->tag_id, "selecting bit %d with offset %d in byte %d.", real_offset,
               (real_offset % 8), (real_offset / 8));

        if((real_offset >= 0) && ((real_offset / 8) < tag->size)) {
            res = !!(((1 << (real_offset % 8)) & 0xFF) & (tag->data[real_offset / 8]));
            tag->status = PLCTAG_STATUS_OK;
            break;
        } else {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "Data offset out of bounds!");
            res = PLCTAG_ERR_OUT_OF_BOUNDS;
            tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
            break;
        }
    } while(0);

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, tag->tag_id, "Done.");

    return res;
}


LIB_EXPORT int plc_tag_get_bit(int32_t id, int offset_bit) {
    int res = PLCTAG_ERR_OUT_OF_BOUNDS;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, id, "Starting.");

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) { res = plc_tag_get_bit_impl(tag, offset_bit); }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return res;
}

/*
 * tag_set_dirty - mark a tag as having locally-modified data and notify
 * the protocol layer so its handler can schedule a write.
 * Must be called while tag->api_mutex is held.
 */
static void tag_set_dirty(plc_tag_p tag) {
    tag->tag_is_dirty = 1;
    if(tag->vtable && tag->vtable->tag_data_written) { tag->vtable->tag_data_written(tag); }
}


static int plc_tag_set_bit_impl(plc_tag_p tag, int offset_bit, int val) {
    int res = PLCTAG_STATUS_OK;
    int real_offset = offset_bit;

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, tag->tag_id, "Starting.");

    do {
        if(!tag->data) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            res = PLCTAG_ERR_NO_DATA;
            break;
        }

        /* if this is a single bit, then make sure the offset is the tag bit. */
        if(tag->is_bit) {
            real_offset = tag->bit;
        } else {
            real_offset = offset_bit;
        }

        /* note: do not log tag->data[real_offset / 8] until the offset has been checked below. */
        pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, tag->tag_id, "Setting bit %d with offset %d in byte %d.", real_offset,
               (real_offset % 8), (real_offset / 8));

        if((real_offset >= 0) && ((real_offset / 8) < tag->size)) {
            if(tag->auto_sync_write_ms > 0) { tag_set_dirty(tag); }

            if(val) {
                tag->data[real_offset / 8] |= (uint8_t)(1 << (real_offset % 8));
            } else {
                tag->data[real_offset / 8] &= (uint8_t)(~(1 << (real_offset % 8)));
            }

            tag->status = PLCTAG_STATUS_OK;
        } else {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "Data offset out of bounds!");
            tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
            res = PLCTAG_ERR_OUT_OF_BOUNDS;
            break;
        }
    } while(0);

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, tag->tag_id, "Done.");

    return res;
}


LIB_EXPORT int plc_tag_set_bit(int32_t id, int offset_bit, int val) {
    int res = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, id, "Starting.");

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) { res = plc_tag_set_bit_impl(tag, offset_bit, val); }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return res;
}


/*
 * The integer accessor families.
 *
 * plc_tag_get_uint8()/int8()/uint16()/.../int64() and their setters all do the same
 * thing: look the tag up, take its mutex, check that it has data and that the field is
 * in bounds, then move `width` bytes through the byte-order map for that width.  Only
 * the width, the signedness of the result and the failure sentinel differ, so the whole
 * body lives here once and each public function is a cast around it.
 *
 * Bit tags are a separate case that these still have to carry: when tag->is_bit is set
 * the tag holds a single bit, and a get returns it while a set writes it, ignoring
 * `offset` and the width entirely.
 *
 * Values move as uint64_t.  A signed accessor casts on the way out, which is
 * implementation-defined before C23 for out-of-range values but well defined for the
 * two's-complement round trip these perform.
 */

/* Pick the byte-order map for a width.  A single byte has no order to apply. */
static const int *byte_order_for_width(plc_tag_p tag, int width, bool is_float) {
    switch(width) {
        case 1: return NULL;
        case 2: return tag->byte_order->int16_order;
        case 4: return (is_float ? tag->byte_order->float32_order : tag->byte_order->int32_order);
        case 8: return (is_float ? tag->byte_order->float64_order : tag->byte_order->int64_order);
        default: return NULL;
    }
}


/*
 * Shared body of every integer and float getter.  On success *out holds the raw value
 * zero-extended to 64 bits and PLCTAG_STATUS_OK is returned; on any failure the caller
 * substitutes its own sentinel.  A float uses the float byte order and has no bit form.
 */
static int tag_get_data_common(int32_t id, int offset, int width, bool is_float, uint64_t *out) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, id, "Starting.");

    *out = 0;

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        if(!tag->data) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            rc = PLCTAG_ERR_NO_DATA;
            break;
        }

        if(tag->is_bit && is_float) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Getting a float value is unsupported on a bit tag!");
            tag->status = PLCTAG_ERR_UNSUPPORTED;
            rc = PLCTAG_ERR_UNSUPPORTED;
            break;
        }

        if(tag->is_bit) {
            int bit_rc = plc_tag_get_bit_impl(tag, tag->bit);

            if(bit_rc < 0) {
                rc = bit_rc;
            } else {
                *out = (uint64_t)(unsigned int)bit_rc;
            }

            break;
        }

        if(!tag_range_is_valid(tag, offset, width)) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Data offset out of bounds!");
            tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            break;
        }

        const int *order = byte_order_for_width(tag, width, is_float);

        for(int i = 0; i < width; i++) {
            int src = order ? (offset + order[i]) : offset;

            *out += ((uint64_t)(tag->data[src])) << (i * 8);
        }

        tag->status = PLCTAG_STATUS_OK;
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return rc;
}


/* Shared body of every integer and float setter.  `val` holds the raw value zero-extended to 64 bits. */
static int tag_set_data_common(int32_t id, int offset, int width, bool is_float, uint64_t val) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, id, "Starting.");

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        if(!tag->data) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            rc = PLCTAG_ERR_NO_DATA;
            break;
        }

        if(tag->is_bit && is_float) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Setting a float value is unsupported on a bit tag!");
            tag->status = PLCTAG_ERR_UNSUPPORTED;
            rc = PLCTAG_ERR_UNSUPPORTED;
            break;
        }

        if(tag->is_bit) {
            rc = plc_tag_set_bit_impl(tag, 0, val ? 1 : 0);
            break;
        }

        if(!tag_range_is_valid(tag, offset, width)) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Data offset out of bounds!");
            tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            break;
        }

        if(tag->auto_sync_write_ms > 0) { tag_set_dirty(tag); }

        const int *order = byte_order_for_width(tag, width, is_float);

        for(int i = 0; i < width; i++) {
            int dest = order ? (offset + order[i]) : offset;

            tag->data[dest] = (uint8_t)((val >> (i * 8)) & 0xFF);
        }

        tag->status = PLCTAG_STATUS_OK;
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return rc;
}


LIB_EXPORT uint64_t plc_tag_get_uint64(int32_t id, int offset) {
    uint64_t raw = 0;

    if(tag_get_data_common(id, offset, (int)sizeof(uint64_t), false, &raw) != PLCTAG_STATUS_OK) { return UINT64_MAX; }

    return (uint64_t)raw;
}


LIB_EXPORT int plc_tag_set_uint64(int32_t id, int offset, uint64_t val) {
    return tag_set_data_common(id, offset, (int)sizeof(uint64_t), false, (uint64_t)val);
}


LIB_EXPORT int64_t plc_tag_get_int64(int32_t id, int offset) {
    uint64_t raw = 0;

    if(tag_get_data_common(id, offset, (int)sizeof(int64_t), false, &raw) != PLCTAG_STATUS_OK) { return INT64_MIN; }

    return (int64_t)raw;
}


LIB_EXPORT int plc_tag_set_int64(int32_t id, int offset, int64_t ival) {
    return tag_set_data_common(id, offset, (int)sizeof(int64_t), false, (uint64_t)(uint64_t)ival);
}


LIB_EXPORT uint32_t plc_tag_get_uint32(int32_t id, int offset) {
    uint64_t raw = 0;

    if(tag_get_data_common(id, offset, (int)sizeof(uint32_t), false, &raw) != PLCTAG_STATUS_OK) { return UINT32_MAX; }

    return (uint32_t)raw;
}

LIB_EXPORT int plc_tag_set_uint32(int32_t id, int offset, uint32_t val) {
    return tag_set_data_common(id, offset, (int)sizeof(uint32_t), false, (uint64_t)val);
}

LIB_EXPORT int32_t plc_tag_get_int32(int32_t id, int offset) {
    uint64_t raw = 0;

    if(tag_get_data_common(id, offset, (int)sizeof(int32_t), false, &raw) != PLCTAG_STATUS_OK) { return INT32_MIN; }

    return (int32_t)raw;
}

LIB_EXPORT int plc_tag_set_int32(int32_t id, int offset, int32_t ival) {
    return tag_set_data_common(id, offset, (int)sizeof(int32_t), false, (uint64_t)(uint32_t)ival);
}

LIB_EXPORT uint16_t plc_tag_get_uint16(int32_t id, int offset) {
    uint64_t raw = 0;

    if(tag_get_data_common(id, offset, (int)sizeof(uint16_t), false, &raw) != PLCTAG_STATUS_OK) { return UINT16_MAX; }

    return (uint16_t)raw;
}


LIB_EXPORT int plc_tag_set_uint16(int32_t id, int offset, uint16_t val) {
    return tag_set_data_common(id, offset, (int)sizeof(uint16_t), false, (uint64_t)val);
}


LIB_EXPORT int16_t plc_tag_get_int16(int32_t id, int offset) {
    uint64_t raw = 0;

    if(tag_get_data_common(id, offset, (int)sizeof(int16_t), false, &raw) != PLCTAG_STATUS_OK) { return INT16_MIN; }

    return (int16_t)raw;
}


LIB_EXPORT int plc_tag_set_int16(int32_t id, int offset, int16_t ival) {
    return tag_set_data_common(id, offset, (int)sizeof(int16_t), false, (uint64_t)(uint16_t)ival);
}


LIB_EXPORT uint8_t plc_tag_get_uint8(int32_t id, int offset) {
    uint64_t raw = 0;

    if(tag_get_data_common(id, offset, (int)sizeof(uint8_t), false, &raw) != PLCTAG_STATUS_OK) { return UINT8_MAX; }

    return (uint8_t)raw;
}


LIB_EXPORT int plc_tag_set_uint8(int32_t id, int offset, uint8_t val) {
    return tag_set_data_common(id, offset, (int)sizeof(uint8_t), false, (uint64_t)val);
}


LIB_EXPORT int8_t plc_tag_get_int8(int32_t id, int offset) {
    uint64_t raw = 0;

    if(tag_get_data_common(id, offset, (int)sizeof(int8_t), false, &raw) != PLCTAG_STATUS_OK) { return INT8_MIN; }

    return (int8_t)raw;
}


LIB_EXPORT int plc_tag_set_int8(int32_t id, int offset, int8_t ival) {
    return tag_set_data_common(id, offset, (int)sizeof(int8_t), false, (uint64_t)(uint8_t)ival);
}

LIB_EXPORT double plc_tag_get_float64(int32_t id, int offset) {
    uint64_t raw = 0;
    double res = DBL_MIN;

    if(tag_get_data_common(id, offset, (int)sizeof(double), true, &raw) != PLCTAG_STATUS_OK) { return DBL_MIN; }

    mem_copy(&res, &raw, sizeof(res));

    return res;
}


LIB_EXPORT int plc_tag_set_float64(int32_t id, int offset, double fval) {
    uint64_t raw = 0;

    mem_copy(&raw, &fval, sizeof(raw));

    return tag_set_data_common(id, offset, (int)sizeof(double), true, raw);
}


LIB_EXPORT float plc_tag_get_float32(int32_t id, int offset) {
    uint64_t raw = 0;
    uint32_t raw32 = 0;
    float res = FLT_MIN;

    if(tag_get_data_common(id, offset, (int)sizeof(float), true, &raw) != PLCTAG_STATUS_OK) { return FLT_MIN; }

    raw32 = (uint32_t)raw;
    mem_copy(&res, &raw32, sizeof(res));

    return res;
}


LIB_EXPORT int plc_tag_set_float32(int32_t id, int offset, float fval) {
    uint32_t raw32 = 0;

    mem_copy(&raw32, &fval, sizeof(raw32));

    return tag_set_data_common(id, offset, (int)sizeof(float), true, (uint64_t)raw32);
}


LIB_EXPORT int plc_tag_get_string(int32_t tag_id, int string_start_offset, char *buffer, int buffer_length) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(tag_id);
    int max_len = 0;

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, tag_id, "Starting.");

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* are strings defined for this tag? */
    if(!tag->byte_order || !tag->byte_order->str_is_defined) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Tag has no definitions for strings!");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Tag has no data!");
        tag->status = PLCTAG_ERR_NO_DATA;
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        return PLCTAG_ERR_NO_DATA;
    }

    if(tag->is_bit) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Getting a string value from a bit tag is not supported!");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    if(!buffer || buffer_length < 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Buffer is null or has a negative length!");
        tag->status = PLCTAG_ERR_BAD_PARAM;
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* set all buffer bytes to zero. */
    mem_set(buffer, 0, buffer_length);

    critical_block(tag->api_mutex) {
        int string_length = get_string_length_unsafe(tag, string_start_offset);

        /* a bad start offset shows up as a negative length here.  Do not treat it as a count. */
        if(string_length < 0) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Unable to get the string length, error %s!",
                   plc_tag_decode_error(string_length));
            tag->status = (int8_t)string_length;
            rc = string_length;
            break;
        }

        /* determine the maximum number of characters/bytes to copy. */
        if(buffer_length < string_length) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Buffer length, %d, is less than the string length, %d!", buffer_length,
                   string_length);
            max_len = buffer_length;
        } else {
            max_len = string_length;
        }

        /* check the amount of space. */
        if(string_start_offset + (int)tag->byte_order->str_count_word_bytes + max_len <= tag->size) {
            for(int i = 0; i < max_len && i < tag->size; i++) {
                size_t char_index =
                    (((size_t)(unsigned int)i) ^ (tag->byte_order->str_is_byte_swapped)) /* byte swap if necessary */
                    + (size_t)(unsigned int)string_start_offset + (size_t)(unsigned int)(tag->byte_order->str_count_word_bytes);

                if(char_index < (size_t)tag->size) {
                    buffer[i] = (char)tag->data[char_index];
                } else {
                    pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Out of bounds index, %zu, generated!", char_index);
                    rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                    break;
                }
            }

            if(rc != PLCTAG_STATUS_OK) { break; }

            tag->status = PLCTAG_STATUS_OK;
            rc = PLCTAG_STATUS_OK;
        } else {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Data offset out of bounds!");
            tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
        }
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, tag_id, "Done.");

    return rc;
}


LIB_EXPORT int plc_tag_set_string(int32_t tag_id, int string_start_offset, const char *string_val) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(tag_id);
    unsigned int string_length = 0;
    unsigned int string_data_start_offset = (unsigned int)string_start_offset;

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "Starting with string %s.", string_val);

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* are strings defined for this tag? */
    if(!tag->byte_order || !tag->byte_order->str_is_defined) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Tag has no definitions for strings!");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    if(!string_val) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "New string value pointer is null!");
        tag->status = PLCTAG_ERR_NULL_PTR;
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        return PLCTAG_ERR_NULL_PTR;
    }

    /* note that passing a zero-length string is valid. */

    if(tag->is_bit) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Setting a string value on a bit tag is not supported!");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    string_length = (unsigned int)str_length(string_val);

    /* will the string fit in the space on the PLC?  If we have a max capacity we check. */
    if(tag->byte_order->str_max_capacity && string_length > tag->byte_order->str_max_capacity) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "String is longer, %u bytes, than the maximum capacity, %u!", string_length,
               tag->byte_order->str_max_capacity);
        rc = PLCTAG_ERR_TOO_LARGE;
        tag->status = (int8_t)rc;
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        return rc;
    }

    /* we may be changing things, so take the mutex */
    critical_block(tag->api_mutex) {
        int old_string_size_in_buffer = 0;
        int new_string_size_in_buffer = 0;

        old_string_size_in_buffer = get_string_total_length_unsafe(tag, string_start_offset);
        if(old_string_size_in_buffer < 0) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Error getting existing string size in the tag buffer!");
            rc = old_string_size_in_buffer;
            break;
        }

        new_string_size_in_buffer = get_new_string_total_length_unsafe(tag, string_val);
        if(new_string_size_in_buffer < 0) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Error getting new string size!");
            rc = new_string_size_in_buffer;
            break;
        }

        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id,
               "allow_field_resize=%d, old_string_size_in_buffer=%" PRId32 ", new_string_size_in_buffer=%" PRId32 ".",
               tag->allow_field_resize, old_string_size_in_buffer, new_string_size_in_buffer);

        if(!tag->allow_field_resize && (new_string_size_in_buffer != old_string_size_in_buffer)) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "This tag does not allow resizing of fields.");
            rc = PLCTAG_ERR_NOT_ALLOWED;
            break;
        }

        rc = resize_tag_buffer_at_offset_unsafe(tag, string_start_offset + old_string_size_in_buffer,
                                                string_start_offset + new_string_size_in_buffer);
        if(rc != PLCTAG_STATUS_OK) { break; }

        /* zero out the string data in the buffer. */
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "Zeroing out the string data in the buffer.");
        for(unsigned int i = (unsigned int)string_start_offset;
            i < (unsigned int)(string_start_offset + new_string_size_in_buffer) && i < (unsigned int)tag->size; i++) {
            tag->data[i] = 0;
        }

        /* if the string is counted, set the length */
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "Set count word if the string is counted.");
        if(tag->byte_order->str_is_counted) {
            int last_count_word_index = string_start_offset + (int)(unsigned int)tag->byte_order->str_count_word_bytes;

            if(last_count_word_index > (int)(tag->size)) {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id,
                       "Unable to write valid count word as count word would go past the end of the tag buffer!");
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                tag->status = (int8_t)rc;
                break;
            }

            /* move the index of the string data start past the count word. */
            string_data_start_offset += tag->byte_order->str_count_word_bytes;

            switch(tag->byte_order->str_count_word_bytes) {
                case 1:
                    if(string_length > UINT8_MAX) {
                        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id,
                               "String length, %u, is greater than can be expressed in a one-byte count word!", string_length);
                        rc = PLCTAG_ERR_TOO_LARGE;
                        break;
                    }

                    tag->data[string_start_offset] = (uint8_t)(unsigned int)string_length;
                    break;

                case 2:
                    if(string_length > UINT16_MAX) {
                        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id,
                               "String length, %u, is greater than can be expressed in a two-byte count word!", string_length);
                        rc = PLCTAG_ERR_TOO_LARGE;
                        break;
                    }

                    tag->data[string_start_offset + tag->byte_order->int16_order[0]] =
                        (uint8_t)((((unsigned int)string_length) >> 0) & 0xFF);
                    tag->data[string_start_offset + tag->byte_order->int16_order[1]] =
                        (uint8_t)((((unsigned int)string_length) >> 8) & 0xFF);
                    break;

                case 4:
                    if(string_length > UINT32_MAX) {
                        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id,
                               "String length, %u, is greater than can be expressed in a four-byte count word!", string_length);
                        rc = PLCTAG_ERR_TOO_LARGE;
                        break;
                    }

                    tag->data[string_start_offset + tag->byte_order->int32_order[0]] =
                        (uint8_t)((((unsigned int)string_length) >> 0) & 0xFF);
                    tag->data[string_start_offset + tag->byte_order->int32_order[1]] =
                        (uint8_t)((((unsigned int)string_length) >> 8) & 0xFF);
                    tag->data[string_start_offset + tag->byte_order->int32_order[2]] =
                        (uint8_t)((((unsigned int)string_length) >> 16) & 0xFF);
                    tag->data[string_start_offset + tag->byte_order->int32_order[3]] =
                        (uint8_t)((((unsigned int)string_length) >> 24) & 0xFF);
                    break;

                default:
                    pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Unsupported string count size, %d!",
                           tag->byte_order->str_count_word_bytes);
                    rc = PLCTAG_ERR_UNSUPPORTED;
                    tag->status = (int8_t)rc;
                    break;
            }
        }

        /* if status is bad, punt out of the critical block */
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Error %s (%d) trying to set the count word!", plc_tag_decode_error(rc),
                   rc);
            tag->status = (int8_t)rc;
            break;
        }

        /* copy the string data into the tag. */
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "Copying %u bytes of the string into the tag data buffer.", string_length);
        for(unsigned int i = 0; i < string_length; i++) {
            size_t char_index = 0;

            if(tag->byte_order->str_is_byte_swapped) {
                char_index = string_data_start_offset + ((i & 0x01) ? i - 1 : i + 1);
            } else {
                char_index = string_data_start_offset + i;
            }

            if(char_index < (size_t)(uint32_t)tag->size) {
                tag->data[char_index] = (uint8_t)string_val[i];
            } else {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id,
                       "Out of bounds index, %zu, generated during string copy!  Tag size is %" PRId32 ".", char_index,
                       tag->size);
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;

                /* note: only breaks out of the for loop, we need another break. */
                break;
            }
        }

        /* break out of the critical block if bad status. */
        if(rc != PLCTAG_STATUS_OK) {
            tag->status = (int8_t)rc;
            break;
        }

        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "If string is nul terminated we need to set the termination byte.");
        if(tag->byte_order->str_is_zero_terminated) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "Setting the nul termination byte.");

            if(string_data_start_offset + string_length < (unsigned int)tag->size) {
                tag->data[string_data_start_offset + string_length] = (uint8_t)0;
            } else {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id,
                       "Index of nul termination byte, %u, is outside of the tag data of %u bytes!",
                       string_data_start_offset + string_length, tag->size);
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                break;
            }
        }

        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "String data in buffer:");
        pdebug_dump_bytes(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, tag->data + string_start_offset, new_string_size_in_buffer);

        /* if this is an auto-write tag, mark dirty and notify the protocol layer */
        if(rc == PLCTAG_STATUS_OK && tag->auto_sync_write_ms > 0) { tag_set_dirty(tag); }

        /* set the return and tag status. */
        rc = PLCTAG_STATUS_OK;
        tag->status = (int8_t)rc;
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "Done with status %s (%d).", plc_tag_decode_error(rc), rc);

    return rc;
}


LIB_EXPORT int plc_tag_get_string_capacity(int32_t id, int string_start_offset) {
    int string_capacity = 0;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, id, "Starting.");

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* are strings defined for this tag? */
    if(!tag->byte_order || !tag->byte_order->str_is_defined) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag has no definitions for strings!");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag has no data!");
        tag->status = PLCTAG_ERR_NO_DATA;
        return PLCTAG_ERR_NO_DATA;
    }

    if(tag->is_bit) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Getting string capacity from a bit tag is not supported!");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* FIXME - there is no capacity that is valid if it str_max_capacity is not set.  Should return 0 or an error. */
    critical_block(tag->api_mutex) {
        string_capacity = (tag->byte_order->str_max_capacity ? (int)(tag->byte_order->str_max_capacity) :
                                                               get_string_length_unsafe(tag, string_start_offset));
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, id, "Done.");

    return string_capacity;
}


LIB_EXPORT int plc_tag_get_string_length(int32_t id, int string_start_offset) {
    int string_length = 0;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, id, "Starting.");

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* are strings defined for this tag? */
    if(!tag->byte_order || !tag->byte_order->str_is_defined) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag has no definitions for strings!");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag has no data!");
        tag->status = PLCTAG_ERR_NO_DATA;
        return PLCTAG_ERR_NO_DATA;
    }

    if(tag->is_bit) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Getting string length from a bit tag is not supported!");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        return PLCTAG_ERR_UNSUPPORTED;
    }

    critical_block(tag->api_mutex) { string_length = get_string_length_unsafe(tag, string_start_offset); }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, id, "Done.");

    return string_length;
}


LIB_EXPORT int plc_tag_get_string_total_length(int32_t id, int string_start_offset) {
    int total_length = 0;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, id, "Starting.");

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* are strings defined for this tag? */
    if(!tag->byte_order || !tag->byte_order->str_is_defined) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag has no definitions for strings!");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag has no data!");
        tag->status = PLCTAG_ERR_NO_DATA;
        return PLCTAG_ERR_NO_DATA;
    }

    if(tag->is_bit) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Getting a string total length from a bit tag is not supported!");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* FIXME - what about byte swapping?  If the string length is not even, what happens? */
    critical_block(tag->api_mutex) { total_length = get_string_total_length_unsafe(tag, string_start_offset); }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, id, "Done.");

    return total_length;
}


LIB_EXPORT int plc_tag_set_raw_bytes(int32_t id, int offset, uint8_t *buffer, int buffer_size) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, id, "Starting.");

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag has no data!");
        tag->status = PLCTAG_ERR_NO_DATA;
        return PLCTAG_ERR_NO_DATA;
    }

    if(!buffer) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Buffer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(buffer_size <= 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "The buffer must have some capacity for data.");
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(!tag->is_bit) {
        critical_block(tag->api_mutex) {
            if(tag_range_is_valid(tag, offset, buffer_size)) {
                if(tag->auto_sync_write_ms > 0) { tag_set_dirty(tag); }

                int i;
                for(i = 0; i < buffer_size; i++) { tag->data[offset + i] = buffer[i]; }

                tag->status = PLCTAG_STATUS_OK;
            } else {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Data offset out of bounds!");
                tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            }
        }
    } else {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Trying to write a list of values on a Tag bit.");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        rc = PLCTAG_ERR_UNSUPPORTED;
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return rc;
}


LIB_EXPORT int plc_tag_get_raw_bytes(int32_t id, int offset, uint8_t *buffer, int buffer_size) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, id, "Starting.");

    if(!tag) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Tag has no data!");
        tag->status = PLCTAG_ERR_NO_DATA;
        return PLCTAG_ERR_NO_DATA;
    }

    if(!buffer) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Buffer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(buffer_size <= 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "The buffer must have some capacity for data.");
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(!tag->is_bit) {
        critical_block(tag->api_mutex) {
            if(tag_range_is_valid(tag, offset, buffer_size)) {
                int i;
                for(i = 0; i < buffer_size; i++) { buffer[i] = tag->data[offset + i]; }

                tag->status = PLCTAG_STATUS_OK;
            } else {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Data offset out of bounds!");
                tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            }
        }
    } else {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, id, "Trying to read a list of values from a Tag bit.");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        rc = PLCTAG_ERR_UNSUPPORTED;
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return rc;
}
