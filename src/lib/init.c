/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
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

#include <stdlib.h>

#include <ab/ab.h>
#include <ab2/ab.h>
#include <lib/init.h>
#include <lib/libplctag.h>
#include <lib/tag.h>
#include <mb/modbus.h>
#include <system/system.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/event_loop.h>
#include <util/lock.h>
#include <util/mutex.h>
#include <util/plc.h>
#include <util/string.h>
#include <util/time.h>




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
    {NULL, "system", "library", NULL, system_tag_create},
    /* Allen-Bradley PLCs */
    {"ab-eip", NULL, NULL, NULL, ab_tag_create},
    {"ab_eip", NULL, NULL, NULL, ab_tag_create},
    {"ab-eip2", NULL, NULL, NULL, ab2_tag_create},
    {"ab_eip2", NULL, NULL, NULL, ab2_tag_create},
    {"modbus-tcp", NULL, NULL, NULL, mb_tag_create},
    {"modbus_tcp", NULL, NULL, NULL, mb_tag_create}
};

static lock_t library_initialization_lock = LOCK_INIT;
static volatile int library_initialized = 0;
static volatile mutex_p lib_mutex = NULL;


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

tag_create_function find_tag_create_func(attr attributes)
{
    int i = 0;
    const char *protocol = attr_get_str(attributes, "protocol", NULL);
    const char *make = attr_get_str(attributes, "make", attr_get_str(attributes, "manufacturer", NULL));
    const char *family = attr_get_str(attributes, "family", NULL);
    const char *model = attr_get_str(attributes, "model", NULL);
    int num_entries = (sizeof(tag_type_map)/sizeof(tag_type_map[0]));

    /* if protocol is set, then use it to match. */
    if(protocol && str_length(protocol) > 0) {
        for(i=0; i < num_entries; i++) {
            if(tag_type_map[i].protocol && str_cmp(tag_type_map[i].protocol, protocol) == 0) {
                pdebug(DEBUG_INFO,"Matched protocol=%s", protocol);
                return tag_type_map[i].tag_constructor;
            }
        }
    } else {
        /* match make/family/model */
        for(i=0; i < num_entries; i++) {
            if(tag_type_map[i].make && make && str_cmp_i(tag_type_map[i].make, make) == 0) {
                pdebug(DEBUG_INFO,"Matched make=%s",make);
                if(tag_type_map[i].family) {
                    if(family && str_cmp_i(tag_type_map[i].family, family) == 0) {
                        pdebug(DEBUG_INFO, "Matched make=%s family=%s", make, family);
                        if(tag_type_map[i].model) {
                            if(model && str_cmp_i(tag_type_map[i].model, model) == 0) {
                                pdebug(DEBUG_INFO, "Matched make=%s family=%s model=%s", make, family, model);
                                return tag_type_map[i].tag_constructor;
                            }
                        } else {
                            /* matches until a NULL */
                            pdebug(DEBUG_INFO, "Matched make=%s family=%s model=NULL", make, family);
                            return tag_type_map[i].tag_constructor;
                        }
                    }
                } else {
                    /* matched until a NULL, so we matched */
                    pdebug(DEBUG_INFO, "Matched make=%s family=NULL model=NULL", make);
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

void destroy_modules(void)
{
    ab_teardown();

    // ab2_protocol_teardown();

    mb_teardown();

    plc_module_teardown();

    event_loop_teardown();

    lib_teardown();

    spin_block(&library_initialization_lock) {
        if(lib_mutex != NULL) {
            /* FIXME casting to get rid of volatile is WRONG */
            mutex_destroy((mutex_p *)&lib_mutex);
            lib_mutex = NULL;
        }
    }

    plc_tag_unregister_logger();

    library_initialized = 0;
}



/*
 * initialize_modules() is called the first time any kind of tag is
 * created.  It will be called before the tag creation routines are
 * run.
 */


int initialize_modules(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    /*
     * Try to keep busy waiting to a minimum.
     * If there is no mutex set up, then create one.
     * Only one thread allowed at a time through this gate.
     */
    spin_block(&library_initialization_lock) {
        if(lib_mutex == NULL) {
            pdebug(DEBUG_INFO, "Creating library mutex.");
            /* FIXME - casting to get rid of volatile is WRONG */
            rc = mutex_create((mutex_p *)&lib_mutex);
        }
    }

    /* check the status outside the lock. */
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to initialize library mutex!  Error %s!", plc_tag_decode_error(rc));
        return rc;
    } else {
        /*
        * guard library initialization with a mutex.
        *
        * This prevents busy waiting as would happen with just a spin lock.
        */
        critical_block(lib_mutex) {
            if(!library_initialized) {
                /* initialize a random seed value. */
                srand((unsigned int)time_ms());

                pdebug(DEBUG_INFO,"Initializing platform module.");
                if(rc == PLCTAG_STATUS_OK) {
                    rc = event_loop_init();
                }

                pdebug(DEBUG_INFO,"Initialized library modules.");
                rc = lib_init();

                pdebug(DEBUG_INFO,"Initializing event loop.");
                if(rc == PLCTAG_STATUS_OK) {
                    rc = event_loop_init();
                }

                pdebug(DEBUG_INFO,"Initializing PLC module.");
                if(rc == PLCTAG_STATUS_OK) {
                    rc = plc_module_init();
                }

                // pdebug(DEBUG_INFO,"Initializing AB2 module.");
                // if(rc == PLCTAG_STATUS_OK) {
                //     rc = ab2_protocol_init();
                // }

                pdebug(DEBUG_INFO,"Initializing AB module.");
                if(rc == PLCTAG_STATUS_OK) {
                    rc = ab_init();
                }

                pdebug(DEBUG_INFO,"Initializing Modbus module.");
                if(rc == PLCTAG_STATUS_OK) {
                    rc = mb_init();
                }

                /* hook the destructor */
                atexit(destroy_modules);

                /* do this last */
                library_initialized = 1;

                pdebug(DEBUG_INFO,"Done initializing library modules.");
            }
        }
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}

