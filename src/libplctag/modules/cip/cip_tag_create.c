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
 * The CIP PLC dispatch table.
 *
 * Eleven rows for eight PLC types, because three of them -- PLC/5, SLC and MicroLogix --
 * are reached either directly or across a DH+ bridge and the two cases set different
 * messaging.  Rows map many-to-one onto leaf functions: the four real behaviours are PCCC
 * over CIP, PCCC-mapped Logix, CIP symbolic and OMRON's CIP symbolic, so the leaf count
 * stays at the number of behaviours rather than the number of configurations.
 *
 * A NULL setup_tag means that family has not been moved off the old per-family switch in
 * its module yet.  The rows exist anyway so the table is complete and the invariant test
 * has something to check.
 */

#include <libplctag/api/libplctag.h>
#include <libplctag/modules/ab/ab.h>
#include <libplctag/modules/ab/ab_common.h>
#include <libplctag/modules/omron/omron.h>
#include <libplctag/modules/omron/omron_common.h>
#include <platform.h>
#include <libplctag/modules/cip/conn.h>
#include <libplctag/modules/cip/path.h>
#include <libplctag/modules/cip/plc_type.h>
#include <libplctag/modules/cip/cip_tag_create.h>
#include <stdint.h>
#include <utils/attr.h>
#include <utils/debug.h>


static const cip_plc_entry_t cip_plc_entries[] = {
    {.name = "PLC/5", .plc_type = CIP_PLC_PLC5, .dialect = CIP_DIALECT_DIRECT, .use_connected_msg = 0, .setup_tag = ab_setup_plc5_tag},
    {.name = "PLC/5 via DH+", .plc_type = CIP_PLC_PLC5, .dialect = CIP_DIALECT_DHP, .use_connected_msg = 1, .setup_tag = ab_setup_plc5_tag},
    {.name = "SLC 500", .plc_type = CIP_PLC_SLC, .dialect = CIP_DIALECT_DIRECT, .use_connected_msg = 0, .setup_tag = ab_setup_slc_tag},
    {.name = "SLC 500 via DH+", .plc_type = CIP_PLC_SLC, .dialect = CIP_DIALECT_DHP, .use_connected_msg = 1, .setup_tag = ab_setup_slc_tag},
    {.name = "MicroLogix", .plc_type = CIP_PLC_MLGX, .dialect = CIP_DIALECT_DIRECT, .use_connected_msg = 0, .setup_tag = ab_setup_slc_tag},
    {.name = "MicroLogix via DH+", .plc_type = CIP_PLC_MLGX, .dialect = CIP_DIALECT_DHP, .use_connected_msg = 1, .setup_tag = ab_setup_slc_tag},
    {.name = "PCCC-mapped Logix", .plc_type = CIP_PLC_LGX_PCCC, .dialect = CIP_DIALECT_DIRECT, .use_connected_msg = 0, .setup_tag = ab_setup_lgx_pccc_tag},
    {.name = "Logix", .plc_type = CIP_PLC_LGX, .dialect = CIP_DIALECT_DIRECT, .use_connected_msg = -1, .setup_tag = ab_setup_logix_tag},
    {.name = "Micro800", .plc_type = CIP_PLC_MICRO800, .dialect = CIP_DIALECT_DIRECT, .use_connected_msg = 1, .setup_tag = ab_setup_micro800_tag},
    {.name = "generic CIP", .plc_type = CIP_PLC_GENERIC, .dialect = CIP_DIALECT_DIRECT, .use_connected_msg = 0, .setup_tag = ab_setup_generic_tag},
    {.name = "OMRON NJ/NX", .plc_type = CIP_PLC_OMRON_NJNX, .dialect = CIP_DIALECT_DIRECT, .use_connected_msg = 1, .setup_tag = omron_setup_njnx_tag},
};


bool cip_entry_use_connected_msg(const cip_plc_entry_t *entry, attr attribs) {
    /* the one family that lets the tag string decide, and it defaults to connected. */
    if(entry->use_connected_msg < 0) { return (attr_get_int(attribs, "use_connected_msg", 1) != 0); }

    return (entry->use_connected_msg != 0);
}


const cip_plc_entry_t *cip_plc_entry_find(cip_plc_type_t plc_type, cip_dialect_t dialect) {
    size_t index = 0;

    for(index = 0; index < (sizeof(cip_plc_entries) / sizeof(cip_plc_entries[0])); index++) {
        if(cip_plc_entries[index].plc_type == plc_type && cip_plc_entries[index].dialect == dialect) {
            return &cip_plc_entries[index];
        }
    }

    /*
     * No warning here.  cip_dialect_from_attributes() asks this for a DH+ row on every tag
     * create precisely to find out whether the family has one, so a miss is the normal
     * answer rather than a problem.  The callers that do treat a miss as an error say so
     * themselves, with the PLC type they were given.
     */
    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "No PLC table entry for PLC type %d, dialect %d.", (int)plc_type, (int)dialect);

    return NULL;
}


cip_dialect_t cip_dialect_from_attributes(cip_plc_type_t plc_type, attr attribs) {
    const char *path = attr_get_str(attribs, "path", NULL);
    uint8_t encoded_path[MAX_CONN_PATH + MAX_IP_ADDR_SEG_LEN];
    int encoded_path_size = (int)sizeof(encoded_path);
    int needs_connection = 0;
    int is_dhp = 0;
    uint16_t dhp_dest = 0;
    bool is_bridged = false;

    /* a family with no DH+ row cannot bridge, so the path cannot change the answer. */
    if(!cip_plc_entry_find(plc_type, CIP_DIALECT_DHP)) { return CIP_DIALECT_DIRECT; }

    /*
     * The same parser the connection will use, so the two cannot disagree about what a path
     * means.  Only is_dhp is wanted here; the encoded path itself is built again, properly
     * sized, when the connection is created.
     */
    if(cip_encode_path(path, &needs_connection, CIP_PLC_KIND_DHP_CAPABLE, &encoded_path[0], &encoded_path_size, &is_dhp,
                       &dhp_dest, &is_bridged)
       != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Path does not parse; treating the tag as direct.");
        return CIP_DIALECT_DIRECT;
    }

    return (is_dhp ? CIP_DIALECT_DHP : CIP_DIALECT_DIRECT);
}


cip_plc_type_t cip_plc_type_from_attributes(attr attribs) {
    const char *cpu_type = attr_get_str(attribs, "plc", attr_get_str(attribs, "cpu", "NONE"));

    if(!str_cmp_i(cpu_type, "plc") || !str_cmp_i(cpu_type, "plc5")) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Found PLC/5 PLC.");
        return CIP_PLC_PLC5;
    } else if(!str_cmp_i(cpu_type, "slc") || !str_cmp_i(cpu_type, "slc500")) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Found SLC 500 PLC.");
        return CIP_PLC_SLC;
    } else if(!str_cmp_i(cpu_type, "lgxpccc") || !str_cmp_i(cpu_type, "logixpccc") || !str_cmp_i(cpu_type, "lgxplc5")
              || !str_cmp_i(cpu_type, "logixplc5") || !str_cmp_i(cpu_type, "lgx-pccc") || !str_cmp_i(cpu_type, "logix-pccc")
              || !str_cmp_i(cpu_type, "lgx-plc5") || !str_cmp_i(cpu_type, "logix-plc5")) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Found Logix-class PLC using PCCC protocol.");
        return CIP_PLC_LGX_PCCC;
    } else if(!str_cmp_i(cpu_type, "micrologix800") || !str_cmp_i(cpu_type, "mlgx800") || !str_cmp_i(cpu_type, "micro800")) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Found Micro8xx PLC.");
        return CIP_PLC_MICRO800;
    } else if(!str_cmp_i(cpu_type, "micrologix") || !str_cmp_i(cpu_type, "mlgx")) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Found MicroLogix PLC.");
        return CIP_PLC_MLGX;
    } else if(!str_cmp_i(cpu_type, "compactlogix") || !str_cmp_i(cpu_type, "clgx") || !str_cmp_i(cpu_type, "lgx")
              || !str_cmp_i(cpu_type, "controllogix") || !str_cmp_i(cpu_type, "contrologix") || !str_cmp_i(cpu_type, "logix")) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Found ControlLogix/CompactLogix PLC.");
        return CIP_PLC_LGX;
    } else if(!str_cmp_i(cpu_type, "omron-njnx") || !str_cmp_i(cpu_type, "omron-nj") || !str_cmp_i(cpu_type, "omron-nx")
              || !str_cmp_i(cpu_type, "njnx") || !str_cmp_i(cpu_type, "nx1p2")) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Found OMRON NJ/NX Series PLC.");
        return CIP_PLC_OMRON_NJNX;
    } else if(!str_cmp_i(cpu_type, "generic") || !str_cmp_i(cpu_type, "cip")) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Found generic CIP device.");
        return CIP_PLC_GENERIC;
    } else {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unsupported device type: %s", cpu_type);

        return CIP_PLC_NONE;
    }
}


int cip_tag_create(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                   void *userdata, plc_tag_p src_tag, plc_tag_p *tag_out) {
    /*
     * Both families arrive as protocol=ab-eip and are told apart by the plc attribute, so
     * something has to read that attribute and pick a module.  That something is this file,
     * which is above both of them.  It used to be ab_tag_create(), which meant the Rockwell
     * module called the OMRON module's constructor and had to know the OMRON spellings of
     * the plc attribute to do it.
     *
     * A cloned tag names its module directly and needs no attribute at all.
     */
    if(src_tag) {
        switch(src_tag->protocol_type) {
            case TAG_PROTOCOL_OMRON:
            case TAG_PROTOCOL_OMRON_CONNECTION: return omron_tag_create(attribs, tag_callback_func, userdata, src_tag, tag_out);

            default: return ab_tag_create(attribs, tag_callback_func, userdata, src_tag, tag_out);
        }
    }

    if(cip_plc_type_from_attributes(attribs) == CIP_PLC_OMRON_NJNX) {
        return omron_tag_create(attribs, tag_callback_func, userdata, src_tag, tag_out);
    }

    return ab_tag_create(attribs, tag_callback_func, userdata, src_tag, tag_out);
}
