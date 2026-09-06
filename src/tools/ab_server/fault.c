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

#include <stdlib.h>
#include <string.h>

#include "fault.h"
#include "log.h"
#include "utils.h"

/* indexed by fault_kind_t, so this must stay in the same order as the enum. */
static const char *fault_names[FAULT_MAX] = {
    "none",    "cpf_count", "cpf_type", "conn_id", "item_len",   "short_cpf",
    "short_cip", "eip_cmd", "session", "context", "pccc_reply", "pccc_tns",
};


const char *fault_kind_name(fault_kind_t kind) {
    if(kind <= FAULT_NONE || kind >= FAULT_MAX) { return "none"; }

    return fault_names[kind];
}


bool fault_parse(const char *spec, plc_s *plc) {
    const char *colon = NULL;
    size_t name_len = 0;
    long count = 1;

    if(!spec || !plc) { return false; }

    /*
     * "<kind>" or "<kind>:<count>".  The count is separated with a colon rather than another
     * "=" because args_parse() has already split the argument at the first "=".
     */
    colon = strchr(spec, ':');
    name_len = colon ? (size_t)(colon - spec) : strlen(spec);

    if(colon) {
        char *end = NULL;

        count = strtol(colon + 1, &end, 10);

        if(end == colon + 1 || *end != '\0' || count < 1 || count > INT32_MAX) {
            log_error("Fault count in \"%s\" must be a positive number.", spec);
            return false;
        }
    }

    for(int kind = FAULT_NONE + 1; kind < FAULT_MAX; kind++) {
        if(strlen(fault_names[kind]) == name_len && strncmp(spec, fault_names[kind], name_len) == 0) {
            atomic_store_int32(plc->fault_counts[kind], (int32_t)count);
            log_info_always("Corrupting the next %ld response(s) with fault \"%s\".", count, fault_names[kind]);
            return true;
        }
    }

    log_error("Unknown fault kind in \"%s\".", spec);

    return false;
}


bool fault_fires(plc_s *plc, fault_kind_t kind) {
    int32_t remaining = 0;

    if(!plc || kind <= FAULT_NONE || kind >= FAULT_MAX || !plc->fault_counts[kind]) { return false; }

    /*
     * Single atomic operation rather than a check followed by a decrement: the counters are
     * shared across every connection's copy of plc_s, so two connections arriving with a count
     * of one must not both decide to fire.  atomic_dec_int32() returns the value from before
     * the decrement, so a value above zero means this caller won.
     *
     * ponytail: like reject_fo_count this keeps counting down past zero once exhausted, which
     * is harmless for a test tool at any plausible request rate.
     */
    remaining = atomic_dec_int32(plc->fault_counts[kind]);

    if(remaining > 0) {
        log_info("Injecting fault \"%s\", %d to go.", fault_names[kind], remaining - 1);
        return true;
    }

    return false;
}
