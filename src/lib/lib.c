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

#define LIBPLCTAGDLL_EXPORTS 1

#include <ctype.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <lib/libplctag.h>
#include <lib/tag.h>
#include <lib/init.h>
#include <lib/version.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/hash.h>
#include <util/hashtable.h>
#include <util/mem.h>
#include <util/mutex.h>
#include <util/thread.h>
#include <util/time.h>
#include <util/sleep.h>
#include <util/string.h>
#include <util/vector.h>
#include <ab/ab.h>
#include <mb/modbus.h>


#define INITIAL_TAG_TABLE_SIZE (201)

#define TAG_ID_MASK (0xFFFFFFF)

#define MAX_TAG_MAP_ATTEMPTS (50)

/* these are only internal to the file */

static volatile int32_t next_tag_id = 10; /* MAGIC */
static volatile hashtable_p tags = NULL;
static mutex_p tag_lookup_mutex = NULL;

static volatile int library_terminating = 0;
static thread_p tag_tickler_thread = NULL;

//static mutex_p global_library_mutex = NULL;



/* helper functions. */
static plc_tag_p lookup_tag(int32_t id);
static int add_tag_lookup(plc_tag_p tag);
static int tag_id_inc(int id);
static THREAD_FUNC(tag_tickler_func);
static int set_tag_byte_order(plc_tag_p tag, attr attribs);
static int check_byte_order_str(const char *byte_order, int length);
// static int get_string_count_size_unsafe(plc_tag_p tag, int offset);
static int get_string_length_unsafe(plc_tag_p tag, int offset);
// static int get_string_capacity_unsafe(plc_tag_p tag, int offset);
// static int get_string_padding_unsafe(plc_tag_p tag, int offset);
// static int get_string_total_length_unsafe(plc_tag_p tag, int offset);
// static int get_string_byte_swapped_index_unsafe(plc_tag_p tag, int offset, int char_index);


#if defined(WIN32) || defined(_WIN32) || defined(_WIN64)
#include <process.h>
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    switch(fdwReason) {
        case DLL_PROCESS_ATTACH:
            //fprintf(stderr, "DllMain called with DLL_PROCESS_ATTACH\n");
            break;

        case DLL_PROCESS_DETACH:
            //fprintf(stderr, "DllMain called with DLL_PROCESS_DETACH\n");
            destroy_modules();
            break;

        case DLL_THREAD_ATTACH:
            //fprintf(stderr, "DllMain called with DLL_THREAD_ATTACH\n");
            break;

        case DLL_THREAD_DETACH:
            //fprintf(stderr, "DllMain called with DLL_THREAD_DETACH\n");
            break;

        default:
            //fprintf(stderr, "DllMain called with unexpected code %d!\n", fdwReason);
            break;
    }

    return TRUE;
}

#endif

/*
 * Initialize the library.  This is called in a threadsafe manner and
 * only called once.
 */

int lib_init(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO,"Setting up global library data.");

    pdebug(DEBUG_INFO,"Creating tag hashtable.");
    if((tags = hashtable_create(INITIAL_TAG_TABLE_SIZE)) == NULL) { /* MAGIC */
        pdebug(DEBUG_ERROR, "Unable to create tag hashtable!");
        return PLCTAG_ERR_NO_MEM;
    }

    pdebug(DEBUG_INFO,"Creating tag hashtable mutex.");
    rc = mutex_create((mutex_p *)&tag_lookup_mutex);
    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to create tag hashtable mutex!");
    }

    pdebug(DEBUG_INFO,"Creating tag tickler thread.");
    rc = thread_create(&tag_tickler_thread, tag_tickler_func, 32*1024, NULL);
    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to create tag tickler thread!");
    }

    pdebug(DEBUG_INFO,"Done.");

    return rc;
}




void lib_teardown(void)
{
    pdebug(DEBUG_INFO,"Tearing down library.");

    library_terminating = 1;

    if(tag_tickler_thread) {
        pdebug(DEBUG_INFO,"Tearing down tag tickler thread.");
        thread_join(tag_tickler_thread);
        thread_destroy(&tag_tickler_thread);
        tag_tickler_thread = NULL;
    }

    if(tag_lookup_mutex) {
        pdebug(DEBUG_INFO,"Tearing down tag lookup mutex.");
        mutex_destroy(&tag_lookup_mutex);
        tag_lookup_mutex = NULL;
    }

    if(tags) {
        pdebug(DEBUG_INFO, "Destroying tag hashtable.");
        hashtable_destroy(tags);
        tags = NULL;
    }

    library_terminating = 0;

    pdebug(DEBUG_INFO,"Done.");
}




THREAD_FUNC(tag_tickler_func)
{
    (void)arg;

    debug_set_tag_id(0);

    pdebug(DEBUG_INFO, "Starting.");

    while(!library_terminating) {
        int max_index = 0;

        critical_block(tag_lookup_mutex) {
            max_index = hashtable_capacity(tags);
        }

        for(int i=0; i < max_index; i++) {
            plc_tag_p tag = NULL;

            critical_block(tag_lookup_mutex) {
                /* look up the max index again. it may have changed. */
                max_index = hashtable_capacity(tags);

                if(i < max_index) {
                    tag = hashtable_get_index(tags, i);

                    if(tag) {
                        debug_set_tag_id(tag->tag_id);
                        tag = rc_inc(tag);
                    }
                } else {
                    debug_set_tag_id(0);
                    tag = NULL;
                }
            }

            if(tag) {
                int events[PLCTAG_EVENT_DESTROYED+1] =  {0};

                debug_set_tag_id(tag->tag_id);

                /* try to hold the tag API mutex while all this goes on. */
                if(mutex_try_lock(tag->api_mutex) == PLCTAG_STATUS_OK) {
                    /* if this tag has automatic writes, then there are many things we should check */
                    if(tag->auto_sync_write_ms > 0) {
                        /* has the tag been written to? */
                        if(tag->tag_is_dirty) {
                            /* abort any in flight read if the tag is dirty. */
                            if(tag->read_in_flight) {
                                if(tag->vtable->abort) {
                                    tag->vtable->abort(tag);
                                }

                                pdebug(DEBUG_DETAIL, "Aborting in-flight automatic read!");

                                tag->read_complete = 0;
                                tag->read_in_flight = 0;

                                /* FIXME - should we report an ABORT event here? */
                                events[PLCTAG_EVENT_ABORTED] = 1;
                            }

                            /* have we already done something about it? */
                            if(!tag->auto_sync_next_write) {
                                /* we need to queue up a new write. */
                                tag->auto_sync_next_write = time_ms() + tag->auto_sync_write_ms;

                                pdebug(DEBUG_DETAIL, "Queueing up automatic write in %dms.", tag->auto_sync_write_ms);
                            } else if(!tag->write_in_flight && tag->auto_sync_next_write <= time_ms()) {
                                pdebug(DEBUG_DETAIL, "Triggering automatic write start.");

                                /* clear out any outstanding reads. */
                                if(tag->read_in_flight && tag->vtable->abort) {
                                    tag->vtable->abort(tag);
                                    tag->read_in_flight = 0;
                                }

                                tag->tag_is_dirty = 0;
                                tag->write_in_flight = 1;
                                tag->auto_sync_next_write = 0;

                                if(tag->vtable->write) {
                                    SET_STATUS(tag->status, tag->vtable->write(tag));
                                }

                                events[PLCTAG_EVENT_WRITE_STARTED] = 1;
                            }
                        }
                    }

                    /* if this tag has automatic reads, we need to check that state too. */
                    if(tag->auto_sync_read_ms > 0) {
                        int64_t current_time = time_ms();

                        /* do we need to read? */
                        if(tag->auto_sync_next_read <= current_time) {
                            /* make sure that we do not have an outstanding read or write. */
                            if(!tag->read_in_flight && !tag->tag_is_dirty && !tag->write_in_flight) {
                                int64_t periods = 0;

                                pdebug(DEBUG_DETAIL, "Triggering automatic read start.");

                                tag->read_in_flight = 1;

                                if(tag->vtable->read) {
                                    SET_STATUS(tag->status, (tag->vtable->read(tag)));
                                }

                                /*
                                 * schedule the next read.
                                 *
                                 * Note that there will be some jitter.  In that case we want to skip
                                 * to the next read time that is a whole multiple of the read period.
                                 *
                                 * This keeps the jitter from slowly moving the polling cycle.
                                 */
                                periods = (current_time - tag->auto_sync_next_read)/tag->auto_sync_read_ms;

                                /* warn if we need to skip more than one period. */
                                if(tag->auto_sync_next_read && periods > 0) {
                                    pdebug(DEBUG_WARN, "Skipping multiple read periods due to long delay!");
                                }

                                tag->auto_sync_next_read += (periods + 1) * tag->auto_sync_read_ms;
                                pdebug(DEBUG_WARN, "Scheduling next read at time %"PRId64".", tag->auto_sync_next_read);

                                events[PLCTAG_EVENT_READ_STARTED] = 1;
                            }
                        }
                    }

                    /* call the tickler function if we can. */
                    if(tag->vtable->tickler) {
                        /* call the tickler on the tag. */
                        tag->vtable->tickler(tag);

                        if(tag->read_complete) {
                            tag->read_complete = 0;
                            tag->read_in_flight = 0;

                            events[PLCTAG_EVENT_READ_COMPLETED] = 1;
                        }

                        if(tag->write_complete) {
                            tag->write_complete = 0;
                            tag->write_in_flight = 0;
                            tag->auto_sync_next_write = 0;

                            events[PLCTAG_EVENT_WRITE_COMPLETED] = 1;
                        }
                    }

                    /* we are done with the tag API mutex now. */
                    mutex_unlock(tag->api_mutex);

                    /* call the callback outside the API mutex. */
                    if(tag->callback) {
                        /* was there a read start? */
                        if(events[PLCTAG_EVENT_READ_STARTED]) {
                            pdebug(DEBUG_DETAIL, "Tag read started.");
                            tag->callback(tag->tag_id, PLCTAG_EVENT_READ_STARTED, plc_tag_status(tag->tag_id));
                        }

                        /* was there a write start? */
                        if(events[PLCTAG_EVENT_WRITE_STARTED]) {
                            pdebug(DEBUG_DETAIL, "Tag write started.");
                            tag->callback(tag->tag_id, PLCTAG_EVENT_WRITE_STARTED, plc_tag_status(tag->tag_id));
                        }

                        /* was there an abort? */
                        if(events[PLCTAG_EVENT_ABORTED]) {
                            pdebug(DEBUG_DETAIL, "Tag operation aborted.");
                            tag->callback(tag->tag_id, PLCTAG_EVENT_ABORTED, plc_tag_status(tag->tag_id));
                        }

                        /* was there a read completion? */
                        if(events[PLCTAG_EVENT_READ_COMPLETED]) {
                            pdebug(DEBUG_DETAIL, "Tag read completed.");
                            tag->callback(tag->tag_id, PLCTAG_EVENT_READ_COMPLETED, plc_tag_status(tag->tag_id));
                        }

                        /* was there a write completion? */
                        if(events[PLCTAG_EVENT_WRITE_COMPLETED]) {
                            pdebug(DEBUG_DETAIL, "Tag write completed.");
                            tag->callback(tag->tag_id, PLCTAG_EVENT_WRITE_COMPLETED, plc_tag_status(tag->tag_id));
                        }
                    }
                }
            }

            if(tag) {
                debug_set_tag_id(0);
                rc_dec(tag);
            }
        }

        if(!library_terminating) {
            sleep_ms(1);
        }
    }

    debug_set_tag_id(0);

    pdebug(DEBUG_INFO,"Terminating.");

    THREAD_RETURN(0);
}


/**************************************************************************
 ***************************  API Functions  ******************************
 **************************************************************************/


/*
 * plc_tag_decode_error()
 *
 * This takes an integer error value and turns it into a printable string.
 *
 * FIXME - this should produce better errors than this!
 */



LIB_EXPORT const char *plc_tag_decode_error(int rc)
{
    switch(rc) {
    case PLCTAG_STATUS_PENDING:
        return "PLCTAG_STATUS_PENDING";
    case PLCTAG_STATUS_OK:
        return "PLCTAG_STATUS_OK";
    case PLCTAG_ERR_ABORT:
        return "PLCTAG_ERR_ABORT";
    case PLCTAG_ERR_BAD_CONFIG:
        return "PLCTAG_ERR_BAD_CONFIG";
    case PLCTAG_ERR_BAD_CONNECTION:
        return "PLCTAG_ERR_BAD_CONNECTION";
    case PLCTAG_ERR_BAD_DATA:
        return "PLCTAG_ERR_BAD_DATA";
    case PLCTAG_ERR_BAD_DEVICE:
        return "PLCTAG_ERR_BAD_DEVICE";
    case PLCTAG_ERR_BAD_GATEWAY:
        return "PLCTAG_ERR_BAD_GATEWAY";
    case PLCTAG_ERR_BAD_PARAM:
        return "PLCTAG_ERR_BAD_PARAM";
    case PLCTAG_ERR_BAD_REPLY:
        return "PLCTAG_ERR_BAD_REPLY";
    case PLCTAG_ERR_BAD_STATUS:
        return "PLCTAG_ERR_BAD_STATUS";
    case PLCTAG_ERR_CLOSE:
        return "PLCTAG_ERR_CLOSE";
    case PLCTAG_ERR_CREATE:
        return "PLCTAG_ERR_CREATE";
    case PLCTAG_ERR_DUPLICATE:
        return "PLCTAG_ERR_DUPLICATE";
    case PLCTAG_ERR_ENCODE:
        return "PLCTAG_ERR_ENCODE";
    case PLCTAG_ERR_MUTEX_DESTROY:
        return "PLCTAG_ERR_MUTEX_DESTROY";
    case PLCTAG_ERR_MUTEX_INIT:
        return "PLCTAG_ERR_MUTEX_INIT";
    case PLCTAG_ERR_MUTEX_LOCK:
        return "PLCTAG_ERR_MUTEX_LOCK";
    case PLCTAG_ERR_MUTEX_UNLOCK:
        return "PLCTAG_ERR_MUTEX_UNLOCK";
    case PLCTAG_ERR_NOT_ALLOWED:
        return "PLCTAG_ERR_NOT_ALLOWED";
    case PLCTAG_ERR_NOT_FOUND:
        return "PLCTAG_ERR_NOT_FOUND";
    case PLCTAG_ERR_NOT_IMPLEMENTED:
        return "PLCTAG_ERR_NOT_IMPLEMENTED";
    case PLCTAG_ERR_NO_DATA:
        return "PLCTAG_ERR_NO_DATA";
    case PLCTAG_ERR_NO_MATCH:
        return "PLCTAG_ERR_NO_MATCH";
    case PLCTAG_ERR_NO_MEM:
        return "PLCTAG_ERR_NO_MEM";
    case PLCTAG_ERR_NO_RESOURCES:
        return "PLCTAG_ERR_NO_RESOURCES";
    case PLCTAG_ERR_NULL_PTR:
        return "PLCTAG_ERR_NULL_PTR";
    case PLCTAG_ERR_OPEN:
        return "PLCTAG_ERR_OPEN";
    case PLCTAG_ERR_OUT_OF_BOUNDS:
        return "PLCTAG_ERR_OUT_OF_BOUNDS";
    case PLCTAG_ERR_READ:
        return "PLCTAG_ERR_READ";
    case PLCTAG_ERR_REMOTE_ERR:
        return "PLCTAG_ERR_REMOTE_ERR";
    case PLCTAG_ERR_THREAD_CREATE:
        return "PLCTAG_ERR_THREAD_CREATE";
    case PLCTAG_ERR_THREAD_JOIN:
        return "PLCTAG_ERR_THREAD_JOIN";
    case PLCTAG_ERR_TIMEOUT:
        return "PLCTAG_ERR_TIMEOUT";
    case PLCTAG_ERR_TOO_LARGE:
        return "PLCTAG_ERR_TOO_LARGE";
    case PLCTAG_ERR_TOO_SMALL:
        return "PLCTAG_ERR_TOO_SMALL";
    case PLCTAG_ERR_UNSUPPORTED:
        return "PLCTAG_ERR_UNSUPPORTED";
    case PLCTAG_ERR_WINSOCK:
        return "PLCTAG_ERR_WINSOCK";
    case PLCTAG_ERR_WRITE:
        return "PLCTAG_ERR_WRITE";
    case PLCTAG_ERR_PARTIAL:
        return "PLCTAG_ERR_PARTIAL";
    case PLCTAG_ERR_BUSY:
        return "PLCTAG_ERR_BUSY";

    default:
        return "Unknown error.";
        break;
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

LIB_EXPORT void plc_tag_set_debug_level(int debug_level)
{
	if (debug_level >= PLCTAG_DEBUG_NONE && debug_level <= PLCTAG_DEBUG_SPEW) {
		set_debug_level(debug_level);
	}
}




/*
 * Check that the library supports the required API version.
 *
 * PLCTAG_STATUS_OK is returned if the version matches.  If it does not,
 * PLCTAG_ERR_UNSUPPORTED is returned.
 */

LIB_EXPORT int plc_tag_check_lib_version(int req_major, int req_minor, int req_patch)
{
    /* encode these with 16-bits per version part. */
    uint64_t lib_encoded_version = (((uint64_t)version_major) << 32u)
                                 + (((uint64_t)version_minor) << 16u)
                                   + (uint64_t)version_patch;

    uint64_t req_encoded_version = (((uint64_t)req_major) << 32u)
                                 + (((uint64_t)req_minor) << 16u)
                                   + (uint64_t)req_patch;

    if(version_major == (uint64_t)req_major && lib_encoded_version >= req_encoded_version) {
        return PLCTAG_STATUS_OK;
    } else {
        return PLCTAG_ERR_UNSUPPORTED;
    }
}






/*
 * plc_tag_create()
 *
 * Just pass through to the plc_tag_create_sync() function.
 *
 * This is where the dispatch occurs to the protocol specific implementation.
 */

LIB_EXPORT int32_t plc_tag_create(const char *attrib_str, int timeout)
{
    plc_tag_p tag = PLC_TAG_P_NULL;
    int id = PLCTAG_ERR_OUT_OF_BOUNDS;
    attr attribs = NULL;
    int rc = PLCTAG_STATUS_OK;
    int read_cache_ms = 0;
    tag_create_function tag_constructor;
	int debug_level = -1;

    pdebug(DEBUG_INFO,"Starting");

    if(timeout < 0) {
        pdebug(DEBUG_WARN, "Timeout must not be negative!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    if((rc = initialize_modules()) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR,"Unable to initialize the internal library state!");
        return rc;
    }

    if(!attrib_str || str_length(attrib_str) == 0) {
        pdebug(DEBUG_WARN,"Tag attribute string is null or zero length!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    attribs = attr_create_from_str(attrib_str);
    if(!attribs) {
        pdebug(DEBUG_WARN,"Unable to parse attribute string!");
        return PLCTAG_ERR_BAD_DATA;
    }

    /* set debug level */
	debug_level = attr_get_int(attribs, "debug", -1);
	if (debug_level > DEBUG_NONE) {
		set_debug_level(debug_level);
	}

    /*
     * create the tag, this is protocol specific.
     *
     * If this routine wants to keep the attributes around, it needs
     * to clone them.
     */
    tag_constructor = find_tag_create_func(attribs);

    if(!tag_constructor) {
        pdebug(DEBUG_WARN,"Tag creation failed, no tag constructor found for tag type!");
        attr_destroy(attribs);
        return PLCTAG_ERR_BAD_PARAM;
    }

    tag = tag_constructor(attribs);

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag creation failed, skipping mutex creation and other generic setup.");
        attr_destroy(attribs);
        return PLCTAG_ERR_CREATE;
    }

    /*
     * FIXME - this really should be here???  Maybe not?  But, this is
     * the only place it can be without making every protocol type do this automatically.
     */
    if(!tag->ext_mutex) {
        rc = mutex_create(&(tag->ext_mutex));
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN,"Unable to create tag external mutex!");
            rc_dec(tag);
            return PLCTAG_ERR_CREATE;
        }
    } else {
        pdebug(DEBUG_DETAIL, "Specific tag type created external mutex.");
    }

    if(!tag->api_mutex) {
        rc = mutex_create(&(tag->api_mutex));
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN,"Unable to create tag API mutex!");
            rc_dec(tag);
            return PLCTAG_ERR_CREATE;
        }
    } else {
        pdebug(DEBUG_DETAIL, "Specific tag type created API mutex.");
    }

    /* set up the read cache config. */
    read_cache_ms = attr_get_int(attribs,"read_cache_ms",0);
    if(read_cache_ms < 0) {
        pdebug(DEBUG_WARN, "read_cache_ms value must be positive, using zero.");
        read_cache_ms = 0;
    }

    tag->read_cache_expire = (int64_t)0;
    tag->read_cache_ms = (int64_t)read_cache_ms;

    /* set up any automatic read/write */
    tag->auto_sync_read_ms = attr_get_int(attribs, "auto_sync_read_ms", 0);
    if(tag->auto_sync_read_ms < 0) {
        pdebug(DEBUG_WARN, "auto_sync_read_ms value must be positive!");
        rc_dec(tag);
        return PLCTAG_ERR_BAD_PARAM;
    } else if(tag->auto_sync_read_ms > 0) {
        /* how many periods did we already pass? */
        int64_t periods = (time_ms() / tag->auto_sync_read_ms);
        tag->auto_sync_next_read = (periods + 1) * tag->auto_sync_read_ms;
    }

    tag->auto_sync_write_ms = attr_get_int(attribs, "auto_sync_write_ms", 0);
    if(tag->auto_sync_write_ms < 0) {
        pdebug(DEBUG_WARN, "auto_sync_write_ms value must be positive!");
        rc_dec(tag);
        return PLCTAG_ERR_BAD_PARAM;
    } else {
        tag->auto_sync_next_write = 0;
    }

    /* set up the tag byte order if there are any overrides. */
    rc = set_tag_byte_order(tag, attribs);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to correctly set tag data byte order: %s!", plc_tag_decode_error(rc));
        rc_dec(tag);
        return rc;
    }

    /*
     * Release memory for attributes
     */
    attr_destroy(attribs);

    /*
    * if there is a timeout, then loop until we get
    * an error or we timeout.
    */
    if(timeout) {
        int64_t timeout_time = timeout + time_ms();
        int64_t start_time = time_ms();

        /* get the tag status. */
        rc = tag->vtable->status(tag);

        while(rc == PLCTAG_STATUS_PENDING && timeout_time > time_ms()) {
            /* give some time to the tickler function. */
            if(tag->vtable->tickler) {
                tag->vtable->tickler(tag);
            }

            rc = tag->vtable->status(tag);

            /*
             * terminate early and do not wait again if the
             * IO is done.
             */
            if(rc != PLCTAG_STATUS_PENDING) {
                break;
            }

            sleep_ms(1); /* MAGIC */
        }

        /*
         * if we dropped out of the while loop but the status is
         * still pending, then we timed out.
         *
         * Abort the operation and set the status to show the timeout.
         */
        if(rc == PLCTAG_STATUS_PENDING) {
            pdebug(DEBUG_WARN,"Timeout waiting for tag to be ready!");
            tag->vtable->abort(tag);
            rc = PLCTAG_ERR_TIMEOUT;
        }

        /* check to see if there was an error during tag creation. */
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error %s while trying to create tag!", plc_tag_decode_error(rc));
            rc_dec(tag);
            return rc;
        }

        /* clear up any remaining flags.  This should be refactored. */
        tag->read_in_flight = 0;
        tag->write_in_flight = 0;

        pdebug(DEBUG_INFO,"tag set up elapsed time %" PRId64 "ms",(time_ms()-start_time));
    }

    /* map the tag to a tag ID */
    id = add_tag_lookup(tag);

    /* if the mapping failed, then punt */
    if(id < 0) {
        pdebug(DEBUG_ERROR, "Unable to map tag %p to lookup table entry, rc=%s", tag, plc_tag_decode_error(id));
        rc_dec(tag);
        return id;
    }

    /* save this for later. */
    tag->tag_id = id;

    debug_set_tag_id(id);

    pdebug(DEBUG_INFO, "Returning mapped tag ID %d", id);

    pdebug(DEBUG_INFO,"Done.");

    return id;
}




/*
 * plc_tag_shutdown
 *
 * Some systems may not be able to call atexit() handlers.  In those cases, wrappers should
 * call this function before unloading the library or terminating.   Most OSes will cleanly
 * recover all system resources when a process is terminated and this will not be necessary.
 */

LIB_EXPORT void plc_tag_shutdown(void)
{
    destroy_modules();
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
 *      * starting a tag read operation.
 *      * a tag read operation ending.
 *      * a tag read being aborted.
 *      * starting a tag write operation.
 *      * a tag write operation ending.
 *      * a tag write being aborted.
 *      * a tag being destroyed
 *
 * The callback is called outside of the internal tag mutex so it can call any tag functions safely.   However,
 * the callback is called in the context of the internal tag helper thread and not the client library thread(s).
 * This means that YOU are responsible for making sure that all client application data structures the callback
 * function touches are safe to access by the callback!
 *
 * Do not do any operations in the callback that block for any significant time.   This will cause library
 * performance to be poor or even to start failing!
 *
 * When the callback is called with the PLCTAG_EVENT_DESTROY_STARTED, do not call any tag functions.  It is
 * not guaranteed that they will work and they will possibly hang or fail.
 *
 * Return values:
 *void (*tag_callback_func)(int32_t tag_id, uint32_t event, int status)
 * If there is already a callback registered, the function will return PLCTAG_ERR_DUPLICATE.   Only one callback
 * function may be registered at a time on each tag.
 *
 * If all is successful, the function will return PLCTAG_STATUS_OK.
 */

LIB_EXPORT int plc_tag_register_callback(int32_t tag_id, void (*tag_callback_func)(int32_t tag_id, int event, int status))
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(tag_id);

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        if(tag->callback) {
            rc = PLCTAG_ERR_DUPLICATE;
        } else {
            rc = PLCTAG_STATUS_OK;
            tag->callback = tag_callback_func;
        }
    }

    rc_dec(tag);

    pdebug(DEBUG_INFO, "Done.");

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

LIB_EXPORT int plc_tag_unregister_callback(int32_t tag_id)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(tag_id);

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        if(tag->callback) {
            rc = PLCTAG_STATUS_OK;
            tag->callback = NULL;
        } else {
            rc = PLCTAG_ERR_NOT_FOUND;
        }
    }

    rc_dec(tag);

    pdebug(DEBUG_INFO, "Done.");

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

LIB_EXPORT int plc_tag_register_logger(void (*log_callback_func)(int32_t tag_id, int debug_level, const char *message))
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    rc = debug_register_logger(log_callback_func);

    pdebug(DEBUG_DETAIL, "Done.");

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

LIB_EXPORT int plc_tag_unregister_logger(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting");

    rc = debug_unregister_logger();

    pdebug(DEBUG_DETAIL, "Done.");

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

LIB_EXPORT int plc_tag_lock(int32_t id)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        rc = mutex_lock(tag->ext_mutex);
    }

    rc_dec(tag);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}




/*
 * plc_tag_unlock
 *
 * The opposite action of plc_tag_unlock.  This allows other threads to access the
 * tag.
 */

LIB_EXPORT int plc_tag_unlock(int32_t id)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        rc = mutex_unlock(tag->ext_mutex);
    }

    rc_dec(tag);

    pdebug(DEBUG_INFO, "Done.");

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

LIB_EXPORT int plc_tag_abort(int32_t id)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        /* who knows what state the tag data is in.  */
        tag->read_cache_expire = (uint64_t)0;

        if(!tag->vtable || !tag->vtable->abort) {
            pdebug(DEBUG_WARN,"Tag does not have a abort function!");
            rc = PLCTAG_ERR_NOT_IMPLEMENTED;
            break;
        }

        /* this may be synchronous. */
        rc = tag->vtable->abort(tag);

        tag->read_in_flight = 0;
        tag->read_complete = 0;
        tag->write_in_flight = 0;
        tag->write_complete = 0;
    }

    if(tag->callback) {
        pdebug(DEBUG_DETAIL, "Calling callback with PLCTAG_EVENT_ABORTED.");
        tag->callback(id, PLCTAG_EVENT_ABORTED, PLCTAG_STATUS_OK);
    }

    rc_dec(tag);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}




/*
 * plc_tag_destroy()
 *
 * Remove all implementation specific details about a tag and clear its
 * memory.
 */


LIB_EXPORT int plc_tag_destroy(int32_t tag_id)
{
    plc_tag_p tag = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    if(tag_id <= 0 || tag_id >= TAG_ID_MASK) {
        pdebug(DEBUG_WARN, "Called with zero or invalid tag!");
        return PLCTAG_ERR_NULL_PTR;
    }

    critical_block(tag_lookup_mutex) {
        tag = hashtable_remove(tags, tag_id);
    }

    if(!tag) {
        pdebug(DEBUG_WARN, "Called with non-existent tag!");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* abort anything in flight */
    pdebug(DEBUG_DETAIL, "Aborting any in-flight operations.");

    critical_block(tag->api_mutex) {
        if(!tag->vtable || !tag->vtable->abort) {
            pdebug(DEBUG_WARN,"Tag does not have a abort function!");
        }

        /* Force a clean up. */
        tag->vtable->abort(tag);
    }

    if(tag->callback) {
        pdebug(DEBUG_DETAIL, "Calling callback with PLCTAG_EVENT_DESTROYED.");
        tag->callback(tag_id, PLCTAG_EVENT_DESTROYED, PLCTAG_STATUS_OK);
    }

    /* release the reference outside the mutex. */
    rc_dec(tag);

    pdebug(DEBUG_INFO, "Done.");

    debug_set_tag_id(0);

    return PLCTAG_STATUS_OK;
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

LIB_EXPORT int plc_tag_read(int32_t id, int timeout)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);
    int is_done = 0;

    pdebug(DEBUG_INFO, "Starting.");

    if(timeout < 0) {
        pdebug(DEBUG_WARN, "Timeout must not be negative!");
        rc_dec(tag);
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    if(tag->callback) {
        pdebug(DEBUG_DETAIL, "Calling callback with PLCTAG_EVENT_READ_STARTED.");
        tag->callback(id, PLCTAG_EVENT_READ_STARTED, PLCTAG_STATUS_OK);
    }

    critical_block(tag->api_mutex) {
        /* check read cache, if not expired, return existing data. */
        if(tag->read_cache_expire > time_ms()) {
            pdebug(DEBUG_INFO, "Returning cached data.");
            rc = PLCTAG_STATUS_OK;
            is_done = 1;
            break;
        }

        if(tag->read_in_flight || tag->write_in_flight) {
            pdebug(DEBUG_WARN, "An operation is already in flight!");
            rc = PLCTAG_ERR_BUSY;
            is_done = 1;
            break;
        }

        if(tag->tag_is_dirty) {
            pdebug(DEBUG_WARN, "Tag has locally updated data that will be overwritten!");
            rc = PLCTAG_ERR_BUSY;
            is_done = 1;
            break;
        }

        tag->read_in_flight = 1;
        SET_STATUS(tag->status, PLCTAG_STATUS_PENDING);

        /* the protocol implementation does not do the timeout. */
        rc = tag->vtable->read(tag);

        /* if not pending then check for success or error. */
        if(rc != PLCTAG_STATUS_PENDING) {
            if(rc != PLCTAG_STATUS_OK) {
                /* not pending and not OK, so error. Abort and clean up. */

                pdebug(DEBUG_WARN,"Response from read command returned error %s!", plc_tag_decode_error(rc));

                if(tag->vtable->abort) {
                    tag->vtable->abort(tag);
                }
            }

            tag->read_in_flight = 0;
            is_done = 1;
            break;
        }

        /*
         * if there is a timeout, then loop until we get
         * an error or we timeout.
         */
        if(timeout) {
            int64_t timeout_time = timeout + time_ms();
            int64_t start_time = time_ms();

            while(rc == PLCTAG_STATUS_PENDING && timeout_time > time_ms()) {
                /* give some time to the tickler function. */
                if(tag->vtable->tickler) {
                    tag->vtable->tickler(tag);
                }

                rc = tag->vtable->status(tag);

                /*
                 * terminate early and do not wait again if the
                 * IO is done.
                 */
                if(rc != PLCTAG_STATUS_PENDING) {
                    break;
                }

                sleep_ms(1); /* MAGIC */
            }

            /*
             * if we dropped out of the while loop but the status is
             * still pending, then we timed out.
             *
             * Abort the operation and set the status to show the timeout.
             */
            if(rc != PLCTAG_STATUS_OK) {
                /* abort the request. */
                if(tag->vtable->abort) {
                    tag->vtable->abort(tag);
                }

                /* translate error if we are still pending. */
                if(rc == PLCTAG_STATUS_PENDING) {
                    pdebug(DEBUG_WARN, "Read operation timed out.");
                    rc = PLCTAG_ERR_TIMEOUT;
                }
            }

            /* we are done. */
            tag->read_complete = 0;
            tag->read_in_flight = 0;
            is_done = 1;

            pdebug(DEBUG_INFO,"elapsed time %" PRId64 "ms",(time_ms()-start_time));
        }
    } /* end of api mutex block */

    if(rc == PLCTAG_STATUS_OK) {
        /* set up the cache time.  This works when read_cache_ms is zero as it is already expired. */
        tag->read_cache_expire = time_ms() + tag->read_cache_ms;
    }

    if(tag->callback) {
        if(is_done) {
            pdebug(DEBUG_DETAIL, "Calling callback with PLCTAG_EVENT_READ_COMPLETED.");
            tag->callback(id, PLCTAG_EVENT_READ_COMPLETED, rc);
        }
    }

    rc_dec(tag);

    pdebug(DEBUG_INFO, "Done");

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


LIB_EXPORT int plc_tag_status(int32_t id)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    /* check the ID.  It might be an error status from creating the tag. */
    if(!tag) {
        if(id < 0) {
            pdebug(DEBUG_WARN, "Called with an error status %s!", plc_tag_decode_error(id));
            return id;
        } else {
            pdebug(DEBUG_WARN,"Tag not found.");
            return PLCTAG_ERR_NOT_FOUND;
        }
    }

    critical_block(tag->api_mutex) {
        if(tag && tag->vtable->tickler) {
            tag->vtable->tickler(tag);
        }

        rc = tag->vtable->status(tag);

        if(rc == PLCTAG_STATUS_OK) {
            if(tag->read_in_flight || tag->write_in_flight) {
                rc = PLCTAG_STATUS_PENDING;
            }
        }
    }

    rc_dec(tag);

    pdebug(DEBUG_SPEW, "Done with rc=%s.", plc_tag_decode_error(rc));

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

LIB_EXPORT int plc_tag_write(int32_t id, int timeout)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);
    int is_done = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    if(timeout < 0) {
        pdebug(DEBUG_WARN, "Timeout must not be negative!");
        rc_dec(tag);
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    if(tag->callback) {
        pdebug(DEBUG_DETAIL, "Calling callback with PLCTAG_EVENT_WRITE_STARTED.");
        tag->callback(id, PLCTAG_EVENT_WRITE_STARTED, PLCTAG_STATUS_OK);
    }

    critical_block(tag->api_mutex) {
        if(tag->read_in_flight || tag->write_in_flight) {
            pdebug(DEBUG_WARN, "Tag already has an operation in flight!");
            is_done = 1;
            rc = PLCTAG_ERR_BUSY;
            break;
        }

        /* a write is now in flight. */
        tag->write_in_flight = 1;
        SET_STATUS(tag->status, PLCTAG_STATUS_PENDING);

        /* the protocol implementation does not do the timeout. */
        rc = tag->vtable->write(tag);

        /* if not pending then check for success or error. */
        if(rc != PLCTAG_STATUS_PENDING) {
            if(rc != PLCTAG_STATUS_OK) {
                /* not pending and not OK, so error. Abort and clean up. */

                pdebug(DEBUG_WARN,"Response from write command returned error %s!", plc_tag_decode_error(rc));

                if(tag->vtable->abort) {
                    tag->vtable->abort(tag);
                }
            }

            tag->write_in_flight = 0;
            is_done = 1;
            break;
        }

        /*
         * if there is a timeout, then loop until we get
         * an error or we timeout.
         */
        if(timeout) {
            int64_t start_time = time_ms();
            int64_t timeout_time = timeout + start_time;

            while(rc == PLCTAG_STATUS_PENDING && timeout_time > time_ms()) {
                /* give some time to the tickler function. */
                if(tag->vtable->tickler) {
                    tag->vtable->tickler(tag);
                }

                rc = tag->vtable->status(tag);

                /*
                 * terminate early and do not wait again if the
                 * IO is done.
                 */
                if(rc != PLCTAG_STATUS_PENDING) {
                    break;
                }

                sleep_ms(1); /* MAGIC */
            }

            /*
             * if we dropped out of the while loop but the status is
             * still pending, then we timed out.
             *
             * Abort the operation and set the status to show the timeout.
             */
            if(rc != PLCTAG_STATUS_OK) {
                /* abort the request. */
                if(tag->vtable->abort) {
                    tag->vtable->abort(tag);
                }

                /* translate error if we are still pending. */
                if(rc == PLCTAG_STATUS_PENDING) {
                    pdebug(DEBUG_WARN, "Write operation timed out.");
                    rc = PLCTAG_ERR_TIMEOUT;
                }
            }

            /* the write is not in flight anymore. */
            tag->write_in_flight = 0;
            tag->write_complete = 0;
            is_done = 1;

            pdebug(DEBUG_INFO,"elapsed time %" PRId64 "ms",(time_ms()-start_time));
        }
    } /* end of api mutex block */

    if(tag->callback) {
        if(is_done) {
            pdebug(DEBUG_DETAIL, "Calling callback with PLCTAG_EVENT_WRITE_COMPLETED.");
            tag->callback(id, PLCTAG_EVENT_WRITE_COMPLETED, rc);
        }
    }

    rc_dec(tag);

    pdebug(DEBUG_INFO, "Done");

    return rc;
}





/*
 * Tag data accessors.
 */




LIB_EXPORT int plc_tag_get_int_attribute(int32_t id, const char *attrib_name, int default_value)
{
    int res = default_value;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    /* FIXME - this should set the tag status if there is a tag. */
    if(!attrib_name || str_length(attrib_name) == 0) {
        pdebug(DEBUG_WARN, "Attribute name must not be null or zero-length!");
        return default_value;
    }

    /* get library attributes */
    if(id == 0) {
        if(str_cmp_i(attrib_name, "version_major") == 0) {
            res = (int)version_major;
        } else if(str_cmp_i(attrib_name, "version_minor") == 0) {
            res = (int)version_minor;
        } else if(str_cmp_i(attrib_name, "version_patch") == 0) {
            res = (int)version_patch;
        } else if(str_cmp_i(attrib_name, "debug") == 0) {
            res = (int)get_debug_level();
        } else if(str_cmp_i(attrib_name, "debug_level") == 0) {
            pdebug(DEBUG_WARN, "Deprecated attribute \"debug_level\" used, use \"debug\" instead.");
            res = (int)get_debug_level();
        } else {
            pdebug(DEBUG_WARN, "Attribute \"%s\" is not supported at the library level!");
            res = default_value;
        }
    } else {
        tag = lookup_tag(id);

        if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            return default_value;
        }

        critical_block(tag->api_mutex) {
            /* match the generic ones first. */
            if(str_cmp_i(attrib_name, "size") == 0) {
                SET_STATUS(tag->status, PLCTAG_STATUS_OK);
                res = (int)tag->size;
            } else if(str_cmp_i(attrib_name, "read_cache_ms") == 0) {
                /* FIXME - what happens if this overflows? */
                SET_STATUS(tag->status, PLCTAG_STATUS_OK);
                res = (int)tag->read_cache_ms;
            }  else if(str_cmp_i(attrib_name, "auto_sync_read_ms") == 0) {
                SET_STATUS(tag->status, PLCTAG_STATUS_OK);
                res = (int)tag->auto_sync_read_ms;
            }  else if(str_cmp_i(attrib_name, "auto_sync_write_ms") == 0) {
                SET_STATUS(tag->status, PLCTAG_STATUS_OK);
                res = (int)tag->auto_sync_write_ms;
            } else  {
                if(tag->vtable->get_int_attrib) {
                    res = tag->vtable->get_int_attrib(tag, attrib_name, default_value);
                } else {
                    pdebug(DEBUG_WARN, "Unsupported attribute \"%s\"!", attrib_name);
                    SET_STATUS(tag->status, PLCTAG_ERR_UNSUPPORTED);
                }
            }
        }

        rc_dec(tag);
    }

    pdebug(DEBUG_SPEW, "Done.");

    return res;
}



LIB_EXPORT int plc_tag_set_int_attribute(int32_t id, const char *attrib_name, int new_value)
{
    int res = PLCTAG_ERR_NOT_FOUND;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    if(!attrib_name || str_length(attrib_name) == 0) {
        pdebug(DEBUG_WARN, "Attribute name must not be null or zero-length!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* get library attributes */
    if(id == 0) {
        if(str_cmp_i(attrib_name, "debug") == 0) {
            if(new_value >= DEBUG_ERROR && new_value < DEBUG_SPEW) {
                set_debug_level(new_value);
                res = PLCTAG_STATUS_OK;
            } else {
                res = PLCTAG_ERR_OUT_OF_BOUNDS;
            }
        } else if(str_cmp_i(attrib_name, "debug_level") == 0) {
            pdebug(DEBUG_WARN, "Deprecated attribute \"debug_level\" used, use \"debug\" instead.");
            if(new_value >= DEBUG_ERROR && new_value < DEBUG_SPEW) {
                set_debug_level(new_value);
                res = PLCTAG_STATUS_OK;
            } else {
                res = PLCTAG_ERR_OUT_OF_BOUNDS;
            }
        } else {
            pdebug(DEBUG_WARN, "Attribute \"%s\" is not support at the library level!", attrib_name);
            return PLCTAG_ERR_UNSUPPORTED;
        }
    } else {
        tag = lookup_tag(id);

        if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            return PLCTAG_ERR_NOT_FOUND;
        }

        critical_block(tag->api_mutex) {
            /* match the generic ones first. */
            if(str_cmp_i(attrib_name, "read_cache_ms") == 0) {
                if(new_value >= 0) {
                    /* expire the cache. */
                    tag->read_cache_expire = (int64_t)0;
                    tag->read_cache_ms = (int64_t)new_value;
                    SET_STATUS(tag->status, PLCTAG_STATUS_OK);
                    res = PLCTAG_STATUS_OK;
                } else {
                    SET_STATUS(tag->status, PLCTAG_ERR_OUT_OF_BOUNDS);
                    res = PLCTAG_ERR_OUT_OF_BOUNDS;
                }
            } else if(str_cmp_i(attrib_name, "auto_sync_read_ms") == 0) {
                if(new_value >= 0) {
                    tag->auto_sync_read_ms = new_value;
                    SET_STATUS(tag->status, PLCTAG_STATUS_OK);
                    res = PLCTAG_STATUS_OK;
                } else {
                    pdebug(DEBUG_WARN, "auto_sync_read_ms must be greater than or equal to zero!");
                    SET_STATUS(tag->status, PLCTAG_ERR_OUT_OF_BOUNDS);
                    res = PLCTAG_ERR_OUT_OF_BOUNDS;
                }
            } else if(str_cmp_i(attrib_name, "auto_sync_write_ms") == 0) {
                if(new_value >= 0) {
                    tag->auto_sync_write_ms = new_value;
                    SET_STATUS(tag->status, PLCTAG_STATUS_OK);
                    res = PLCTAG_STATUS_OK;
                } else {
                    pdebug(DEBUG_WARN, "auto_sync_write_ms must be greater than or equal to zero!");
                    SET_STATUS(tag->status, PLCTAG_ERR_OUT_OF_BOUNDS);
                    res = PLCTAG_ERR_OUT_OF_BOUNDS;
                }
            } else {
                if(tag->vtable->set_int_attrib) {
                    res = tag->vtable->set_int_attrib(tag, attrib_name, new_value);
                    SET_STATUS(tag->status, res);
                } else {
                    pdebug(DEBUG_WARN, "Unsupported attribute \"%s\"!", attrib_name);
                    SET_STATUS(tag->status, PLCTAG_ERR_UNSUPPORTED);
                }
            }
        }
    }

    rc_dec(tag);

    pdebug(DEBUG_SPEW, "Done.");

    return res;
}




LIB_EXPORT int plc_tag_get_size(int32_t id)
{
    int result = 0;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        result = tag->size;
        SET_STATUS(tag->status, PLCTAG_STATUS_OK);
    }

    rc_dec(tag);

    pdebug(DEBUG_SPEW, "Done.");

    return result;
}



LIB_EXPORT int plc_tag_get_bit(int32_t id, int offset_bit)
{
    int res = PLCTAG_ERR_OUT_OF_BOUNDS;
    int real_offset = offset_bit;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN, "Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return PLCTAG_ERR_NO_DATA;
    }

    /* if this is a single bit, then make sure the offset is the tag bit. */
    if(tag->is_bit) {
        real_offset = tag->bit;
    } else {
        real_offset = offset_bit;
    }

    pdebug(DEBUG_SPEW, "selecting bit %d with offset %d in byte %d (%x).", real_offset, (real_offset % 8), (real_offset / 8), tag->data[real_offset / 8]);

    critical_block(tag->api_mutex) {
        if((real_offset >= 0) && ((real_offset / 8) < tag->size)) {
            res = !!(((1 << (real_offset % 8)) & 0xFF) & (tag->data[real_offset / 8]));
            SET_STATUS(tag->status, PLCTAG_STATUS_OK);
        } else {
            pdebug(DEBUG_WARN, "Data offset out of bounds!");
            res = PLCTAG_ERR_OUT_OF_BOUNDS;
            SET_STATUS(tag->status, res);
        }
    }

    rc_dec(tag);

    return res;
}


LIB_EXPORT int plc_tag_set_bit(int32_t id, int offset_bit, int val)
{
    int res = PLCTAG_STATUS_OK;
    int real_offset = offset_bit;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN, "Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return PLCTAG_ERR_NO_DATA;
    }

    /* if this is a single bit, then make sure the offset is the tag bit. */
    if(tag->is_bit) {
        real_offset = tag->bit;
    } else {
        real_offset = offset_bit;
    }

    pdebug(DEBUG_SPEW, "Setting bit %d with offset %d in byte %d (%x).", real_offset, (real_offset % 8), (real_offset / 8), tag->data[real_offset / 8]);

    critical_block(tag->api_mutex) {
        if((real_offset >= 0) && ((real_offset / 8) < tag->size)) {
            if(tag->auto_sync_write_ms > 0) {
                tag->tag_is_dirty = 1;
            }

            if(val) {
                tag->data[real_offset / 8] |= (uint8_t)(1 << (real_offset % 8));
            } else {
                tag->data[real_offset / 8] &= (uint8_t)(~(1 << (real_offset % 8)));
            }

            SET_STATUS(tag->status, PLCTAG_STATUS_OK);
        } else {
            pdebug(DEBUG_WARN, "Data offset out of bounds!");
            res = PLCTAG_ERR_OUT_OF_BOUNDS;
            SET_STATUS(tag->status, res);
        }
    }

    rc_dec(tag);

    return res;
}



LIB_EXPORT uint64_t plc_tag_get_uint64(int32_t id, int offset)
{
    uint64_t res = UINT64_MAX;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return res;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return res;
    }

    if(!tag->is_bit) {
        critical_block(tag->api_mutex) {
            if((offset >= 0) && (offset + ((int)sizeof(uint64_t)) <= tag->size)) {
                res =   ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[0]]) << 0 ) +
                        ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[1]]) << 8 ) +
                        ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[2]]) << 16) +
                        ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[3]]) << 24) +
                        ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[4]]) << 32) +
                        ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[5]]) << 40) +
                        ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[6]]) << 48) +
                        ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[7]]) << 56);

                SET_STATUS(tag->status, PLCTAG_STATUS_OK);
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                SET_STATUS(tag->status, PLCTAG_ERR_OUT_OF_BOUNDS);
            }
        }
    } else {
        int rc = plc_tag_get_bit(id, tag->bit);

        /* make sure the response is good. */
        if(rc >= 0) {
            res = (unsigned int)rc;
        }
    }

    rc_dec(tag);

    return res;
}



LIB_EXPORT int plc_tag_set_uint64(int32_t id, int offset, uint64_t val)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return PLCTAG_ERR_NO_DATA;
    }

    if(!tag->is_bit) {
        critical_block(tag->api_mutex) {
            if((offset >= 0) && (offset + ((int)sizeof(uint64_t)) <= tag->size)) {
                if(tag->auto_sync_write_ms > 0) {
                    tag->tag_is_dirty = 1;
                }

                tag->data[offset + tag->byte_order->int64_order[0]] = (uint8_t)((val >> 0 ) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[1]] = (uint8_t)((val >> 8 ) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[2]] = (uint8_t)((val >> 16) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[3]] = (uint8_t)((val >> 24) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[4]] = (uint8_t)((val >> 32) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[5]] = (uint8_t)((val >> 40) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[6]] = (uint8_t)((val >> 48) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[7]] = (uint8_t)((val >> 56) & 0xFF);

                SET_STATUS(tag->status, PLCTAG_STATUS_OK);
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                SET_STATUS(tag->status, rc);
            }
        }
    } else {
        if(!val) {
            rc = plc_tag_set_bit(id, 0, 0);
        } else {
            rc = plc_tag_set_bit(id, 0, 1);
        }
    }

    rc_dec(tag);

    return rc;
}




LIB_EXPORT int64_t plc_tag_get_int64(int32_t id, int offset)
{
    int64_t res = INT64_MIN;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return res;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return res;
    }

    if(!tag->is_bit) {
        critical_block(tag->api_mutex) {
            if((offset >= 0) && (offset + ((int)sizeof(int64_t)) <= tag->size)) {
                res = (int64_t)(((uint64_t)(tag->data[offset + tag->byte_order->int64_order[0]]) << 0 ) +
                                ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[1]]) << 8 ) +
                                ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[2]]) << 16) +
                                ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[3]]) << 24) +
                                ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[4]]) << 32) +
                                ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[5]]) << 40) +
                                ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[6]]) << 48) +
                                ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[7]]) << 56));

                SET_STATUS(tag->status, PLCTAG_STATUS_OK);
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                SET_STATUS(tag->status, PLCTAG_ERR_OUT_OF_BOUNDS);
            }
        }
    } else {
        int rc = plc_tag_get_bit(id, tag->bit);

        /* make sure the response is good. */
        if(rc >= 0) {
            res = rc;
        }
    }

    rc_dec(tag);

    return res;
}



LIB_EXPORT int plc_tag_set_int64(int32_t id, int offset, int64_t ival)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);
    uint64_t val = (uint64_t)(ival);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return PLCTAG_ERR_NO_DATA;
    }

    if(!tag->is_bit) {
        critical_block(tag->api_mutex) {
            if((offset >= 0) && (offset + ((int)sizeof(int64_t)) <= tag->size)) {
                if(tag->auto_sync_write_ms > 0) {
                    tag->tag_is_dirty = 1;
                }

                tag->data[offset + tag->byte_order->int64_order[0]] = (uint8_t)((val >> 0 ) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[1]] = (uint8_t)((val >> 8 ) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[2]] = (uint8_t)((val >> 16) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[3]] = (uint8_t)((val >> 24) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[4]] = (uint8_t)((val >> 32) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[5]] = (uint8_t)((val >> 40) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[6]] = (uint8_t)((val >> 48) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[7]] = (uint8_t)((val >> 56) & 0xFF);

                SET_STATUS(tag->status, PLCTAG_STATUS_OK);
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                SET_STATUS(tag->status, rc);
            }
        }
    } else {
        if(!val) {
            rc = plc_tag_set_bit(id, 0, 0);
        } else {
            rc = plc_tag_set_bit(id, 0, 1);
        }
    }

    rc_dec(tag);

    return rc;
}







LIB_EXPORT uint32_t plc_tag_get_uint32(int32_t id, int offset)
{
    uint32_t res = UINT32_MAX;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return res;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return res;
    }

    if(!tag->is_bit) {
        critical_block(tag->api_mutex) {
            if((offset >= 0) && (offset + ((int)sizeof(uint32_t)) <= tag->size)) {
                res =   ((uint32_t)(tag->data[offset + tag->byte_order->int32_order[0]]) << 0 ) +
                        ((uint32_t)(tag->data[offset + tag->byte_order->int32_order[1]]) << 8 ) +
                        ((uint32_t)(tag->data[offset + tag->byte_order->int32_order[2]]) << 16) +
                        ((uint32_t)(tag->data[offset + tag->byte_order->int32_order[3]]) << 24);

                SET_STATUS(tag->status, PLCTAG_STATUS_OK);
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                SET_STATUS(tag->status, PLCTAG_ERR_OUT_OF_BOUNDS);
            }
        }
    } else {
        int rc = plc_tag_get_bit(id, tag->bit);

        /* make sure the response is good. */
        if(rc >= 0) {
            res = (unsigned int)rc;
        }
    }

    rc_dec(tag);

    return res;
}



LIB_EXPORT int plc_tag_set_uint32(int32_t id, int offset, uint32_t val)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return PLCTAG_ERR_NO_DATA;
    }

    if(!tag->is_bit) {
        critical_block(tag->api_mutex) {
            if((offset >= 0) && (offset + ((int)sizeof(uint32_t)) <= tag->size)) {
                if(tag->auto_sync_write_ms > 0) {
                    tag->tag_is_dirty = 1;
                }

                tag->data[offset + tag->byte_order->int32_order[0]] = (uint8_t)((val >> 0 ) & 0xFF);
                tag->data[offset + tag->byte_order->int32_order[1]] = (uint8_t)((val >> 8 ) & 0xFF);
                tag->data[offset + tag->byte_order->int32_order[2]] = (uint8_t)((val >> 16) & 0xFF);
                tag->data[offset + tag->byte_order->int32_order[3]] = (uint8_t)((val >> 24) & 0xFF);

                SET_STATUS(tag->status, PLCTAG_STATUS_OK);
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                SET_STATUS(tag->status, rc);
            }
        }
    } else {
        if(!val) {
            rc = plc_tag_set_bit(id, 0, 0);
        } else {
            rc = plc_tag_set_bit(id, 0, 1);
        }
    }

    rc_dec(tag);

    return rc;
}




LIB_EXPORT int32_t  plc_tag_get_int32(int32_t id, int offset)
{
    int32_t res = INT32_MIN;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return res;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return res;
    }

    if(!tag->is_bit) {
        critical_block(tag->api_mutex) {
            if((offset >= 0) && (offset + ((int)sizeof(int32_t)) <= tag->size)) {
                res = (int32_t)(((uint32_t)(tag->data[offset + tag->byte_order->int32_order[0]]) << 0 ) +
                                ((uint32_t)(tag->data[offset + tag->byte_order->int32_order[1]]) << 8 ) +
                                ((uint32_t)(tag->data[offset + tag->byte_order->int32_order[2]]) << 16) +
                                ((uint32_t)(tag->data[offset + tag->byte_order->int32_order[3]]) << 24));

                SET_STATUS(tag->status, PLCTAG_STATUS_OK);
            }  else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                SET_STATUS(tag->status, PLCTAG_ERR_OUT_OF_BOUNDS);
            }
        }
    } else {
        int rc = plc_tag_get_bit(id, tag->bit);

        /* make sure the response is good. */
        if(rc >= 0) {
            res = (int32_t)rc;
        }
    }

    rc_dec(tag);

    return res;
}



LIB_EXPORT int plc_tag_set_int32(int32_t id, int offset, int32_t ival)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);
    uint32_t val = (uint32_t)ival;

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return PLCTAG_ERR_NO_DATA;
    }

    if(!tag->is_bit) {
        critical_block(tag->api_mutex) {
            if((offset >= 0) && (offset + ((int)sizeof(int32_t)) <= tag->size)) {
                if(tag->auto_sync_write_ms > 0) {
                    tag->tag_is_dirty = 1;
                }

                tag->data[offset + tag->byte_order->int32_order[0]] = (uint8_t)((val >> 0 ) & 0xFF);
                tag->data[offset + tag->byte_order->int32_order[1]] = (uint8_t)((val >> 8 ) & 0xFF);
                tag->data[offset + tag->byte_order->int32_order[2]] = (uint8_t)((val >> 16) & 0xFF);
                tag->data[offset + tag->byte_order->int32_order[3]] = (uint8_t)((val >> 24) & 0xFF);

                SET_STATUS(tag->status, PLCTAG_STATUS_OK);
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                SET_STATUS(tag->status, PLCTAG_ERR_OUT_OF_BOUNDS);
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            }
        }
    } else {
        if(!val) {
            rc = plc_tag_set_bit(id, 0, 0);
        } else {
            rc = plc_tag_set_bit(id, 0, 1);
        }
    }

    rc_dec(tag);

    return rc;
}







LIB_EXPORT uint16_t plc_tag_get_uint16(int32_t id, int offset)
{
    uint16_t res = UINT16_MAX;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return res;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return res;
    }

    if(!tag->is_bit) {
        critical_block(tag->api_mutex) {
            if((offset >= 0) && (offset + ((int)sizeof(uint16_t)) <= tag->size)) {
                res =   (uint16_t)(((uint16_t)(tag->data[offset + tag->byte_order->int16_order[0]]) << 0 ) +
                                   ((uint16_t)(tag->data[offset + tag->byte_order->int16_order[1]]) << 8 ));

                SET_STATUS(tag->status, PLCTAG_STATUS_OK);
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                SET_STATUS(tag->status, PLCTAG_ERR_OUT_OF_BOUNDS);
            }
        }
    } else {
        int rc = plc_tag_get_bit(id, tag->bit);

        /* make sure the response is good. */
        if(rc >= 0) {
            res = (uint16_t)(unsigned int)rc;
        }
    }

    rc_dec(tag);

    return res;
}




LIB_EXPORT int plc_tag_set_uint16(int32_t id, int offset, uint16_t val)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return PLCTAG_ERR_NO_DATA;
    }

    if(!tag->is_bit) {
        critical_block(tag->api_mutex) {
            if((offset >= 0) && (offset + ((int)sizeof(uint16_t)) <= tag->size)) {
                if(tag->auto_sync_write_ms > 0) {
                    tag->tag_is_dirty = 1;
                }

                tag->data[offset + tag->byte_order->int16_order[0]] = (uint8_t)((val >> 0 ) & 0xFF);
                tag->data[offset + tag->byte_order->int16_order[1]] = (uint8_t)((val >> 8 ) & 0xFF);

                SET_STATUS(tag->status, PLCTAG_STATUS_OK);
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                SET_STATUS(tag->status, PLCTAG_ERR_OUT_OF_BOUNDS);
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            }
        }
    } else {
        if(!val) {
            rc = plc_tag_set_bit(id, 0, 0);
        } else {
            rc = plc_tag_set_bit(id, 0, 1);
        }
    }

    rc_dec(tag);

    return rc;
}









LIB_EXPORT int16_t  plc_tag_get_int16(int32_t id, int offset)
{
    int16_t res = INT16_MIN;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return res;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return res;
    }

    if(!tag->is_bit) {
        critical_block(tag->api_mutex) {
            if((offset >= 0) && (offset + ((int)sizeof(int16_t)) <= tag->size)) {
                res =   (int16_t)(uint16_t)(((uint16_t)(tag->data[offset + tag->byte_order->int16_order[0]]) << 0 ) +
                                            ((uint16_t)(tag->data[offset + tag->byte_order->int16_order[1]]) << 8 ));
                SET_STATUS(tag->status, PLCTAG_STATUS_OK);
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                SET_STATUS(tag->status, PLCTAG_ERR_OUT_OF_BOUNDS);
            }
        }
    } else {
        int rc = plc_tag_get_bit(id, tag->bit);

        /* make sure the response is good. */
        if(rc >= 0) {
            res = (int16_t)(uint16_t)(unsigned int)rc;
        }
    }

    rc_dec(tag);

    return res;
}




LIB_EXPORT int plc_tag_set_int16(int32_t id, int offset, int16_t ival)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);
    uint16_t val = (uint16_t)ival;

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return PLCTAG_ERR_NO_DATA;
    }

    if(!tag->is_bit) {
        critical_block(tag->api_mutex) {
            if((offset >= 0) && (offset + ((int)sizeof(int16_t)) <= tag->size)) {
                if(tag->auto_sync_write_ms > 0) {
                    tag->tag_is_dirty = 1;
                }

                tag->data[offset + tag->byte_order->int16_order[0]] = (uint8_t)((val >> 0 ) & 0xFF);
                tag->data[offset + tag->byte_order->int16_order[1]] = (uint8_t)((val >> 8 ) & 0xFF);

                SET_STATUS(tag->status, PLCTAG_STATUS_OK);
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                SET_STATUS(tag->status, PLCTAG_ERR_OUT_OF_BOUNDS);
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            }
        }
    } else {
        if(!val) {
            rc = plc_tag_set_bit(id, 0, 0);
        } else {
            rc = plc_tag_set_bit(id, 0, 1);
        }
    }

    rc_dec(tag);

    return rc;
}








LIB_EXPORT uint8_t plc_tag_get_uint8(int32_t id, int offset)
{
    uint8_t res = UINT8_MAX;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return res;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return res;
    }

    if(!tag->is_bit) {
        critical_block(tag->api_mutex) {
            if((offset >= 0) && (offset + ((int)sizeof(uint8_t)) <= tag->size)) {
                res = tag->data[offset];
                SET_STATUS(tag->status, PLCTAG_STATUS_OK);
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                SET_STATUS(tag->status, PLCTAG_ERR_OUT_OF_BOUNDS);
            }
        }
    } else {
        int rc = plc_tag_get_bit(id, tag->bit);

        /* make sure the response is good. */
        if(rc >= 0) {
            res = (uint8_t)(unsigned int)rc;
        }
    }

    rc_dec(tag);

    return res;
}




LIB_EXPORT int plc_tag_set_uint8(int32_t id, int offset, uint8_t val)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return PLCTAG_ERR_NO_DATA;
    }

    if(!tag->is_bit) {
        critical_block(tag->api_mutex) {
            if((offset >= 0) && (offset + ((int)sizeof(uint8_t)) <= tag->size)) {
                if(tag->auto_sync_write_ms > 0) {
                    tag->tag_is_dirty = 1;
                }

                tag->data[offset] = val;

                SET_STATUS(tag->status, PLCTAG_STATUS_OK);
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                SET_STATUS(tag->status, PLCTAG_ERR_OUT_OF_BOUNDS);
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            }
        }
    } else {
        if(!val) {
            rc = plc_tag_set_bit(id, 0, 0);
        } else {
            rc = plc_tag_set_bit(id, 0, 1);
        }
    }

    rc_dec(tag);

    return rc;
}





LIB_EXPORT int8_t plc_tag_get_int8(int32_t id, int offset)
{
    int8_t res = INT8_MIN;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return res;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return res;
    }

    if(!tag->is_bit) {
        critical_block(tag->api_mutex) {
            if((offset >= 0) && (offset + ((int)sizeof(uint8_t)) <= tag->size)) {
                res =   (int8_t)tag->data[offset];
                SET_STATUS(tag->status, PLCTAG_STATUS_OK);
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                SET_STATUS(tag->status, PLCTAG_ERR_OUT_OF_BOUNDS);
            }
        }
    } else {
        int rc = plc_tag_get_bit(id, tag->bit);

        /* make sure the response is good. */
        if(rc >= 0) {
            res = (int8_t)(unsigned int)rc;
        }
    }

    rc_dec(tag);

    return res;
}




LIB_EXPORT int plc_tag_set_int8(int32_t id, int offset, int8_t ival)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);
    uint8_t val = (uint8_t)ival;

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return PLCTAG_ERR_NO_DATA;
    }

    if(!tag->is_bit) {
        critical_block(tag->api_mutex) {
            if((offset >= 0) && (offset + ((int)sizeof(int8_t)) <= tag->size)) {
                if(tag->auto_sync_write_ms > 0) {
                    tag->tag_is_dirty = 1;
                }

                tag->data[offset] = val;

                SET_STATUS(tag->status, PLCTAG_STATUS_OK);
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                SET_STATUS(tag->status, PLCTAG_ERR_OUT_OF_BOUNDS);
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            }
        }
    } else {
        if(!val) {
            rc = plc_tag_set_bit(id, 0, 0);
        } else {
            rc = plc_tag_set_bit(id, 0, 1);
        }
    }

    rc_dec(tag);

    return rc;
}






LIB_EXPORT double plc_tag_get_float64(int32_t id, int offset)
{
    double res = DBL_MIN;
    int rc = PLCTAG_STATUS_OK;
    uint64_t ures = 0;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return res;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return res;
    }

    if(tag->is_bit) {
        pdebug(DEBUG_WARN, "Getting float64 value is unsupported on a bit tag!");
        SET_STATUS(tag->status, PLCTAG_ERR_UNSUPPORTED);
        return res;
    }

    critical_block(tag->api_mutex) {
        if((offset >= 0) && (offset + ((int)sizeof(double)) <= tag->size)) {
            ures =  ((uint64_t)(tag->data[offset + tag->byte_order->float64_order[0]]) << 0 ) +
                    ((uint64_t)(tag->data[offset + tag->byte_order->float64_order[1]]) << 8 ) +
                    ((uint64_t)(tag->data[offset + tag->byte_order->float64_order[2]]) << 16) +
                    ((uint64_t)(tag->data[offset + tag->byte_order->float64_order[3]]) << 24) +
                    ((uint64_t)(tag->data[offset + tag->byte_order->float64_order[4]]) << 32) +
                    ((uint64_t)(tag->data[offset + tag->byte_order->float64_order[5]]) << 40) +
                    ((uint64_t)(tag->data[offset + tag->byte_order->float64_order[6]]) << 48) +
                    ((uint64_t)(tag->data[offset + tag->byte_order->float64_order[7]]) << 56);

            SET_STATUS(tag->status, PLCTAG_STATUS_OK);
            rc = PLCTAG_STATUS_OK;
        } else {
            pdebug(DEBUG_WARN, "Data offset out of bounds!");
            SET_STATUS(tag->status, PLCTAG_ERR_OUT_OF_BOUNDS);
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
        }
    }

    if(rc == PLCTAG_STATUS_OK) {
        /* copy the data */
        mem_copy(&res,&ures,sizeof(res));
    } else {
        res = DBL_MIN;
    }

    rc_dec(tag);

    return res;
}




LIB_EXPORT int plc_tag_set_float64(int32_t id, int offset, double fval)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);
    uint64_t val = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return PLCTAG_ERR_NO_DATA;
    }

    if(tag->is_bit) {
        pdebug(DEBUG_WARN, "Setting float64 value is unsupported on a bit tag!");
        SET_STATUS(tag->status, PLCTAG_ERR_UNSUPPORTED);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* copy the data into the uint64 value */
    mem_copy(&val, &fval, sizeof(val));

    critical_block(tag->api_mutex) {
        if((offset >= 0) && (offset + ((int)sizeof(uint64_t)) <= tag->size)) {
            if(tag->auto_sync_write_ms > 0) {
                tag->tag_is_dirty = 1;
            }

            tag->data[offset + tag->byte_order->float64_order[0]] = (uint8_t)((val >> 0 ) & 0xFF);
            tag->data[offset + tag->byte_order->float64_order[1]] = (uint8_t)((val >> 8 ) & 0xFF);
            tag->data[offset + tag->byte_order->float64_order[2]] = (uint8_t)((val >> 16) & 0xFF);
            tag->data[offset + tag->byte_order->float64_order[3]] = (uint8_t)((val >> 24) & 0xFF);
            tag->data[offset + tag->byte_order->float64_order[4]] = (uint8_t)((val >> 32) & 0xFF);
            tag->data[offset + tag->byte_order->float64_order[5]] = (uint8_t)((val >> 40) & 0xFF);
            tag->data[offset + tag->byte_order->float64_order[6]] = (uint8_t)((val >> 48) & 0xFF);
            tag->data[offset + tag->byte_order->float64_order[7]] = (uint8_t)((val >> 56) & 0xFF);

            SET_STATUS(tag->status, PLCTAG_STATUS_OK);
        } else {
            pdebug(DEBUG_WARN, "Data offset out of bounds!");
            SET_STATUS(tag->status, PLCTAG_ERR_OUT_OF_BOUNDS);
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
        }
    }

    rc_dec(tag);

    return rc;
}



LIB_EXPORT float plc_tag_get_float32(int32_t id, int offset)
{
    float res = FLT_MIN;
    int rc = PLCTAG_STATUS_OK;
    uint32_t ures = 0;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return res;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return res;
    }

    if(tag->is_bit) {
        pdebug(DEBUG_WARN, "Getting float32 value is unsupported on a bit tag!");
        SET_STATUS(tag->status, PLCTAG_ERR_UNSUPPORTED);
        return res;
    }

    critical_block(tag->api_mutex) {
        if((offset >= 0) && (offset + ((int)sizeof(float)) <= tag->size)) {
            ures =  (uint32_t)(((uint32_t)(tag->data[offset + tag->byte_order->float32_order[0]]) << 0 ) +
                               ((uint32_t)(tag->data[offset + tag->byte_order->float32_order[1]]) << 8 ) +
                               ((uint32_t)(tag->data[offset + tag->byte_order->float32_order[2]]) << 16) +
                               ((uint32_t)(tag->data[offset + tag->byte_order->float32_order[3]]) << 24));

            SET_STATUS(tag->status, PLCTAG_STATUS_OK);
            rc = PLCTAG_STATUS_OK;
        } else {
            pdebug(DEBUG_WARN, "Data offset out of bounds!");
            SET_STATUS(tag->status, PLCTAG_ERR_OUT_OF_BOUNDS);
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
        }
    }

    if(rc == PLCTAG_STATUS_OK) {
        /* copy the data */
        mem_copy(&res,&ures,sizeof(res));
    } else {
        res = FLT_MIN;
    }

    rc_dec(tag);

    return res;
}




LIB_EXPORT int plc_tag_set_float32(int32_t id, int offset, float fval)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);
    uint32_t val = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return PLCTAG_ERR_NO_DATA;
    }

    if(tag->is_bit) {
        pdebug(DEBUG_WARN, "Setting float32 value is unsupported on a bit tag!");
        SET_STATUS(tag->status, PLCTAG_ERR_UNSUPPORTED);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* copy the data into the uint64 value */
    mem_copy(&val, &fval, sizeof(val));

    critical_block(tag->api_mutex) {
        if((offset >= 0) && (offset + ((int)sizeof(float)) <= tag->size)) {
            if(tag->auto_sync_write_ms > 0) {
                tag->tag_is_dirty = 1;
            }

            tag->data[offset + tag->byte_order->float32_order[0]] = (uint8_t)((val >> 0 ) & 0xFF);
            tag->data[offset + tag->byte_order->float32_order[1]] = (uint8_t)((val >> 8 ) & 0xFF);
            tag->data[offset + tag->byte_order->float32_order[2]] = (uint8_t)((val >> 16) & 0xFF);
            tag->data[offset + tag->byte_order->float32_order[3]] = (uint8_t)((val >> 24) & 0xFF);

            SET_STATUS(tag->status, PLCTAG_STATUS_OK);
        } else {
            pdebug(DEBUG_WARN, "Data offset out of bounds!");
            SET_STATUS(tag->status, PLCTAG_ERR_OUT_OF_BOUNDS);
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
        }
    }

    rc_dec(tag);

    return rc;
}


LIB_EXPORT int plc_tag_get_string(int32_t tag_id, int string_start_offset, char *buffer, int buffer_length)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(tag_id);
    int max_len = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* are strings defined for this tag? */
    if(!tag->byte_order || !tag->byte_order->str_is_defined) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no definitions for strings!");
        SET_STATUS(tag->status, PLCTAG_ERR_UNSUPPORTED);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return PLCTAG_ERR_NO_DATA;
    }

    if(tag->is_bit) {
        rc_dec(tag);
        pdebug(DEBUG_WARN, "Getting a string value from a bit tag is not supported!");
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* set all buffer bytes to zero. */
    mem_set(buffer, 0, buffer_length);

    critical_block(tag->api_mutex) {
        int string_length = get_string_length_unsafe(tag, string_start_offset);

        /* determine the maximum number of characters/bytes to copy. */
        if(buffer_length < string_length) {
            pdebug(DEBUG_WARN, "Buffer length, %d, is less than the string length, %d!", buffer_length, string_length);
            max_len = buffer_length;
        } else {
            max_len = string_length;
        }

        /* check the amount of space. */
        if(string_start_offset + (int)tag->byte_order->str_count_word_bytes + max_len <= tag->size) {
            for(int i = 0; i < max_len; i++) {
                size_t char_index = ((size_t)(unsigned int)i ^ (tag->byte_order->str_is_byte_swapped ? (size_t)1 : (size_t)0)) /* byte swap if necessary */
                                  + (size_t)(unsigned int)string_start_offset
                                  + (size_t)(unsigned int)(tag->byte_order->str_count_word_bytes);
                buffer[i] = (char)tag->data[char_index];
            }

            SET_STATUS(tag->status, PLCTAG_STATUS_OK);
            rc = PLCTAG_STATUS_OK;
        } else {
            pdebug(DEBUG_WARN, "Data offset out of bounds!");
            SET_STATUS(tag->status, PLCTAG_ERR_OUT_OF_BOUNDS);
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
        }
    }

    rc_dec(tag);

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}


LIB_EXPORT int plc_tag_set_string(int32_t tag_id, int string_start_offset, const char *string_val)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(tag_id);
    int string_length = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return PLCTAG_ERR_NO_DATA;
    }

    /* are strings defined for this tag? */
    if(!tag->byte_order || !tag->byte_order->str_is_defined) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no definitions for strings!");
        SET_STATUS(tag->status, PLCTAG_ERR_UNSUPPORTED);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    if(!string_val) {
        rc_dec(tag);
        pdebug(DEBUG_WARN, "New string value pointer is null!");
        SET_STATUS(tag->status, PLCTAG_ERR_NULL_PTR);
        return PLCTAG_ERR_NULL_PTR;
    }

    /* note that passing a zero-length string is valid. */

    if(tag->is_bit) {
        rc_dec(tag);
        pdebug(DEBUG_WARN, "Setting a string value on a bit tag is not supported!");
        SET_STATUS(tag->status, PLCTAG_ERR_UNSUPPORTED);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    string_length = str_length(string_val);

    critical_block(tag->api_mutex) {
        int string_capacity = (tag->byte_order->str_max_capacity ? (int)(tag->byte_order->str_max_capacity) : get_string_length_unsafe(tag, string_start_offset));

        /* determine the maximum number of characters/bytes to copy. */
        if(string_capacity >= string_length) {
            /* is there sufficient room in the tag? */
            if(string_start_offset + (int)(tag->byte_order->str_count_word_bytes) + string_length + (tag->byte_order->str_is_zero_terminated ? 1 : 0) <= tag->size) {
                /* set the status, we might change it below. */
                rc = PLCTAG_STATUS_OK;
                SET_STATUS(tag->status, rc);

                /* copy the string data into the tag. */
                for(int i = 0; i < string_length; i++) {
                    size_t char_index = (((size_t)(unsigned int)i) ^ (tag->byte_order->str_is_byte_swapped ? (size_t)1 : (size_t)0)) /* byte swap if necessary */
                                    + (size_t)(unsigned int)string_start_offset
                                    + (size_t)(unsigned int)(tag->byte_order->str_count_word_bytes);
                    tag->data[char_index] = (uint8_t)string_val[i];
                }

                /* zero pad the rest. */
                for(int i = string_length; i < string_capacity; i++) {
                    size_t char_index = (((size_t)(unsigned int)i) ^ (tag->byte_order->str_is_byte_swapped ? (size_t)1 : (size_t)0)) /* byte swap if necessary */
                                    + (size_t)(unsigned int)string_start_offset
                                    + (size_t)(unsigned int)(tag->byte_order->str_count_word_bytes);
                    tag->data[char_index] = (uint8_t)0;
                }

                /* if the string is counted, set the length */
                if(tag->byte_order->str_is_counted) {
                    switch(tag->byte_order->str_count_word_bytes) {
                        case 1:
                            tag->data[string_start_offset] = (uint8_t)(unsigned int)string_length;
                            break;

                        case 2:
                            tag->data[string_start_offset + tag->byte_order->int16_order[0]] = (uint8_t)((((unsigned int)string_length) >> 0 ) & 0xFF);
                            tag->data[string_start_offset + tag->byte_order->int16_order[1]] = (uint8_t)((((unsigned int)string_length) >> 8 ) & 0xFF);
                            break;

                        case 4:
                            tag->data[string_start_offset + tag->byte_order->int32_order[0]] = (uint8_t)((((unsigned int)string_length) >> 0 ) & 0xFF);
                            tag->data[string_start_offset + tag->byte_order->int32_order[1]] = (uint8_t)((((unsigned int)string_length) >> 8 ) & 0xFF);
                            tag->data[string_start_offset + tag->byte_order->int32_order[2]] = (uint8_t)((((unsigned int)string_length) >> 16) & 0xFF);
                            tag->data[string_start_offset + tag->byte_order->int32_order[3]] = (uint8_t)((((unsigned int)string_length) >> 24) & 0xFF);
                            break;

                        default:
                            pdebug(DEBUG_WARN, "Unsupported string count size, %d!", tag->byte_order->str_count_word_bytes);
                            rc = PLCTAG_ERR_UNSUPPORTED;
                            SET_STATUS(tag->status, rc);
                            break;
                    }
                }

                if(rc == PLCTAG_STATUS_OK && tag->auto_sync_write_ms > 0) {
                    tag->tag_is_dirty = 1;
                }
            } else {
                pdebug(DEBUG_WARN, "Writing the full string would go out of bounds in the tag buffer!");
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                SET_STATUS(tag->status, rc);
            }
        } else {
            pdebug(DEBUG_WARN, "String capacity, %d, is less than the string length, %d!", string_capacity, string_length);
            rc = PLCTAG_ERR_TOO_LARGE;
            SET_STATUS(tag->status, rc);
        }
    }

    rc_dec(tag);

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}




LIB_EXPORT int plc_tag_get_string_capacity(int32_t id, int string_start_offset)
{
    int string_capacity = 0;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* are strings defined for this tag? */
    if(!tag->byte_order || !tag->byte_order->str_is_defined) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no definitions for strings!");
        SET_STATUS(tag->status, PLCTAG_ERR_UNSUPPORTED);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return PLCTAG_ERR_NO_DATA;
    }

    if(tag->is_bit) {
        rc_dec(tag);
        pdebug(DEBUG_WARN, "Getting string capacity from a bit tag is not supported!");
        SET_STATUS(tag->status, PLCTAG_ERR_UNSUPPORTED);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    critical_block(tag->api_mutex) {
        string_capacity = (tag->byte_order->str_max_capacity ? (int)(tag->byte_order->str_max_capacity) : get_string_length_unsafe(tag, string_start_offset));
    }

    rc_dec(tag);

    pdebug(DEBUG_SPEW, "Done.");

    return string_capacity;
}



LIB_EXPORT int plc_tag_get_string_length(int32_t id, int string_start_offset)
{
    int string_length = 0;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* are strings defined for this tag? */
    if(!tag->byte_order || !tag->byte_order->str_is_defined) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no definitions for strings!");
        SET_STATUS(tag->status, PLCTAG_ERR_UNSUPPORTED);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return PLCTAG_ERR_NO_DATA;
    }

    if(tag->is_bit) {
        rc_dec(tag);
        pdebug(DEBUG_WARN, "Getting string length from a bit tag is not supported!");
        SET_STATUS(tag->status, PLCTAG_ERR_UNSUPPORTED);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    critical_block(tag->api_mutex) {
        string_length = get_string_length_unsafe(tag, string_start_offset);
    }

    rc_dec(tag);

    pdebug(DEBUG_SPEW, "Done.");

    return string_length;
}




LIB_EXPORT int plc_tag_get_string_total_length(int32_t id, int string_start_offset)
{
    int total_length = 0;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* are strings defined for this tag? */
    if(!tag->byte_order || !tag->byte_order->str_is_defined) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no definitions for strings!");
        SET_STATUS(tag->status, PLCTAG_ERR_UNSUPPORTED);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* is there data? */
    if(!tag->data) {
        rc_dec(tag);
        pdebug(DEBUG_WARN,"Tag has no data!");
        SET_STATUS(tag->status, PLCTAG_ERR_NO_DATA);
        return PLCTAG_ERR_NO_DATA;
    }

    if(tag->is_bit) {
        rc_dec(tag);
        pdebug(DEBUG_WARN, "Getting a string total length from a bit tag is not supported!");
        SET_STATUS(tag->status, PLCTAG_ERR_UNSUPPORTED);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    critical_block(tag->api_mutex) {
        total_length = (int)(tag->byte_order->str_count_word_bytes)
                     + (tag->byte_order->str_is_fixed_length ? (int)(tag->byte_order->str_max_capacity) : get_string_length_unsafe(tag, string_start_offset))
                     + (int)(tag->byte_order->str_pad_bytes);
    }

    rc_dec(tag);

    pdebug(DEBUG_SPEW, "Done.");

    return total_length;
}




/*****************************************************************************************************
 *****************************  Support routines for extra indirection *******************************
 ****************************************************************************************************/

int set_tag_byte_order(plc_tag_p tag, attr attribs)

{
    int use_default = 1;

    pdebug(DEBUG_INFO, "Starting.");

    /* the default values are already set in the tag. */

    /* check for overrides. */
    if(attr_get_str(attribs,  "int16_byte_order", NULL) != NULL) {
        use_default = 0;
    }

    if(attr_get_str(attribs,  "int32_byte_order", NULL) != NULL) {
        use_default = 0;
    }

    if(attr_get_str(attribs,  "int64_byte_order", NULL) != NULL) {
        use_default = 0;
    }

    if(attr_get_str(attribs,  "float32_byte_order", NULL) != NULL) {
        use_default = 0;
    }

    if(attr_get_str(attribs,  "float64_byte_order", NULL) != NULL) {
        use_default = 0;
    }

    if(attr_get_str(attribs, "str_is_counted", NULL) != NULL) {
        use_default = 0;
    }

    if(attr_get_str(attribs, "str_is_fixed_length", NULL) != NULL) {
        use_default = 0;
    }

    if(attr_get_str(attribs, "str_is_zero_terminated", NULL) != NULL) {
        use_default = 0;
    }

    if(attr_get_str(attribs, "str_is_byte_swapped", NULL) != NULL) {
        use_default = 0;
    }

    if(attr_get_str(attribs, "str_count_word_bytes", NULL) != NULL) {
        use_default = 0;
    }

    if(attr_get_str(attribs, "str_max_capacity", NULL) != NULL) {
        use_default = 0;
    }

    if(attr_get_str(attribs, "str_total_length", NULL) != NULL) {
        use_default = 0;
    }

    if(attr_get_str(attribs, "str_pad_bytes", NULL) != NULL) {
        use_default = 0;
    }

    /* if we need to override something, build a new byte order structure. */
    if(!use_default) {
        const char *byte_order_str = NULL;
        int str_param = 0;
        int rc = PLCTAG_STATUS_OK;
        tag_byte_order_t *new_byte_order = mem_alloc((int)(unsigned int)sizeof(*(tag->byte_order)));

        if(!new_byte_order) {
            pdebug(DEBUG_WARN, "Unable to allocate byte order struct for tag!");
            return PLCTAG_ERR_NO_MEM;
        }

        /* copy the defaults. */
        *new_byte_order = *(tag->byte_order);

        /* replace the old byte order. */
        tag->byte_order = new_byte_order;

        /* mark it as allocated so that we free it later. */
        tag->byte_order->is_allocated = 1;

        /* 16-bit ints. */
        byte_order_str = attr_get_str(attribs, "int16_byte_order", NULL);
        if(byte_order_str) {
            pdebug(DEBUG_DETAIL, "Override byte order int16_byte_order=%s", byte_order_str);

            rc = check_byte_order_str(byte_order_str, 2);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Byte order string int16_byte_order, \"%s\", is illegal or malformed.", byte_order_str);
                return rc;
            }

            /* strange gyrations to make the compiler happy.   MSVC will probably complain. */
            tag->byte_order->int16_order[0] = (int)(unsigned int)(((unsigned int)byte_order_str[0] - (unsigned int)('0')) & 0x01);
            tag->byte_order->int16_order[1] = (int)(unsigned int)(((unsigned int)byte_order_str[1] - (unsigned int)('0')) & 0x01);
        }

        /* 32-bit ints. */
        byte_order_str = attr_get_str(attribs, "int32_byte_order", NULL);
        if(byte_order_str) {
            pdebug(DEBUG_DETAIL, "Override byte order int32_byte_order=%s", byte_order_str);

            rc = check_byte_order_str(byte_order_str, 4);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Byte order string int32_byte_order, \"%s\", is illegal or malformed.", byte_order_str);
                return rc;
            }

            tag->byte_order->int32_order[0] = (int)(unsigned int)(((unsigned int)byte_order_str[0] - (unsigned int)('0')) & 0x03);
            tag->byte_order->int32_order[1] = (int)(unsigned int)(((unsigned int)byte_order_str[1] - (unsigned int)('0')) & 0x03);
            tag->byte_order->int32_order[2] = (int)(unsigned int)(((unsigned int)byte_order_str[2] - (unsigned int)('0')) & 0x03);
            tag->byte_order->int32_order[3] = (int)(unsigned int)(((unsigned int)byte_order_str[3] - (unsigned int)('0')) & 0x03);
        }

        /* 64-bit ints. */
        byte_order_str = attr_get_str(attribs, "int64_byte_order", NULL);
        if(byte_order_str) {
            pdebug(DEBUG_DETAIL, "Override byte order int64_byte_order=%s", byte_order_str);

            rc = check_byte_order_str(byte_order_str, 8);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Byte order string int64_byte_order, \"%s\", is illegal or malformed.", byte_order_str);
                return rc;
            }

            tag->byte_order->int64_order[0] = (int)(unsigned int)(((unsigned int)byte_order_str[0] - (unsigned int)('0')) & 0x07);
            tag->byte_order->int64_order[1] = (int)(unsigned int)(((unsigned int)byte_order_str[1] - (unsigned int)('0')) & 0x07);
            tag->byte_order->int64_order[2] = (int)(unsigned int)(((unsigned int)byte_order_str[2] - (unsigned int)('0')) & 0x07);
            tag->byte_order->int64_order[3] = (int)(unsigned int)(((unsigned int)byte_order_str[3] - (unsigned int)('0')) & 0x07);
            tag->byte_order->int64_order[4] = (int)(unsigned int)(((unsigned int)byte_order_str[4] - (unsigned int)('0')) & 0x07);
            tag->byte_order->int64_order[5] = (int)(unsigned int)(((unsigned int)byte_order_str[5] - (unsigned int)('0')) & 0x07);
            tag->byte_order->int64_order[6] = (int)(unsigned int)(((unsigned int)byte_order_str[6] - (unsigned int)('0')) & 0x07);
            tag->byte_order->int64_order[7] = (int)(unsigned int)(((unsigned int)byte_order_str[7] - (unsigned int)('0')) & 0x07);
        }

        /* 32-bit floats. */
        byte_order_str = attr_get_str(attribs, "float32_byte_order", NULL);
        if(byte_order_str) {
            pdebug(DEBUG_DETAIL, "Override byte order float32_byte_order=%s", byte_order_str);

            rc = check_byte_order_str(byte_order_str, 4);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Byte order string float32_byte_order, \"%s\", is illegal or malformed.", byte_order_str);
                return rc;
            }

            tag->byte_order->float32_order[0] = (int)(unsigned int)(((unsigned int)byte_order_str[0] - (unsigned int)('0')) & 0x03);
            tag->byte_order->float32_order[1] = (int)(unsigned int)(((unsigned int)byte_order_str[1] - (unsigned int)('0')) & 0x03);
            tag->byte_order->float32_order[2] = (int)(unsigned int)(((unsigned int)byte_order_str[2] - (unsigned int)('0')) & 0x03);
            tag->byte_order->float32_order[3] = (int)(unsigned int)(((unsigned int)byte_order_str[3] - (unsigned int)('0')) & 0x03);
        }

        /* 64-bit floats */
        byte_order_str = attr_get_str(attribs, "float64_byte_order", NULL);
        if(byte_order_str) {
            pdebug(DEBUG_DETAIL, "Override byte order float64_byte_order=%s", byte_order_str);

            rc = check_byte_order_str(byte_order_str, 8);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Byte order string float64_byte_order, \"%s\", is illegal or malformed.", byte_order_str);
                return rc;
            }

            tag->byte_order->float64_order[0] = (int)(unsigned int)(((unsigned int)byte_order_str[0] - (unsigned int)('0')) & 0x07);
            tag->byte_order->float64_order[1] = (int)(unsigned int)(((unsigned int)byte_order_str[1] - (unsigned int)('0')) & 0x07);
            tag->byte_order->float64_order[2] = (int)(unsigned int)(((unsigned int)byte_order_str[2] - (unsigned int)('0')) & 0x07);
            tag->byte_order->float64_order[3] = (int)(unsigned int)(((unsigned int)byte_order_str[3] - (unsigned int)('0')) & 0x07);
            tag->byte_order->float64_order[4] = (int)(unsigned int)(((unsigned int)byte_order_str[4] - (unsigned int)('0')) & 0x07);
            tag->byte_order->float64_order[5] = (int)(unsigned int)(((unsigned int)byte_order_str[5] - (unsigned int)('0')) & 0x07);
            tag->byte_order->float64_order[6] = (int)(unsigned int)(((unsigned int)byte_order_str[6] - (unsigned int)('0')) & 0x07);
            tag->byte_order->float64_order[7] = (int)(unsigned int)(((unsigned int)byte_order_str[7] - (unsigned int)('0')) & 0x07);
        }

        /* string information. */

        /* is the string counted? */
        if(attr_get_str(attribs, "str_is_counted", NULL)) {
            str_param = attr_get_int(attribs, "str_is_counted", 0);
            if(str_param == 1 || str_param == 0) {
                tag->byte_order->str_is_counted = (str_param ? 1 : 0);
            } else {
                pdebug(DEBUG_WARN, "Tag string attribute str_is_counted must be missing, zero (0) or one (1)!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* is the string a fixed length? */
        if(attr_get_str(attribs, "str_is_fixed_length", NULL)) {
            str_param = attr_get_int(attribs, "str_is_fixed_length", 0);
            if(str_param == 1 || str_param == 0) {
                tag->byte_order->str_is_fixed_length = (str_param ? 1: 0);
            } else {
                pdebug(DEBUG_WARN, "Tag string attribute str_is_fixed_length must be missing, zero (0) or one (1)!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* is the string zero terminated? */
        if(attr_get_str(attribs, "str_is_zero_terminated", NULL)) {
            str_param = attr_get_int(attribs, "str_is_zero_terminated", 0);
            if(str_param == 1 || str_param == 0) {
                tag->byte_order->str_is_zero_terminated = (str_param ? 1 : 0);
            } else {
                pdebug(DEBUG_WARN, "Tag string attribute str_is_zero_terminated must be missing, zero (0) or one (1)!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* is the string byteswapped like PLC/5? */
        if(attr_get_str(attribs, "str_is_byte_swapped", NULL)) {
            str_param = attr_get_int(attribs, "str_is_byte_swapped", 0);
            if(str_param == 1 || str_param == 0) {
                tag->byte_order->str_is_byte_swapped = (str_param ? 1 : 0);
            } else {
                pdebug(DEBUG_WARN, "Tag string attribute str_is_byte_swapped must be missing, zero (0) or one (1)!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* main string parameters. */

        /* how many bytes is the string count word? */
        if(attr_get_str(attribs, "str_count_word_bytes", NULL)) {
            str_param = attr_get_int(attribs, "str_count_word_bytes", 0);
            if(str_param == 0 || str_param == 1 || str_param == 2 || str_param == 4) {
                tag->byte_order->str_count_word_bytes = str_param;
            } else {
                pdebug(DEBUG_WARN, "Tag string attribute str_count_word_bytes must be missing, 0, 1, 2, or 4!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* What is the string maximum capacity */
        if(attr_get_str(attribs, "str_max_capacity", NULL)) {
            str_param = attr_get_int(attribs, "str_max_capacity", 0);
            if(str_param >= 0) {
                tag->byte_order->str_max_capacity = str_param;
            } else {
                pdebug(DEBUG_WARN, "Tag string attribute str_max_capacity must be missing, 0, or positive!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* What is the string total length */
        if(attr_get_str(attribs, "str_total_length", NULL)) {
            str_param = attr_get_int(attribs, "str_total_length", 0);
            if(str_param >= 0) {
                tag->byte_order->str_total_length = str_param;
            } else {
                pdebug(DEBUG_WARN, "Tag string attribute str_total_length must be missing, 0, or positive!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* What is the string padding length */
        if(attr_get_str(attribs, "str_pad_bytes", NULL)) {
            str_param = attr_get_int(attribs, "str_pad_bytes", 0);
            if(str_param >= 0) {
                tag->byte_order->str_pad_bytes = str_param;
            } else {
                pdebug(DEBUG_WARN, "Tag string attribute str_pad_bytes must be missing, 0, or positive!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* now make sure that the combination of settings works. */

        /* if we have a counted string, we need the count! */
        if(tag->byte_order->str_is_counted) {
            if(tag->byte_order->str_count_word_bytes == 0) {
                pdebug(DEBUG_WARN, "If a string definition is counted, you must use both \"str_is_counted\" and \"str_count_word_bytes\" parameters!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* if we have a fixed length string, we need to know what the length is! */
        if(tag->byte_order->str_is_fixed_length) {
            if(tag->byte_order->str_total_length == 0) {
                pdebug(DEBUG_WARN, "If a string definition is fixed length, you must use both \"str_is_fixed_length\" and \"str_total_length\" parameters!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* check the total length. */
        if(tag->byte_order->str_total_length > 0
          && (tag->byte_order->str_is_zero_terminated
            + tag->byte_order->str_max_capacity
            + tag->byte_order->str_count_word_bytes
            + tag->byte_order->str_pad_bytes)
          > tag->byte_order->str_total_length)
        {
            pdebug(DEBUG_WARN, "Tag string total length must be at least the sum of the other string components!");
            return PLCTAG_ERR_BAD_PARAM;
        }

        /* Do we have enough of a definition for a string? */
        /* FIXME - This is probably not enough checking! */
        if(tag->byte_order->str_is_counted || tag->byte_order->str_is_zero_terminated) {
            tag->byte_order->str_is_defined = 1;
        } else {
            pdebug(DEBUG_WARN, "Insufficient definitions found to support strings!");
        }
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}

int check_byte_order_str(const char *byte_order, int length)
{
    int taken[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int byte_order_len = str_length(byte_order);

    pdebug(DEBUG_DETAIL, "Starting.");

    /* check the size. */
    if(byte_order_len != length) {
        pdebug(DEBUG_WARN, "Byte order string, \"%s\", must be %d characters long!", byte_order, length);
        return (byte_order_len < length ? PLCTAG_ERR_TOO_SMALL : PLCTAG_ERR_TOO_LARGE);
    }

    /* check each character. */
    for(int i=0; i < byte_order_len; i++) {
        int val = 0;

        if(!isdigit(byte_order[i]) || byte_order[i] < '0' || byte_order[i] > '7') {
            pdebug(DEBUG_WARN, "Byte order string, \"%s\", must be only characters from '0' to '7'!", byte_order);
            return PLCTAG_ERR_BAD_DATA;
        }

        /* get the numeric value. */
        val = byte_order[i] - '0';

        if(val < 0 || val > (length-1)) {
            pdebug(DEBUG_WARN, "Byte order string, \"%s\", must only values from 0 to %d!", byte_order, (length - 1));
            return PLCTAG_ERR_BAD_DATA;
        }

        if(taken[val]) {
            pdebug(DEBUG_WARN, "Byte order string, \"%s\", must use each digit exactly once!", byte_order);
            return PLCTAG_ERR_BAD_DATA;
        }

        taken[val] = 1;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}




plc_tag_p lookup_tag(int32_t tag_id)
{
    plc_tag_p tag = NULL;

    critical_block(tag_lookup_mutex) {
        tag = hashtable_get(tags, (int64_t)tag_id);

        if(tag) {
            debug_set_tag_id(tag->tag_id);
        } else {
            /* FIXME - remove this. */
            pdebug(DEBUG_WARN, "Tag with ID %d not found.", tag_id);
        }

        if(tag && tag->tag_id == tag_id) {
            pdebug(DEBUG_SPEW, "Found tag %p with id %d.", tag, tag->tag_id);
            tag = rc_inc(tag);
        } else {
            debug_set_tag_id(0);
            tag = NULL;
        }
    }

    return tag;
}



int tag_id_inc(int id)
{
    if(id <= 0) {
        pdebug(DEBUG_ERROR, "Incoming ID is not valid! Got %d",id);
        /* try to correct. */
        id = (TAG_ID_MASK/2);
    }

    id = (id + 1) & TAG_ID_MASK;

    if(id == 0) {
        id = 1; /* skip zero intentionally! Can't return an ID of zero because it looks like a NULL pointer */
    }

    return id;
}



int add_tag_lookup(plc_tag_p tag)
{
    int rc = PLCTAG_ERR_NOT_FOUND;
    int new_id = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    critical_block(tag_lookup_mutex) {
        int attempts = 0;

        /* only get this when we hold the mutex. */
        new_id = next_tag_id;

        do {
            new_id = tag_id_inc(new_id);

            if(new_id <=0) {
                pdebug(DEBUG_WARN,"ID %d is illegal!", new_id);
                attempts = MAX_TAG_MAP_ATTEMPTS;
                break;
            }

            pdebug(DEBUG_SPEW,"Trying new ID %d.", new_id);

            if(!hashtable_get(tags,(int64_t)new_id)) {
                pdebug(DEBUG_DETAIL,"Found unused ID %d", new_id);
                break;
            }

            attempts++;
        } while(attempts < MAX_TAG_MAP_ATTEMPTS);

        if(attempts < MAX_TAG_MAP_ATTEMPTS) {
            rc = hashtable_put(tags, (int64_t)new_id, tag);
        } else {
            rc = PLCTAG_ERR_NO_RESOURCES;
        }

        next_tag_id = new_id;
    }

    if(rc != PLCTAG_STATUS_OK) {
        new_id = rc;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return new_id;
}



/*
 * get the string count length depending on the PLC string type.
 *
 * This is called in other functions so is separated out.
 *
 * This must be called with the tag API mutex held!
 */

// int get_string_count_size_unsafe(plc_tag_p tag, int offset)
// {
//     int string_count_length = 0;
//     string_type_t string_type = tag->byte_order.string_type;

//     /* FIXME - do something with the offset! */
//     (void)offset;

//     switch(string_type) {
//         case STRING_AB_MICRO800:
//             string_count_length = 1; /* FIXME - is this correct? */
//             break;

//         case STRING_AB_PLC5:
//             string_count_length = 2;
//             break;

//         case STRING_AB_LGX:
//             string_count_length = 4;
//             break;

//         case STRING_AB_TAG_NAME:
//             string_count_length = 2;
//             break;

//         case STRING_ZERO_TERM:
//             string_count_length = 0;
//             break;

//         default:
//             pdebug(DEBUG_WARN, "Unsupported string type!");
//             string_count_length = PLCTAG_ERR_UNSUPPORTED;
//             break;
//     }

//     return string_count_length;
// }



/*
 * get the string length depending on the PLC string type.
 *
 * This is called in other functions so is separated out.
 *
 * This must be called with the tag API mutex held!
 */

int get_string_length_unsafe(plc_tag_p tag, int offset)
{
    int string_length = 0;

    if(tag->byte_order->str_is_counted) {
        switch(tag->byte_order->str_count_word_bytes) {
            case 1:
                string_length = (int)(unsigned int)(tag->data[offset]);
                break;

            case 2:
                string_length = (int16_t)(uint16_t)(((uint16_t)(tag->data[offset + tag->byte_order->int16_order[0]]) << 0 ) +
                                                    ((uint16_t)(tag->data[offset + tag->byte_order->int16_order[1]]) << 8 ));
                break;

            case 4:
                string_length = (int32_t)(((uint32_t)(tag->data[offset + tag->byte_order->int32_order[0]]) << 0 ) +
                                          ((uint32_t)(tag->data[offset + tag->byte_order->int32_order[1]]) << 8 ) +
                                          ((uint32_t)(tag->data[offset + tag->byte_order->int32_order[2]]) << 16) +
                                          ((uint32_t)(tag->data[offset + tag->byte_order->int32_order[3]]) << 24));
                break;

            default:
                pdebug(DEBUG_WARN, "Unsupported string count word size, %d bytes!", tag->byte_order->str_count_word_bytes);
                return 0; /* FIXME - this should be an error code. */
                break;
        }

    } else {
        if(tag->byte_order->str_is_zero_terminated) {
            /* slow, but hopefully correct. */
            for(int i = offset + (int)(tag->byte_order->str_count_word_bytes); i < tag->size; i++) {
                size_t char_index = (((size_t)(unsigned int)string_length) ^ (tag->byte_order->str_is_byte_swapped ? (size_t)1 : (size_t)0)) /* byte swap if necessary */
                                + (size_t)(unsigned int)offset
                                + (size_t)(unsigned int)(tag->byte_order->str_count_word_bytes);

                if(tag->data[char_index] == (uint8_t)0) {
                    /* found the end. */
                    break;
                }

                string_length++;
            }
        } else {
            /* it is not counted or zero terminated, so it is not supported. */
            pdebug(DEBUG_WARN, "Unsupported string length type.   Must be counted or zero-terminated!");
            return 0; /* FIXME this should be an error code. */
        }
    }

    return string_length;
}


/*
 * get the string capacity depending on the PLC string type.
 *
 * This is called in other functions so is separated out.
 *
 * This must be called with the tag API mutex held!
 */

// int get_string_capacity_unsafe(plc_tag_p tag, int offset)
// {
//     int string_capacity = 0;
//     string_type_t string_type = tag->byte_order.string_type;

//     /* FIXME - do something with the offset! */
//     (void)offset;

//     switch(string_type) {
//         case STRING_AB_MICRO800:
//             string_capacity = 255; /* FIXME - is this correct? */
//             break;

//         case STRING_AB_PLC5:
//             string_capacity = 82;
//             break;

//         case STRING_AB_LGX:
//             string_capacity = 82;
//             break;

//         case STRING_AB_TAG_NAME:
//             string_capacity = get_string_length_unsafe(tag, offset);
//             break;

//         case STRING_ZERO_TERM:
//             string_capacity = get_string_length_unsafe(tag, offset);
//             break;

//         default:
//             pdebug(DEBUG_WARN, "Unsupported string type!");
//             string_capacity = PLCTAG_ERR_UNSUPPORTED;
//             break;
//     }

//     return string_capacity;
// }


/*
 * get the string padding depending on the PLC string type.
 *
 * This is called in other functions so is separated out.
 *
 * This must be called with the tag API mutex held!
 */

// int get_string_padding_unsafe(plc_tag_p tag, int offset)
// {
//     int string_padding = 0;
//     string_type_t string_type = tag->byte_order.string_type;

//     /* FIXME - do something with the offset! */
//     (void)offset;

//     switch(string_type) {
//         case STRING_AB_MICRO800:
//             string_padding = 0; /* FIXME - is this correct? */
//             break;

//         case STRING_AB_PLC5:
//             string_padding = 0;
//             break;

//         case STRING_AB_LGX:
//             string_padding = 2;
//             break;

//         case STRING_AB_TAG_NAME:
//             string_padding = 0;
//             break;

//         case STRING_ZERO_TERM:
//             string_padding = 1; /* for the zero char termination. */
//             break;

//         default:
//             pdebug(DEBUG_WARN, "Unsupported string type!");
//             string_padding = PLCTAG_ERR_UNSUPPORTED;
//             break;
//     }

//     return string_padding;
// }


/*
 * get the string total length depending on the PLC string type.
 *
 * This is called in other functions so is separated out.
 *
 * This must be called with the tag API mutex held!
 */


// int get_string_total_length_unsafe(plc_tag_p tag, int offset)
// {
//     int string_count_word_size = get_string_count_size_unsafe(tag, offset);
//     int string_capacity = get_string_capacity_unsafe(tag, offset);
//     int string_padding = get_string_padding_unsafe(tag, offset);

//     if(string_count_word_size < 0) {
//         return string_count_word_size;
//     }

//     if(string_capacity < 0) {
//         return string_capacity;
//     }

//     if(string_padding < 0) {
//         return string_padding;
//     }

//     return string_count_word_size + string_capacity + string_padding;
// }


// int get_string_byte_swapped_index_unsafe(plc_tag_p tag, int offset, int char_index)
// {
//     int new_index = 0;
//     string_type_t string_type = tag->byte_order.string_type;

//     /* FIXME - do something with the offset! */
//     (void)offset;

//     switch(string_type) {
//         case STRING_AB_MICRO800:
//             new_index = char_index;

//             break;

//         case STRING_AB_PLC5:
//             /* thank you, AB */
//             /* byte swap the index, odd -> even and even -> odd */
//             if(char_index & 0x01) {
//                 /* odd */
//                 new_index = char_index - 1;
//             } else {
//                 /* even */
//                 new_index = char_index + 1;
//             }

//             break;

//         case STRING_AB_LGX:
//             new_index = char_index;
//             break;

//         case STRING_AB_TAG_NAME:
//             new_index = char_index;
//             break;

//         case STRING_ZERO_TERM:
//             new_index = char_index;
//             break;

//         default:
//             pdebug(DEBUG_WARN, "Unsupported string type!");
//             new_index = PLCTAG_ERR_UNSUPPORTED;

//             break;
//     }

//     return new_index;
// }


/*
 * base_tag_create
 *
 * This function allocates the tag with rc_alloc and then sets up the
 * base tag parts.
 */

plc_tag_p base_tag_create(int tag_size, void (*tag_destructor)(void *arg))
{
    plc_tag_p tag = NULL;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    tag = rc_alloc(tag_size, tag_destructor);
    if(!tag) {
        pdebug(DEBUG_WARN, "Unable to allocate tag memory!");
        return NULL;
    }

    rc = mutex_create(&(tag->ext_mutex));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"Unable to create tag external mutex!");
        rc_dec(tag);
        return NULL;
    }

    rc = mutex_create(&(tag->api_mutex));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"Unable to create tag API mutex!");
        rc_dec(tag);
        return NULL;
    }

    pdebug(DEBUG_INFO, "Done.");

    return tag;
}


void base_tag_destroy(void *tag_arg)
{
    plc_tag_p tag = (plc_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Destructor called with null pointer!");
        return;
    }

    if(tag->ext_mutex) {
        mutex_destroy(&(tag->ext_mutex));
        tag->ext_mutex = NULL;
    }

    if(tag->api_mutex) {
        mutex_destroy(&(tag->api_mutex));
        tag->api_mutex = NULL;
    }

    if(tag->byte_order && tag->byte_order->is_allocated) {
        mem_free(tag->byte_order);
        tag->byte_order = NULL;
    }

    if(tag->data) {
        mem_free(tag->data);
        tag->data = NULL;
    }

    pdebug(DEBUG_INFO, "Done.");
}