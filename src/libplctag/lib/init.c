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

#include <libplctag/lib/init.h>
#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/protocols/ab/ab.h>
#include <libplctag/protocols/mb/modbus.h>
#include <libplctag/protocols/omron/omron.h>
#include <libplctag/protocols/system/system.h>
#include <utils/rc.h>
#include <platform.h>
#include <stdlib.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/atomic_utils.h>


/*
 * The following maps attributes to the tag creation functions.
 */


struct {
    const char *protocol;
    const char *make;
    const char *family;
    const char *model;
    const tag_create_function tag_constructor;
} tag_type_map[] = {
    /* System tags */
    {.protocol = NULL, .make = "system", .family = "library", .model = NULL, .tag_constructor = system_tag_create},
    /* Allen-Bradley PLCs */
    {.protocol = "ab-eip", .make = NULL, .family = NULL, .model = NULL, .tag_constructor = ab_tag_create},
    {.protocol = "ab_eip", .make = NULL, .family = NULL, .model = NULL, .tag_constructor = ab_tag_create},
    {.protocol = "modbus-tcp", .make = NULL, .family = NULL, .model = NULL, .tag_constructor = mb_tag_create},
    {.protocol = "modbus_tcp", .make = NULL, .family = NULL, .model = NULL, .tag_constructor = mb_tag_create}};

/* Library state machine */
#define LIB_STATE_UNINITIALIZED ((int32_t)0)
#define LIB_STATE_INITIALIZING ((int32_t)1)
#define LIB_STATE_RUNNING ((int32_t)2)
#define LIB_STATE_SHUTTING_DOWN ((int32_t)3)

static atomic_int32_t library_state = ATOMIC_INT_STATIC_INIT;


/*
 * find_tag_create_func()
 *
 * Find an appropriate tag creation function.  This scans through the array
 * above to find a matching tag creation type.  The first match is returned.
 * A passed set of options will match when all non-null entries in the list
 * match.  This means that matches must be ordered from most to least general.
 *
 * Note that the protocol is used if it exists otherwise, the make family and
 * model will be used.
 */

tag_create_function find_tag_create_func(attr attributes) {
    int i = 0;
    const char *protocol = attr_get_str(attributes, "protocol", NULL);
    const char *make = attr_get_str(attributes, "make", attr_get_str(attributes, "manufacturer", NULL));
    const char *family = attr_get_str(attributes, "family", NULL);
    const char *model = attr_get_str(attributes, "model", NULL);
    int num_entries = (sizeof(tag_type_map) / sizeof(tag_type_map[0]));

    /* if protocol is set, then use it to match. */
    if(protocol && str_length(protocol) > 0) {
        for(i = 0; i < num_entries; i++) {
            if(tag_type_map[i].protocol && str_cmp(tag_type_map[i].protocol, protocol) == 0) {
                pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Matched protocol=%s", protocol);
                return tag_type_map[i].tag_constructor;
            }
        }
    } else {
        /* match make/family/model */
        for(i = 0; i < num_entries; i++) {
            if(tag_type_map[i].make && make && str_cmp_i(tag_type_map[i].make, make) == 0) {
                pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Matched make=%s", make);
                if(tag_type_map[i].family) {
                    if(family && str_cmp_i(tag_type_map[i].family, family) == 0) {
                        pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Matched make=%s family=%s", make, family);
                        if(tag_type_map[i].model) {
                            if(model && str_cmp_i(tag_type_map[i].model, model) == 0) {
                                pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Matched make=%s family=%s model=%s", make, family,
                                       model);
                                return tag_type_map[i].tag_constructor;
                            }
                        } else {
                            /* matches until a NULL */
                            pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Matched make=%s family=%s model=NULL", make, family);
                            return tag_type_map[i].tag_constructor;
                        }
                    }
                } else {
                    /* matched until a NULL, so we matched */
                    pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Matched make=%s family=NULL model=NULL", make);
                    return tag_type_map[i].tag_constructor;
                }
            }
        }
    }

    /* no match */
    return NULL;
}


/*
 * destroy_modules() is called when the main process exits.
 *
 * Modify this for any PLC/protocol that needs to have something
 * torn down at the end.
 */

void destroy_modules(void) {
    int32_t old_state;

    pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Starting.");

    /*
     * Try to transition from RUNNING to SHUTTING_DOWN.
     * atomic_compare_and_set_int32() returns the old value.
     * If it returns RUNNING, the swap succeeded.
     */
    old_state = atomic_compare_and_set_int32(&library_state, LIB_STATE_RUNNING, LIB_STATE_SHUTTING_DOWN);
    if(old_state != LIB_STATE_RUNNING) {
        pdebug(DEBUG_MODULE_INIT, DEBUG_WARN, 0, "Cannot shutdown - library state is %" PRId32 ", not RUNNING.", old_state);
        return;
    }

    pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Tearing down AB module.");
    ab_teardown();

    pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Tearing down Modbus module.");
    mb_teardown();

    pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Tearing down Omron module.");
    omron_teardown();

    pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Tearing down library module.");
    lib_teardown();

    /* last so that we continue to process deferred destructors until the end. */
    pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Tearing down refcount infrastructure.");
    refcount_teardown();

    pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Unregistering logger.");
    plc_tag_unregister_logger();

    pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Flushing debug output.");
    debug_flush();

    /* Mark as uninitialized - ready for potential re-initialization */
    atomic_set_int32(&library_state, LIB_STATE_UNINITIALIZED);

    pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Done.");
}


/*
 * initialize_modules() is called the first time any kind of tag is
 * created.  It will be called before the tag creation routines are
 * run.
 */


int initialize_modules(void) {
    int rc = PLCTAG_STATUS_OK;
    int32_t old_state;

    pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Starting.");

    /* Fast path: already running */
    if(atomic_get_int32(&library_state) == LIB_STATE_RUNNING) {
        pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Library already initialized, returning.");
        return PLCTAG_STATUS_OK;
    }

    /*
     * Try to transition from UNINITIALIZED to INITIALIZING.
     * Only one thread can win this race.
     *
     * atomic_compare_and_set_int32() returns the old value.
     * If it returns UNINITIALIZED, the swap succeeded.
     */
    while((old_state = atomic_compare_and_set_int32(&library_state, LIB_STATE_UNINITIALIZED, LIB_STATE_INITIALIZING))
          != LIB_STATE_UNINITIALIZED) {

        switch(old_state) {
            case LIB_STATE_RUNNING:
                /* Another thread finished initialization */
                pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Library initialized by another thread.");
                return PLCTAG_STATUS_OK;

            case LIB_STATE_INITIALIZING:
                /* Another thread is initializing, wait for it */
                pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Waiting for another thread to complete initialization...");
                sleep_ms(10);
                break;

            case LIB_STATE_SHUTTING_DOWN:
                /* Shutdown in progress, wait for it to complete */
                pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Waiting for library shutdown to complete...");
                sleep_ms(10);
                break;

            default:
                pdebug(DEBUG_MODULE_INIT, DEBUG_ERROR, 0, "Unknown library state %" PRId32 "!", old_state);
                return PLCTAG_ERR_BAD_STATUS;
        }
    }

    /* We won the CAS - we are now responsible for initialization */
    pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "This thread will initialize the library.");

    /* initialize a random seed value. */
    srand((unsigned int)time_ms());

    /* Start the refcount cleanup thread first */
    pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Starting refcount cleanup infrastructure.");
    rc = refcount_startup();
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_INIT, DEBUG_ERROR, 0, "Unable to start refcount cleanup infrastructure!");
        atomic_set_int32(&library_state, LIB_STATE_UNINITIALIZED);
        return rc;
    }

    pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Initializing library modules.");
    rc = lib_init();
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_INIT, DEBUG_ERROR, 0, "Unable to initialize library module!");
        atomic_set_int32(&library_state, LIB_STATE_UNINITIALIZED);
        return rc;
    }

    pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Initializing AB module.");
    rc = ab_init();
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_INIT, DEBUG_ERROR, 0, "Unable to initialize AB module!");
        atomic_set_int32(&library_state, LIB_STATE_UNINITIALIZED);
        return rc;
    }

    pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Initializing Modbus module.");
    rc = mb_init();
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_INIT, DEBUG_ERROR, 0, "Unable to initialize Modbus module!");
        atomic_set_int32(&library_state, LIB_STATE_UNINITIALIZED);
        return rc;
    }

    pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Initializing Omron module.");
    rc = omron_init();
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_INIT, DEBUG_ERROR, 0, "Unable to initialize Omron module!");
        atomic_set_int32(&library_state, LIB_STATE_UNINITIALIZED);
        return rc;
    }

    /* hook the destructor */
    atexit(plc_tag_shutdown);

    /* Transition to RUNNING - initialization complete */
    atomic_set_int32(&library_state, LIB_STATE_RUNNING);

    pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Done initializing library modules.");

    return PLCTAG_STATUS_OK;
}
