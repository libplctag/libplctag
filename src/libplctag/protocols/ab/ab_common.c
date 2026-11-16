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

#include <ctype.h>
#include <float.h>
#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/protocols/ab/ab.h>
#include <libplctag/protocols/ab/ab_common.h>
#include <libplctag/protocols/ab/cip.h>
#include <libplctag/protocols/ab/defs.h>
#include <libplctag/protocols/ab/connection_tag.h>
#include <libplctag/protocols/ab/eip_cip.h>
#include <libplctag/protocols/ab/eip_cip_special.h>
#include <libplctag/protocols/ab/eip_lgx_pccc.h>
#include <libplctag/protocols/ab/eip_plc5_pccc.h>
#include <libplctag/protocols/ab/eip_slc_pccc.h>
#include <libplctag/protocols/ab/omron_listing.h>
#include <libplctag/protocols/ab/pccc.h>
#include <libplctag/protocols/ab/session.h>
#include <libplctag/protocols/ab/tag.h>
#include <limits.h>
#include <platform.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/vector.h>

/* Minimal view of AB device-tag layout needed for source-session sharing. */
typedef struct ab_connection_tag_view_s {
    TAG_BASE_STRUCT;
    cip_conn_p session;
} ab_connection_tag_view_t;

/*
 * Externally visible global variables
 */

// volatile cip_conn_p sessions = NULL;
// volatile mutex_p global_session_mut = NULL;
//
// volatile vector_p read_group_tags = NULL;


/* request/response handling thread */
volatile thread_p io_handler_thread = NULL;

volatile int ab_protocol_terminating = 0;


/*
 * Generic Rockwell/Allen-Bradley protocol functions.
 *
 * These are the primary entry points into the AB protocol
 * stack.
 */


#define DEFAULT_NUM_RETRIES (5)
#define DEFAULT_RETRY_INTERVAL (300)


/* forward declarations*/
static int cip_tag_data_type(ab_tag_p tag, attr attribs);
static int pccc_plc_tag_create(ab_tag_p tag, attr attribs, bool from_src);
static int cip_plc_tag_create(ab_tag_p tag, attr attribs, bool from_src);
static int generic_plc_tag_create(ab_tag_p tag, attr attribs, bool from_src);
static int finish_tag_create(ab_tag_p tag, attr attribs, bool sized);
static int finish_identified_tag(ab_tag_p tag, attr attribs, plc_type_t plc_type);
static int deferred_abort(plc_tag_p tag);
static int deferred_busy(plc_tag_p tag);
static int deferred_status(plc_tag_p tag);
static int deferred_tickler(plc_tag_p tag);

static void ab_tag_destroy(ab_tag_p tag);
static int default_abort(plc_tag_p tag);
static int default_read(plc_tag_p tag);
static int default_status(plc_tag_p tag);
static int default_tickler(plc_tag_p tag);
static int default_write(plc_tag_p tag);


/* a tag waiting for identify to name the PLC type; the tickler finishes it. */
static struct tag_vtable_t deferred_vtable = {
    .abort = deferred_abort,
    .read = deferred_busy,
    .status = deferred_status,
    .tickler = deferred_tickler,
    .write = deferred_busy,
    .wake_plc = NULL,
    .tag_data_written = NULL,
    .attribs = ab_attribs,
};

/* vtables for different kinds of tags */
struct tag_vtable_t default_vtable = {
    .abort = default_abort,
    .read = default_read,
    .status = default_status,
    .tickler = default_tickler,
    .write = default_write,
    .wake_plc = NULL,
    .tag_data_written = NULL,

    /* attribute accessors */
    .attribs = ab_attribs,
};


/*
 * Public functions.
 */


int ab_init(void) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, 0, "Initializing AB protocol library.");

    ab_protocol_terminating = 0;

    if((rc = session_startup()) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_ERROR, 0, "Unable to initialize session library!");
        return rc;
    }

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, 0, "Finished initializing AB protocol library.");

    return rc;
}

/*
 * called when the whole program is going to terminate.
 */
void ab_teardown(void) {
    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, 0, "Releasing global AB protocol resources.");

    if(io_handler_thread) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, 0, "Terminating IO thread.");
        /* signal the IO thread to quit first. */
        ab_protocol_terminating = 1;

        /* wait for the thread to die */
        thread_join(io_handler_thread);
        thread_destroy((thread_p *)&io_handler_thread);
    } else {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, 0, "IO thread already stopped.");
    }

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, 0, "Freeing session information.");

    session_teardown();

    ab_protocol_terminating = 0;

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, 0, "Done.");
}


static const char *const plc5_aliases[] = {"plc5", "plc", NULL};
static const char *const slc_aliases[] = {"slc500", "slc", NULL};
static const char *const mlgx_aliases[] = {"micrologix", "mlgx", NULL};
static const char *const lgx_pccc_aliases[] = {"logix-pccc", "lgxpccc", "logixpccc", "lgxplc5", "logixplc5", "lgx-pccc",
                                               "lgx-plc5", "logix-plc5", NULL};
static const char *const micro800_aliases[] = {"micro800", "micrologix800", "mlgx800", NULL};
static const char *const lgx_aliases[] = {"ControlLogix", "compactlogix", "clgx", "lgx", "contrologix", "logix", NULL};
static const char *const omron_aliases[] = {"omron-njnx", "omron-nj", "omron-nx", "njnx", "nx1p2", NULL};
static const char *const generic_aliases[] = {"generic", "cip", NULL};

/* The first alias is how the plc attribute reads back. */
static const cip_plc_type_t plc_types[] = {
    {.id = AB_PLC_PLC5, .name = "PLC/5", .aliases = plc5_aliases, .max_msg_size = MAX_CIP_PCCC_MSG_SIZE,
     .fo_conn_size = MAX_CIP_PCCC_MSG_SIZE, .setup_conn = session_setup_pccc, .tag_create = pccc_plc_tag_create},
    {.id = AB_PLC_SLC, .name = "SLC 500", .aliases = slc_aliases, .max_msg_size = MAX_CIP_PCCC_MSG_SIZE,
     .fo_conn_size = MAX_CIP_PCCC_MSG_SIZE, .setup_conn = session_setup_pccc, .tag_create = pccc_plc_tag_create},
    {.id = AB_PLC_MLGX, .name = "MicroLogix", .aliases = mlgx_aliases, .max_msg_size = MAX_CIP_PCCC_MSG_SIZE,
     .fo_conn_size = MAX_CIP_PCCC_MSG_SIZE, .setup_conn = session_setup_pccc, .tag_create = pccc_plc_tag_create},
    {.id = AB_PLC_LGX_PCCC, .name = "Logix PCCC", .aliases = lgx_pccc_aliases, .max_msg_size = MAX_CIP_PCCC_MSG_SIZE,
     .fo_conn_size = MAX_CIP_PCCC_MSG_SIZE, .setup_conn = session_setup_pccc, .tag_create = pccc_plc_tag_create},
    {.id = AB_PLC_MICRO800, .name = "Micro800", .aliases = micro800_aliases, .max_msg_size = MAX_CIP_MICRO800_MSG_SIZE_EX,
     .fo_conn_size = MAX_CIP_MICRO800_MSG_SIZE, .connected = 1, .fragmented = true, .setup_conn = session_setup_generic, .tag_create = cip_plc_tag_create},
    {.id = AB_PLC_LGX, .name = "Logix", .aliases = lgx_aliases, .max_msg_size = MAX_CIP_LGX_MSG_SIZE_EX,
     .fo_conn_size = MAX_CIP_LGX_MSG_SIZE, .fo_ex_conn_size = MAX_CIP_LGX_MSG_SIZE_EX, .connected = -1,
     .allow_packing = true, .needs_path = true, .fragmented = true, .setup_conn = session_setup_logix,
     .tag_create = cip_plc_tag_create},
    /* Omron packs only requests whose reply size is known; see ab_request_t's reply_size. */
    {.id = AB_PLC_OMRON_NJNX, .name = "Omron NJ/NX", .aliases = omron_aliases, .max_msg_size = MAX_CIP_OMRON_MSG_SIZE_EX,
     .fo_conn_size = MAX_CIP_OMRON_MSG_SIZE, .fo_ex_conn_size = MAX_CIP_OMRON_MSG_SIZE_EX, .connected = -1, .allow_packing = true,
     .needs_path = true, .setup_conn = session_setup_generic, .tag_create = cip_plc_tag_create},
    {.id = AB_PLC_GENERIC, .name = "generic CIP", .aliases = generic_aliases, .max_msg_size = MAX_CIP_LGX_MSG_SIZE_EX,
     .fo_conn_size = MAX_CIP_LGX_MSG_SIZE, .fo_ex_conn_size = MAX_CIP_LGX_MSG_SIZE_EX, .fragmented = true,
     .setup_conn = session_setup_generic, .tag_create = generic_plc_tag_create},
};


/* The PLC type the tag string names with plc= or cpu=, or NULL. */
const cip_plc_type_t *ab_plc_type_find(attr attribs) {
    const char *cpu_type = attr_get_str(attribs, "plc", attr_get_str(attribs, "cpu", NULL));

    if(!cpu_type) { return NULL; }

    for(size_t i = 0; i < sizeof(plc_types) / sizeof(plc_types[0]); i++) {
        for(const char *const *alias = plc_types[i].aliases; *alias; alias++) {
            if(str_cmp_i(cpu_type, *alias) == 0) { return &plc_types[i]; }
        }
    }

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Unsupported device type: %s", cpu_type);

    return NULL;
}


const cip_plc_type_t *ab_plc_type_get(plc_type_t id) {
    for(size_t i = 0; i < sizeof(plc_types) / sizeof(plc_types[0]); i++) {
        if(plc_types[i].id == id) { return &plc_types[i]; }
    }

    return NULL;
}


plc_type_t get_plc_type(attr attribs) {
    const cip_plc_type_t *plc = ab_plc_type_find(attribs);

    return plc ? plc->id : AB_PLC_NONE;
}


#define CIP_VENDOR_ROCKWELL ((uint16_t)1)
#define CIP_VENDOR_OMRON ((uint16_t)47)

/*
 * Identity product-name prefixes, first match wins.  AB_PLC_NONE refuses the device: a chassis
 * module or network adapter is not a CPU, and reaching one means the path stops short.  Anything
 * that matches nothing is generic CIP.  Catalog prefixes, not names seen in replies: correct a
 * row when a real reply disagrees.
 */
static const struct {
    uint16_t vendor_id;
    const char *prefix;
    plc_type_t plc_type;
} identity_map[] = {
    {CIP_VENDOR_ROCKWELL, "1756-L", AB_PLC_LGX},
    {CIP_VENDOR_ROCKWELL, "1756-", AB_PLC_NONE},
    {CIP_VENDOR_ROCKWELL, "1768-L", AB_PLC_LGX},
    {CIP_VENDOR_ROCKWELL, "1768-", AB_PLC_NONE},
    {CIP_VENDOR_ROCKWELL, "1769-L", AB_PLC_LGX},
    {CIP_VENDOR_ROCKWELL, "5069-L", AB_PLC_LGX},
    {CIP_VENDOR_ROCKWELL, "2080-", AB_PLC_MICRO800},
    {CIP_VENDOR_ROCKWELL, "PLC-5", AB_PLC_PLC5},
    {CIP_VENDOR_ROCKWELL, "1747-L", AB_PLC_SLC},
    {CIP_VENDOR_ROCKWELL, "1761-NET-ENI", AB_PLC_NONE},
    {CIP_VENDOR_ROCKWELL, "1761-", AB_PLC_MLGX},
    {CIP_VENDOR_ROCKWELL, "1762-", AB_PLC_MLGX},
    {CIP_VENDOR_ROCKWELL, "1763-", AB_PLC_MLGX},
    {CIP_VENDOR_ROCKWELL, "1764-", AB_PLC_MLGX},
    {CIP_VENDOR_ROCKWELL, "1766-", AB_PLC_MLGX},
    {CIP_VENDOR_OMRON, "NX-", AB_PLC_NONE},
    {CIP_VENDOR_OMRON, "NX", AB_PLC_OMRON_NJNX},
    {CIP_VENDOR_OMRON, "NJ", AB_PLC_OMRON_NJNX},
};


int ab_plc_type_from_identity(uint16_t vendor_id, const char *product_name, bool has_path, plc_type_t *plc_type) {
    plc_type_t found = AB_PLC_GENERIC;

    for(size_t i = 0; i < sizeof(identity_map) / sizeof(identity_map[0]); i++) {
        const char *prefix = identity_map[i].prefix;

        if(identity_map[i].vendor_id == vendor_id && str_cmp_i_n(product_name, prefix, str_length(prefix)) == 0) {
            found = identity_map[i].plc_type;
            break;
        }
    }

    if(found == AB_PLC_NONE) {
        if(has_path) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "The path reaches a %s, not a CPU!", product_name);
        } else {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "The gateway is a %s.  A path to the CPU is required!", product_name);
        }

        return PLCTAG_ERR_BAD_PARAM;
    }

    *plc_type = found;

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, 0, "Vendor %u \"%s\" is a %s.", (unsigned int)vendor_id, product_name,
           ab_plc_type_get(found)->name);

    return PLCTAG_STATUS_OK;
}


plc_tag_p ab_tag_create(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                        void *userdata, plc_tag_p src_tag) {
    ab_tag_p tag = AB_TAG_NULL;
    const cip_plc_type_t *plc = NULL;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, 0, "Starting.");

    /* short circuit for device tag */
    if(str_cmp(attr_get_str(attribs, "name", ""), "@connection") == 0) {
        return (plc_tag_p)ab_connection_tag_create(attribs, tag_callback_func, userdata, src_tag);
    }

    /*
     * allocate memory for the new tag.  Do this first so that
     * we have a vehicle for returning status.
     */

    tag = (ab_tag_p)rc_alloc(sizeof(struct ab_tag_t), (rc_cleanup_func)ab_tag_destroy);
    if(!tag) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_ERROR, 0, "Unable to allocate memory for AB EIP tag!");
        return (plc_tag_p)NULL;
    }

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "tag=%p", tag);

    /*
     * we got far enough to allocate memory, set the default vtable up
     * in case we need to abort later.
     */

    tag->vtable = &default_vtable;
    tag->protocol_type = TAG_PROTOCOL_AB;

    /* set up the generic parts. */
    rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Unable to initialize generic tag parts!");
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        return (plc_tag_p)NULL;
    }

    /*
     * check the CPU type.
     *
     * This determines the protocol type.
     *
     */

    if(src_tag) {
        switch(src_tag->protocol_type) {
            case TAG_PROTOCOL_AB: {
                ab_tag_p src = (ab_tag_p)src_tag;
                tag->plc_type = src->plc_type;
                break;
            }

            case TAG_PROTOCOL_AB_CONNECTION: {
                ab_connection_tag_view_t *src_device = (ab_connection_tag_view_t *)src_tag;
                tag->plc_type = src_device->session ? src_device->session->plc_type : AB_PLC_NONE;
                break;
            }

            default: tag->plc_type = AB_PLC_NONE; break;
        }
    } else {
        tag->plc_type = get_plc_type(attribs);

        /* an unknown plc= is an error; no plc= at all means identify picks it. */
        if(tag->plc_type == AB_PLC_NONE && attr_get_str(attribs, "plc", attr_get_str(attribs, "cpu", NULL))) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "CPU type not valid or missing.");
            /* tag->status = PLCTAG_ERR_BAD_DEVICE; */
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
            rc_dec(tag);
            return (plc_tag_p)NULL;
        }
    }

    plc = ab_plc_type_get(tag->plc_type);

    if(plc && !plc->tag_create) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Unsupported PLC type!");
        tag->status = PLCTAG_ERR_BAD_CONFIG;
        return (plc_tag_p)tag;
    }

    if(plc) {
        tag->use_connected_msg = (plc->connected < 0) ? attr_get_int(attribs, "use_connected_msg", 1) : plc->connected;
        tag->allow_packing = plc->allow_packing ? attr_get_int(attribs, "allow_packing", 1) : 0;
    } else if(str_str_cmp_i(attr_get_str(attribs, "path", ""), ":")) {
        /* Identity cannot see past a DH+ bridge, so a DH+ path needs plc=. */
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "A DH+ path needs the plc attribute!");
        tag->status = PLCTAG_ERR_BAD_PARAM;
        return (plc_tag_p)tag;
    }

    /* make sure that the connection requirement is forced.  Without a PLC type it is not known yet. */
    if(plc) { attr_set_int(attribs, "use_connected_msg", tag->use_connected_msg); }


    /*
     * Find or create a session.
     *
     * All tags need sessions.  They are the TCP connection to the gateway PLC.
     */
    if(src_tag) {
        switch(src_tag->protocol_type) {
            case TAG_PROTOCOL_AB: {
                ab_tag_p src = (ab_tag_p)src_tag;
                tag->session = rc_inc(src->session);
                break;
            }

            case TAG_PROTOCOL_AB_CONNECTION: {
                ab_connection_tag_view_t *src_device = (ab_connection_tag_view_t *)src_tag;
                tag->session = rc_inc(src_device->session);
                break;
            }

            default: tag->session = NULL; break;
        }

        if(!tag->session) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Unable to acquire source session reference.");
            tag->status = PLCTAG_ERR_NOT_FOUND;
            return (plc_tag_p)tag;
        }
    } else {
        if(session_find_or_create(&tag->session, attribs, NULL) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, 0, "Unable to create session!");
            tag->status = PLCTAG_ERR_BAD_GATEWAY;
            return (plc_tag_p)tag;
        }
    }

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "using session=%p", tag->session);

    if(!plc) {
        int32_t refused_rc = PLCTAG_STATUS_OK;
        plc_type_t plc_type = session_plc_type(tag->session, &refused_rc);

        if(plc_type != AB_PLC_NONE) {
            rc = finish_identified_tag(tag, attribs, plc_type);
        } else if(refused_rc != PLCTAG_STATUS_OK) {
            rc = refused_rc;
        } else {
            /* wait for identify.  A byte order for overrides to start from until the PLC type sets one. */
            tag->deferred_attribs = attr_copy(attribs);
            tag->vtable = &deferred_vtable;
            tag->byte_order = &logix_tag_byte_order;
            tag->status = PLCTAG_STATUS_PENDING;
            rc = tag->deferred_attribs ? PLCTAG_STATUS_OK : PLCTAG_ERR_NO_MEM;
        }

        if(rc != PLCTAG_STATUS_OK) { tag->status = (int8_t)rc; }

        return (plc_tag_p)tag;
    }

    /* the PLC type finishes the tag. */
    rc = plc->tag_create(tag, attribs, src_tag != NULL);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Error %s creating tag!", plc_tag_decode_error(rc));
        tag->status = (int8_t)rc;
    }

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, 0, "Done.");

    return (plc_tag_p)tag;
}


/* PLC/5, SLC, MicroLogix and PCCC-mapped Logix: data files, sized by the address. */
int pccc_plc_tag_create(ab_tag_p tag, attr attribs, bool from_src) {
    bool is_dhp = (tag->session->is_dhp != 0);
    const char *name = attr_get_str(attribs, "name", "");
    pccc_addr_t file_addr = {0};
    int rc = PLCTAG_STATUS_OK;

    (void)from_src;

    /* @identity: a CIP request, unconnected, sized by its reply. */
    if(str_cmp_i(name, "@identity") == 0) {
        /* a DH+ node has no Identity object; the bridge would answer in its place. */
        if(is_dhp) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "@identity is not supported through a DH+ bridge!");
            return PLCTAG_ERR_UNSUPPORTED;
        }

        if((rc = setup_identity_tag(tag)) != PLCTAG_STATUS_OK) { return rc; }

        tag->use_connected_msg = 0;
        tag->allow_packing = 0;
        return finish_tag_create(tag, attribs, false);
    }

    if((rc = parse_pccc_logical_address(name, &file_addr)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Unable to parse data file %s!", name);
        return rc;
    }

    tag->elem_size = file_addr.element_size_bytes;
    tag->file_type = file_addr.file_type;

    if(!is_dhp && str_length(attr_get_str(attribs, "path", NULL)) && tag->plc_type != AB_PLC_LGX_PCCC) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "The path routes the PCCC request through a CIP bridge.");
    }

    switch(tag->plc_type) {
        case AB_PLC_PLC5:
            tag->vtable = &plc5_vtable;
            if(!tag->byte_order || !tag->byte_order->is_allocated) { tag->byte_order = &plc5_tag_byte_order; }
            break;

        case AB_PLC_LGX_PCCC:
            is_dhp = false;
            tag->vtable = &lgx_pccc_vtable;
            if(!tag->byte_order || !tag->byte_order->is_allocated) { tag->byte_order = &slc_tag_byte_order; }
            break;

        default: /* SLC and MicroLogix */
            tag->vtable = &slc_vtable;
            if(!tag->byte_order || !tag->byte_order->is_allocated) { tag->byte_order = &slc_tag_byte_order; }
            break;
    }

    /* DH+ goes over a connection to the bridge. */
    tag->use_connected_msg = is_dhp ? 1 : 0;
    tag->allow_packing = 0;
    tag->first_read = 1; /* the create read finds a missing data file. */

    return finish_tag_create(tag, attribs, true);
}


/* Logix, Micro800 and Omron: symbolic tags, sized by the first read. */
int cip_plc_tag_create(ab_tag_p tag, attr attribs, bool from_src) {
    const cip_plc_type_t *plc = ab_plc_type_get(tag->plc_type);
    int rc = (str_cmp_i(attr_get_str(attribs, "name", ""), "@identity") == 0) ? setup_identity_tag(tag)
                                                                             : cip_tag_data_type(tag, attribs);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Error %s getting tag element data type or handling special tag!",
               plc_tag_decode_error(rc));
        return rc;
    }

    if(plc->needs_path && !from_src && !attr_get_str(attribs, "path", NULL)) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "A path is required for this PLC type!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(!tag->byte_order) {
        tag->byte_order = (tag->plc_type == AB_PLC_OMRON_NJNX) ? &omron_tag_byte_order : &logix_tag_byte_order;
    }

    /* a special tag set its own vtable. */
    if(tag->vtable == &default_vtable || !tag->vtable) { tag->vtable = &eip_cip_vtable; }

    tag->first_read = 1; /* the first read gets the type. */

    return finish_tag_create(tag, attribs, false);
}


/* generic CIP: @identity only. */
int generic_plc_tag_create(ab_tag_p tag, attr attribs, bool from_src) {
    const char *name = attr_get_str(attribs, "name", "");
    int rc = PLCTAG_STATUS_OK;

    (void)from_src;

    if(str_cmp_i(name, "@identity") != 0) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Generic PLC type only supports @identity tag, got: %s", name);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    if((rc = setup_identity_tag(tag)) != PLCTAG_STATUS_OK) { return rc; }

    if(!tag->byte_order) { tag->byte_order = &logix_tag_byte_order; }

    return finish_tag_create(tag, attribs, false);
}


/*
 * What every PLC type does last: the element count and, for a sized tag, its buffer; the name
 * check; and the create read, or CREATED when there is none.
 */
int finish_tag_create(ab_tag_p tag, attr attribs, bool sized) {
    /* pass the connection requirement on, since the PLC type may have set it. */
    attr_set_int(attribs, "use_connected_msg", tag->use_connected_msg);

    tag->elem_count = attr_get_int(attribs, "elem_count", 1);

    /*
     * An array can legitimately have up to INT32_MAX elements, but it cannot have zero or a
     * negative number of them.  Several places divide the tag size by this to recover the
     * element size, so a zero here is a division by zero later.
     */
    if(tag->elem_count < 1) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Element count must be at least one, was %d!", tag->elem_count);
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(sized) {
        if(!tag->elem_size) { tag->elem_size = attr_get_int(attribs, "elem_size", 0); }

        if(tag->elem_size < 1) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Element size (%d) must be greater than zero!", tag->elem_size);
            return PLCTAG_ERR_BAD_PARAM;
        }

        /*
         * Both of these come from the attribute string, so their product can overflow.  Signed
         * overflow is undefined, so test by division before multiplying.
         */
        if(tag->elem_count > AB_MAX_TAG_DATA_SIZE / tag->elem_size) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Tag of %d elements of %d bytes each is larger than the maximum tag size of %d bytes!",
                   tag->elem_count, tag->elem_size, AB_MAX_TAG_DATA_SIZE);
            return PLCTAG_ERR_TOO_LARGE;
        }

        tag->size = (tag->elem_count) * (tag->elem_size);

        tag->data = (uint8_t *)mem_alloc(tag->size);
        if(tag->data == NULL) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Unable to allocate tag data!");
            return PLCTAG_ERR_NO_MEM;
        }
    } else {
        /* the CIP read and write services carry the element count in two bytes. */
        if(tag->elem_count > UINT16_MAX) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Element count must be no more than %d, was %d!", UINT16_MAX,
                   tag->elem_count);
            return PLCTAG_ERR_TOO_LARGE;
        }

        /* filled in by the first read. */
        tag->size = 0;
        tag->data = NULL;
    }

    if(!tag->special_tag && check_tag_name(tag, attr_get_str(attribs, "name", NULL)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, 0, "Bad tag name!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* @udt reads its definition at create, so a UDT the PLC does not have fails the create. */
    if((!tag->special_tag || tag->elem_type == AB_TYPE_TAG_UDT) && tag->vtable->read && tag->first_read) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Kicking off initial read.");
        tag->read_in_flight = 1;
        tag->vtable->read((plc_tag_p)tag);
    } else {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Tag does not need a first read, raising created event.");
        tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, tag->status);
    }

    return PLCTAG_STATUS_OK;
}


/*
 * A symbolic tag's element type and size from elem_type or elem_size, or a special tag.
 */
int cip_tag_data_type(ab_tag_p tag, attr attribs) {
    const char *elem_type = NULL;

    /* look for the elem_type attribute. */
    elem_type = attr_get_str(attribs, "elem_type", NULL);
    if(elem_type) {
        if(str_cmp_i(elem_type, "lint") == 0 || str_cmp_i(elem_type, "ulint") == 0) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Found tag element type of 64-bit integer.");
            tag->elem_size = 8;
            tag->elem_type = AB_TYPE_INT64;
        } else if(str_cmp_i(elem_type, "dint") == 0 || str_cmp_i(elem_type, "udint") == 0) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Found tag element type of 32-bit integer.");
            tag->elem_size = 4;
            tag->elem_type = AB_TYPE_INT32;
        } else if(str_cmp_i(elem_type, "int") == 0 || str_cmp_i(elem_type, "uint") == 0) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Found tag element type of 16-bit integer.");
            tag->elem_size = 2;
            tag->elem_type = AB_TYPE_INT16;
        } else if(str_cmp_i(elem_type, "sint") == 0 || str_cmp_i(elem_type, "usint") == 0) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Found tag element type of 8-bit integer.");
            tag->elem_size = 1;
            tag->elem_type = AB_TYPE_INT8;
        } else if(str_cmp_i(elem_type, "bool") == 0) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Found tag element type of bit.");
            tag->elem_size = 1;
            tag->elem_type = AB_TYPE_BOOL;
        } else if(str_cmp_i(elem_type, "bool array") == 0) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Found tag element type of bool array.");
            tag->elem_size = 4;
            tag->elem_type = AB_TYPE_BOOL_ARRAY;
        } else if(str_cmp_i(elem_type, "real") == 0) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Found tag element type of 32-bit float.");
            tag->elem_size = 4;
            tag->elem_type = AB_TYPE_FLOAT32;
        } else if(str_cmp_i(elem_type, "lreal") == 0) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Found tag element type of 64-bit float.");
            tag->elem_size = 8;
            tag->elem_type = AB_TYPE_FLOAT64;
        } else if(str_cmp_i(elem_type, "string") == 0) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Found tag element type of string.");
            tag->elem_size = 88;
            tag->elem_type = AB_TYPE_STRING;
        } else if(str_cmp_i(elem_type, "short string") == 0) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Found tag element type of short string.");
            tag->elem_size = 256; /* TODO - find the real length */
            tag->elem_type = AB_TYPE_SHORT_STRING;
        } else {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Unknown tag type %s", elem_type);
            return PLCTAG_ERR_UNSUPPORTED;
        }
    } else {
        /*
         * We have two cases
         *      * tag listing, but only for CIP PLCs (but not for UDTs!).
         *      * no type, just elem_size.
         * Otherwise this is an error.
         */
        int elem_size = attr_get_int(attribs, "elem_size", 0);
        const char *tmp_tag_name = attr_get_str(attribs, "name", NULL);
        int special_tag_rc = PLCTAG_STATUS_OK;

        /* check for special tags. */
        if(str_cmp_i(tmp_tag_name, "@raw") == 0) {
            special_tag_rc = setup_raw_tag(tag);
        } else if(tag->plc_type == AB_PLC_OMRON_NJNX && str_cmp_i(tmp_tag_name, "@tags") == 0) {
            special_tag_rc = omron_setup_listing_tag(tag);
        } else if(tag->plc_type == AB_PLC_OMRON_NJNX && str_str_cmp_i(tmp_tag_name, "@udt/")) {
            special_tag_rc = omron_setup_udt_tag(tag, tmp_tag_name);
        } else if(tag->plc_type == AB_PLC_OMRON_NJNX && str_str_cmp_i(tmp_tag_name, "@tags")) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Omron has no program tag listing, only @tags!");
            special_tag_rc = PLCTAG_ERR_BAD_PARAM;
        } else if(str_str_cmp_i(tmp_tag_name, "@tags")) {
            special_tag_rc = setup_tag_listing_tag(tag, tmp_tag_name);
        } else if(str_str_cmp_i(tmp_tag_name, "@udt/")) {
            special_tag_rc = setup_udt_tag(tag, tmp_tag_name);
        } /* else not a special tag. */

        if(special_tag_rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Error parsing tag listing name!");
            return special_tag_rc;
        }

        /* if we did not set an element size yet, set one. */
        if(tag->elem_size == 0) {
            if(elem_size > 0) {
                pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, 0, "Setting element size to %d.", elem_size);
                tag->elem_size = elem_size;
            }
        } else {
            if(elem_size > 0) {
                pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0,
                       "Tag has elem_size and either is a tag listing or has elem_type, only use one!");
            }
        }
    }

    return PLCTAG_STATUS_OK;
}


/*
 * Finish a tag created without plc= once identify has named the PLC type.
 * ponytail: byte-order overrides on such a tag start from the Logix defaults, not the PLC type's.
 */
int finish_identified_tag(ab_tag_p tag, attr attribs, plc_type_t plc_type) {
    const cip_plc_type_t *plc = ab_plc_type_get(plc_type);

    if(!plc || !plc->tag_create) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "The identified PLC type is not supported here!");
        return PLCTAG_ERR_UNSUPPORTED;
    }

    tag->plc_type = plc_type;
    tag->use_connected_msg = (plc->connected < 0) ? attr_get_int(attribs, "use_connected_msg", 1) : plc->connected;
    tag->allow_packing = plc->allow_packing ? attr_get_int(attribs, "allow_packing", 1) : 0;
    tag->vtable = &default_vtable;
    tag->status = PLCTAG_STATUS_OK;

    return plc->tag_create(tag, attribs, false);
}


int deferred_abort(plc_tag_p tag) {
    (void)tag;

    return PLCTAG_STATUS_OK;
}


/* nothing can be read or written until the PLC type is known. */
int deferred_busy(plc_tag_p tag) {
    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, tag->tag_id, "The tag is waiting for its PLC to be identified.");

    return PLCTAG_ERR_BUSY;
}


int deferred_status(plc_tag_p tag) { return tag->status; }


/* Runs under the tag's API mutex.  Finishes the tag once identify ends. */
int deferred_tickler(plc_tag_p p_tag) {
    ab_tag_p tag = (ab_tag_p)p_tag;
    int32_t refused_rc = PLCTAG_STATUS_OK;
    plc_type_t plc_type = session_plc_type(tag->session, &refused_rc);
    int rc = PLCTAG_STATUS_OK;

    if(plc_type == AB_PLC_NONE && refused_rc == PLCTAG_STATUS_OK) { return PLCTAG_STATUS_PENDING; }

    rc = (plc_type == AB_PLC_NONE) ? refused_rc : finish_identified_tag(tag, tag->deferred_attribs, plc_type);

    attr_destroy(tag->deferred_attribs);
    tag->deferred_attribs = NULL;

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "Unable to finish the tag, %s!", plc_tag_decode_error(rc));
        tag->status = (int8_t)rc;
        tag_raise_event(p_tag, PLCTAG_EVENT_CREATED, (int8_t)rc);
    }

    /* a create waiting on this tag checks its status again. */
    plc_tag_generic_wake_tag(p_tag);

    return rc;
}


int default_abort(plc_tag_p tag) {
    (void)tag;

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "This should be overridden by a PLC-specific function!");

    return PLCTAG_ERR_NOT_IMPLEMENTED;
}


int default_read(plc_tag_p tag) {
    (void)tag;

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "This should be overridden by a PLC-specific function!");

    return PLCTAG_ERR_NOT_IMPLEMENTED;
}

int default_status(plc_tag_p tag) {
    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "This should be overridden by a PLC-specific function!");

    if(tag) {
        return tag->status;
    } else {
        return PLCTAG_ERR_NOT_FOUND;
    }
}


int default_tickler(plc_tag_p tag) {
    (void)tag;

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "This should be overridden by a PLC-specific function!");

    return PLCTAG_STATUS_OK;
}


int default_write(plc_tag_p tag) {
    (void)tag;

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "This should be overridden by a PLC-specific function!");

    return PLCTAG_ERR_NOT_IMPLEMENTED;
}

/*
 * ab_tag_abort_request_only
 *
 * clean up the tag state for the request but not the offset.
 */

int ab_tag_abort_request_only(ab_tag_p tag) {
    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, tag->tag_id, "Starting.");

    if(tag) {
        ab_request_p req = NULL;

        if(tag->req) {
            critical_block(&tag->api_mutex) { req = rc_inc(tag->req); }
        }

        if(req) {
            spin_block(&req->lock) { atomic_set_int32(&req->abort_request, 1); }

            critical_block(&tag->api_mutex) {
                if(tag->req == req) {
                    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, tag->tag_id,
                           "rc_dec: Releasing tag-owned reference to request of tag %" PRId32 ".", tag->tag_id);
                    tag->req = NULL;
                    rc_dec(req);
                } else {
                    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id,
                           "Request changed out from underneath us during abort process!");
                }
            }

            req = rc_dec(req);
        } else {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, tag->tag_id, "Called without a request in flight.");
        }

        tag->read_in_progress = 0;
        tag->write_in_progress = 0;
    } else {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Called with a null tag pointer.");
    }

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}

/*
 * ab_tag_abort_request
 *
 * This does the work of stopping any inflight requests.
 * This is not thread-safe.  It must be called from a function
 * that locks the tag's mutex or only from a single thread.
 */

int ab_tag_abort_request(ab_tag_p tag) {
    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, tag ? tag->tag_id : 0, "Starting.");

    if(tag) {
        tag->offset = 0;

        ab_tag_abort_request_only(tag);
    } else {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Called with a null tag pointer.");
    }

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, tag ? tag->tag_id : 0, "Done.");

    return PLCTAG_STATUS_OK;
}


int ab_tag_abort(ab_tag_p tag) {
    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, tag ? tag->tag_id : 0, "Starting.");

    if(tag) {
        ab_request_p req = NULL;

        critical_block(&tag->api_mutex) { req = rc_inc(tag->req); }

        if(req) {
            spin_block(&req->lock) { atomic_set_int32(&req->abort_request, 1); }

            /* do a real abort */
            ab_tag_abort_request(tag);

            req = rc_dec(req);

            tag->status = PLCTAG_ERR_ABORT;
            return tag->status;
        } else {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, tag ? tag->tag_id : 0, "Called with a null tag pointer.");
        }
    }

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, tag ? tag->tag_id : 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * ab_tag_status
 *
 * Generic status checker.   May be overridden by individual PLC types.
 */
int ab_tag_status(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    if(tag->read_in_progress) { return PLCTAG_STATUS_PENDING; }

    if(tag->write_in_progress) { return PLCTAG_STATUS_PENDING; }

    if(tag->session) {
        rc = tag->status;
    } else {
        /* this is not OK.  This is fatal! */
        rc = PLCTAG_ERR_CREATE;
    }

    return rc;
}


/*
 * ab_tag_destroy
 *
 * This blocks on the global library mutex.  This should
 * be fixed to allow for more parallelism.  For now, safety is
 * the primary concern.
 */

void ab_tag_destroy(ab_tag_p tag) {
    cip_conn_p session = NULL;

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, tag ? tag->tag_id : 0, "Starting.");

    /* already destroyed? */
    if(!tag) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Tag pointer is null!");

        return;
    }

    /* abort anything in flight */
    ab_tag_abort(tag);

    session = tag->session;

    /* tags should always have a session.  Release it. */
    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, tag->tag_id, "Getting ready to release tag session %p",
           tag ? tag->session : NULL);
    if(session) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, tag->tag_id, "Removing tag from session.");
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, tag->tag_id, "rc_dec: Releasing session reference of tag %" PRId32 ".",
               tag->tag_id);
        rc_dec(session);
        tag->session = NULL;
    } else {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "No session pointer!");
    }

    if(tag->ext_mutex.initialized) {
        mutex_destroy(&(tag->ext_mutex));
    }

    if(tag->api_mutex.initialized) {
        mutex_destroy(&(tag->api_mutex));
    }

    if(tag->tag_cond_wait) {
        cond_destroy(&(tag->tag_cond_wait));
        tag->tag_cond_wait = NULL;
    }

    if(tag->byte_order && tag->byte_order->is_allocated) {
        mem_free(tag->byte_order);
        tag->byte_order = NULL;
    }

    if(tag->deferred_attribs) {
        attr_destroy(tag->deferred_attribs);
        tag->deferred_attribs = NULL;
    }

    if(tag->data) {
        mem_free(tag->data);
        tag->data = NULL;
    }

    if(tag->instance) {
        rc_dec(tag->instance);
        tag->instance = NULL;
    }

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, tag->tag_id, "Finished releasing all tag resources.");

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, tag->tag_id, "Done");
}


/* Attribute accessors for the table below.  An accessor returns a status, or for a byte
 * array the number of bytes copied, and never touches tag->status -- the core records it. */

static int32_t ab_get_elem_size(plc_tag_p raw_tag, int32_t *result) {
    ab_tag_p tag = (ab_tag_p)raw_tag;

    *result = (int32_t)tag->elem_size;

    return PLCTAG_STATUS_OK;
}


static int32_t ab_get_elem_count(plc_tag_p raw_tag, int32_t *result) {
    ab_tag_p tag = (ab_tag_p)raw_tag;

    *result = (int32_t)tag->elem_count;

    return PLCTAG_STATUS_OK;
}


static int32_t ab_get_connection_status(plc_tag_p raw_tag, int32_t *result) {
    ab_tag_p tag = (ab_tag_p)raw_tag;

    /* no session means the tag is not connected, which is a state and not an error. */
    *result = (tag->session ? atomic_get_int32(&tag->session->connection_status) : (int32_t)PLCTAG_CONN_STATUS_DOWN);

    return PLCTAG_STATUS_OK;
}


static int32_t ab_get_connection_inactivity_timeout_ms(plc_tag_p raw_tag, int32_t *result) {
    ab_tag_p tag = (ab_tag_p)raw_tag;

    /* no session means the session that will be created uses the default. */
    *result = (tag->session ? atomic_get_int32(&tag->session->connection_inactivity_timeout_ms)
                            : (int32_t)SESSION_DISCONNECT_TIMEOUT);

    return PLCTAG_STATUS_OK;
}


static int32_t ab_set_connection_inactivity_timeout_ms(plc_tag_p raw_tag, int32_t value) {
    ab_tag_p tag = (ab_tag_p)raw_tag;
    int32_t clamped_value = value;
    int32_t rc = PLCTAG_STATUS_OK;

    /* Clamp to valid range: 100ms minimum, SESSION_DISCONNECT_TIMEOUT (31000ms) maximum */
    if(clamped_value < 100) {
        clamped_value = 100;
        rc = PLCTAG_ERR_OUT_OF_BOUNDS;
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id,
               "connection_inactivity_timeout_ms value %d clamped to minimum 100ms.", (int)value);
    } else if(clamped_value > SESSION_DISCONNECT_TIMEOUT) {
        clamped_value = SESSION_DISCONNECT_TIMEOUT;
        rc = PLCTAG_ERR_OUT_OF_BOUNDS;
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id,
               "connection_inactivity_timeout_ms value %d clamped to maximum %d ms.", (int)value, SESSION_DISCONNECT_TIMEOUT);
    }

    if(!tag->session) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "Cannot set connection_inactivity_timeout_ms: no session exists.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    atomic_set_int32(&tag->session->connection_inactivity_timeout_ms, clamped_value);

    return rc;
}


static int32_t ab_get_elem_type(plc_tag_p raw_tag, int32_t *result) {
    ab_tag_p tag = (ab_tag_p)raw_tag;

    switch(tag->plc_type) {
        case AB_PLC_PLC5: /* fall through */
        case AB_PLC_MLGX: /* fall through */
        case AB_PLC_SLC:  /* fall through */
        case AB_PLC_LGX_PCCC: *result = (int32_t)(tag->file_type); break;

        case AB_PLC_LGX:      /* fall through */
        case AB_PLC_MICRO800: /* fall through */
        case AB_PLC_OMRON_NJNX: *result = (int32_t)(tag->elem_type); break;

        default:
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "Unsupported PLC type %d!", tag->plc_type);
            return PLCTAG_ERR_UNSUPPORTED;
    }

    return PLCTAG_STATUS_OK;
}


/* Only the PLC types that encode CIP type information carry this. */
static int32_t ab_get_raw_tag_type_bytes_size(plc_tag_p raw_tag) {
    ab_tag_p tag = (ab_tag_p)raw_tag;

    switch(tag->plc_type) {
        case AB_PLC_LGX:      /* fall through */
        case AB_PLC_MICRO800: /* fall through */
        case AB_PLC_OMRON_NJNX: return (int32_t)(tag->encoded_type_info_size);

        default:
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "Unsupported PLC type %d!", tag->plc_type);
            return PLCTAG_ERR_UNSUPPORTED;
    }
}


static int32_t ab_get_raw_tag_type_bytes_length(plc_tag_p raw_tag, int32_t *result) {
    int32_t size = ab_get_raw_tag_type_bytes_size(raw_tag);

    if(size < 0) { return size; }

    *result = size;

    return PLCTAG_STATUS_OK;
}


static int32_t ab_get_raw_tag_type_bytes(plc_tag_p raw_tag, uint8_t *buffer, int32_t buffer_length) {
    ab_tag_p tag = (ab_tag_p)raw_tag;
    int32_t size = ab_get_raw_tag_type_bytes_size(raw_tag);

    if(size < 0) { return size; }

    if(size > buffer_length) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id,
               "Tag type info is larger, %d bytes, than the buffer can hold, %d bytes.", (int)size, (int)buffer_length);
        return PLCTAG_ERR_TOO_SMALL;
    }

    mem_copy((void *)buffer, (void *)&(tag->encoded_type_info[0]), (int)size);

    /* the caller gets the number of bytes copied. */
    return size;
}


/* The PLC type as the tag string spells it, so plc_type_t stays private. */
static const char *ab_plc_type_name(plc_type_t plc_type) {
    const cip_plc_type_t *plc = ab_plc_type_get(plc_type);

    return plc ? plc->aliases[0] : NULL;
}


static int32_t ab_get_plc(plc_tag_p raw_tag, uint8_t *buffer, int32_t buffer_length) {
    ab_tag_p tag = (ab_tag_p)raw_tag;

    return attr_copy_string(ab_plc_type_name(tag->plc_type), buffer, buffer_length);
}


static int32_t ab_get_plc_size(plc_tag_p raw_tag) {
    ab_tag_p tag = (ab_tag_p)raw_tag;

    return attr_string_size(ab_plc_type_name(tag->plc_type));
}


static int32_t ab_get_use_connected_msg(plc_tag_p raw_tag, int32_t *result) {
    ab_tag_p tag = (ab_tag_p)raw_tag;

    *result = (int32_t)tag->use_connected_msg;

    return PLCTAG_STATUS_OK;
}


static int32_t ab_get_allow_packing(plc_tag_p raw_tag, int32_t *result) {
    ab_tag_p tag = (ab_tag_p)raw_tag;

    *result = (int32_t)tag->allow_packing;

    return PLCTAG_STATUS_OK;
}


static int32_t ab_get_gateway(plc_tag_p raw_tag, uint8_t *buffer, int32_t buffer_length) {
    ab_tag_p tag = (ab_tag_p)raw_tag;

    if(!tag->session) { return PLCTAG_ERR_NOT_FOUND; }

    return attr_copy_string(tag->session->host, buffer, buffer_length);
}


static int32_t ab_get_gateway_size(plc_tag_p raw_tag) {
    ab_tag_p tag = (ab_tag_p)raw_tag;

    if(!tag->session) { return PLCTAG_ERR_NOT_FOUND; }

    return attr_string_size(tag->session->host);
}


static int32_t ab_get_gateway_port(plc_tag_p raw_tag, int32_t *result) {
    ab_tag_p tag = (ab_tag_p)raw_tag;

    if(!tag->session) { return PLCTAG_ERR_NOT_FOUND; }

    *result = (int32_t)tag->session->port;

    return PLCTAG_STATUS_OK;
}


/* The encoded CIP path, not the "1,0" text it was built from. */
static int32_t ab_get_path(plc_tag_p raw_tag, uint8_t *buffer, int32_t buffer_length) {
    ab_tag_p tag = (ab_tag_p)raw_tag;

    if(!tag->session) { return PLCTAG_ERR_NOT_FOUND; }

    if((int32_t)tag->session->conn_path_size > buffer_length) { return PLCTAG_ERR_TOO_SMALL; }

    mem_copy((void *)buffer, (void *)tag->session->conn_path, (int)tag->session->conn_path_size);

    return (int32_t)tag->session->conn_path_size;
}


static int32_t ab_get_path_size(plc_tag_p raw_tag) {
    ab_tag_p tag = (ab_tag_p)raw_tag;

    if(!tag->session) { return PLCTAG_ERR_NOT_FOUND; }

    return (int32_t)tag->session->conn_path_size;
}


static int32_t ab_get_conn_only_use_old_forward_open(plc_tag_p raw_tag, int32_t *result) {
    ab_tag_p tag = (ab_tag_p)raw_tag;

    if(!tag->session) { return PLCTAG_ERR_NOT_FOUND; }

    *result = (int32_t)tag->session->only_use_old_forward_open;

    return PLCTAG_STATUS_OK;
}


const attr_def_t ab_attribs[] = {
    {.name = "elem_size",
     .type = ATTR_TYPE_INT,
     .description = "The size in bytes of a single element of this tag.",
     .get_int = ab_get_elem_size},

    {.name = "elem_count",
     .type = ATTR_TYPE_INT,
     .description = "The number of elements this tag holds.",
     .get_int = ab_get_elem_count},

    {.name = "elem_type",
     .type = ATTR_TYPE_INT,
     .description = "The PLC's type code for a single element of this tag.",
     .get_int = ab_get_elem_type},

    {.name = "connection_status",
     .type = ATTR_TYPE_INT,
     .description = "The state of the session or connection this tag uses, as a plc_tag_conn_status_t.",
     .get_int = ab_get_connection_status},

    {.name = "connection_inactivity_timeout_ms",
     .type = ATTR_TYPE_INT,
     .description = "Disconnect the session after this many milliseconds without traffic.",
     .get_int = ab_get_connection_inactivity_timeout_ms,
     .set_int = ab_set_connection_inactivity_timeout_ms},

    {.name = "raw_tag_type_bytes",
     .type = ATTR_TYPE_BYTES,
     .description = "The encoded CIP type information for this tag.",
     .get_bytes = ab_get_raw_tag_type_bytes,
     .get_bytes_size = ab_get_raw_tag_type_bytes_size},

    /* Deprecated: it predates plc_tag_get_attribute_size().  No new byte array gets a length
     * attribute of its own. */
    {.name = "raw_tag_type_bytes.length",
     .type = ATTR_TYPE_INT,
     .description = "Deprecated, use plc_tag_get_attribute_size(). The size of raw_tag_type_bytes in bytes.",
     .get_int = ab_get_raw_tag_type_bytes_length},

    /* Read-only after creation.  Those on the shared session report PLCTAG_ERR_NOT_FOUND
     * until it exists. */
    {.name = "plc",
     .type = ATTR_TYPE_STRING,
     .description = "The PLC family this tag talks to, as a canonical name.",
     .get_bytes = ab_get_plc,
     .get_bytes_size = ab_get_plc_size},

    {.name = "use_connected_msg",
     .type = ATTR_TYPE_INT,
     .description = "This tag uses CIP connected messaging.",
     .get_int = ab_get_use_connected_msg},

    {.name = "allow_packing",
     .type = ATTR_TYPE_INT,
     .description = "This tag's requests may be packed with others into one CIP request.",
     .get_int = ab_get_allow_packing},

    {.name = "gateway",
     .type = ATTR_TYPE_STRING,
     .description = "The host name or address of the gateway this tag's session connects to.",
     .get_bytes = ab_get_gateway,
     .get_bytes_size = ab_get_gateway_size},

    {.name = "gateway_port",
     .type = ATTR_TYPE_INT,
     .description = "The TCP port this tag's session connects to.",
     .get_int = ab_get_gateway_port},

    {.name = "path",
     .type = ATTR_TYPE_BYTES,
     .description = "The encoded CIP path from the gateway to the PLC.",
     .get_bytes = ab_get_path,
     .get_bytes_size = ab_get_path_size},

    {.name = "conn_only_use_old_forward_open",
     .type = ATTR_TYPE_INT,
     .description = "This tag's session uses the original Forward Open only, never the large one.",
     .get_int = ab_get_conn_only_use_old_forward_open},

    {.name = NULL},
};


int check_tag_name(ab_tag_p tag, const char *name) {
    int rc = PLCTAG_STATUS_OK;
    pccc_addr_t pccc_address;

    if(!name) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "No tag name parameter found!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    mem_set(&pccc_address, 0, sizeof(pccc_address));

    /* attempt to parse the tag name */
    switch(tag->plc_type) {
        case AB_PLC_PLC5:
        case AB_PLC_LGX_PCCC:
            if((rc = parse_pccc_logical_address(name, &pccc_address))) {
                pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "Parse of PCCC-style tag name %s failed!", name);
                return rc;
            }

            if(pccc_address.is_bit) {
                tag->is_bit = 1;
                tag->bit = (int)(unsigned int)pccc_address.bit;
                pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, tag->tag_id, "PLC/5 address references bit %d.", tag->bit);
            }

            if((rc = plc5_encode_address(tag->encoded_name, &(tag->encoded_name_size), MAX_TAG_NAME, &pccc_address))
               != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "Encoding of PLC/5-style tag name %s failed!", name);
                return rc;
            }

            break;

        case AB_PLC_SLC:
        case AB_PLC_MLGX:
            if((rc = parse_pccc_logical_address(name, &pccc_address))) {
                pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "Parse of PCCC-style tag name %s failed!", name);
                return rc;
            }

            if(pccc_address.is_bit) {
                tag->is_bit = 1;
                tag->bit = (int)(unsigned int)pccc_address.bit;
                pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, tag->tag_id, "SLC/Micrologix address references bit %d.", tag->bit);
            }

            if((rc = slc_encode_address(tag->encoded_name, &(tag->encoded_name_size), MAX_TAG_NAME, &pccc_address))
               != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "Encoding of SLC-style tag name %s failed!", name);
                return rc;
            }

            break;

        case AB_PLC_MICRO800:
        case AB_PLC_LGX:
        case AB_PLC_OMRON_NJNX:
            if((rc = cip_encode_tag_name(tag, name)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "parse of CIP-style tag name %s failed!", name);

                return rc;
            }

            break;

        case AB_PLC_GENERIC:
            /* Generic PLC type only supports special tags like @identity */
            /* Special tag handling is done elsewhere, just validate the name format */
            if(name[0] != '@') {
                pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id,
                       "Generic PLC type only supports special tags starting with @, got: %s", name);
                return PLCTAG_ERR_UNSUPPORTED;
            }

            /* Placeholder - actual tag handling is done in special tag setup */
            break;

        default:
            /* how would we get here? */
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "unsupported PLC type %d", tag->plc_type);

            return PLCTAG_ERR_BAD_PARAM;

            break;
    }

    return PLCTAG_STATUS_OK;
}


/**
 * @brief Check the status of the request
 *
 * This function checks the request itself and updates the
 * tag if there are any failures or changes that need to be
 * made due to the request status.
 *
 * The tag and the request must not be deleted out from underneath
 * this function.   Both must be held with write mutexes.
 *
 * @return status of the request.
 */

int check_request_status(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    ab_request_p request = NULL;

    /* check early for null pointers */
    if(!tag) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Called with null tag pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    do {
        /* do we have an abort outstanding? */
        if(atomic_get_bool(&tag->abort_requested)) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, tag->tag_id, "Abort requested on tag %" PRId32 ".", tag->tag_id);
            ab_tag_abort_request(tag);
            atomic_set_bool(&tag->abort_requested, false);
            rc = PLCTAG_ERR_ABORT;
            break;
        }

        /* make sure the request cannot be pulled out from underneath us. */
        if(tag->req) {
            critical_block(&tag->api_mutex) { request = rc_inc(tag->req); }
        }

        /* it was already gone. */
        if(!request) {
            if(tag->read_in_progress || tag->write_in_progress) {
                tag->read_in_progress = 0;
                tag->write_in_progress = 0;
                tag->offset = 0;

                pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "A request was in progress, but no request in flight!");
            }

            rc = PLCTAG_STATUS_OK;
            break;
        }

        /* request can be used by more than one thread at once. */
        spin_block(&request->lock) {
            if(!request->resp_received) {
                rc = PLCTAG_STATUS_PENDING;
                break;
            }

            /* check to see if it was an abort on the session side. */
            if(request->status != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "Request completed with error status %s.",
                       plc_tag_decode_error(request->status));
                rc = request->status;
                break;
            }
        }

        /* if we failed above, punt out of the do/while loop. */
        if(rc != PLCTAG_STATUS_OK) { break; }

        /* the session already checked the framing and handed over only the CIP reply. */
        if(request->request_size < (int)sizeof(cip_header)) { rc = PLCTAG_ERR_TOO_SMALL; }
    } while(0);

    /* if this is still hanging around, release the reference */
    if(request) { request = rc_dec(request); }

    if(rc_is_error(rc)) {
        /* the request is dead, from session side. */
        ab_tag_abort(tag);

        /* the create's own read failed, so the create did. */
        if(tag->first_read) {
            tag->first_read = 0;
            tag->status = (int8_t)rc;
            tag->read_complete = 1;
            tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, (int8_t)rc);
        }

        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, tag->tag_id, "Response not OK with status %s.", plc_tag_decode_error(rc));
    }

    /* FIXME - This is not correct */
    // tag->status = (int8_t)rc;

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_SPEW, tag->tag_id, "Done with tag status %s.", plc_tag_decode_error(rc));

    return rc;
}
