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
 * ENIP Manufacturer Strategy Selector
 *
 * STATUS: KEEP AS-IS.  Two cosmetic fixes needed.
 *
 * Phase 0: Change DEBUG_MODULE_LIB to DEBUG_MODULE_ENIP in all pdebug calls.
 * Phase 0: No functional changes — selection logic is correct.
 *
 * The function is called from enip_connection_get_identity after parsing the
 * identity response and is correct for all three strategies (AB, OMRON, PCCC).
 * Maps Get Identity response (vendor_id, device_type) to the correct mfg_ops struct.
 */

#include <libplctag/protocols/enip/enip_mfg_ops.h>
#include <utils/debug.h>
#include <string.h>


/* ============================================================================
 * Vendor ID Constants (from ODVA registry)
 * ============================================================================ */

#define VENDOR_ID_ALLEN_BRADLEY 0x0001
#define VENDOR_ID_OMRON 0x00FA


/* ============================================================================
 * Strategy Selection Function
 * ============================================================================ */

/* Phase 0: change DEBUG_MODULE_LIB -> DEBUG_MODULE_ENIP in all pdebug calls inside.
 * Phase 2: called from enip_connection_get_identity — correct as-is. */
enip_mfg_ops_t *enip_select_mfg_ops(enip_identity_t *identity) {
    if(!identity) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "ENIP: NULL identity in select_mfg_ops");
        return NULL;
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "ENIP: Selecting strategy for vendor=0x%04x device=0x%04x product=0x%04x",
           identity->vendor_id, identity->device_type, identity->product_code);

    /* Allen-Bradley family */
    if(identity->vendor_id == VENDOR_ID_ALLEN_BRADLEY) {
        /*
         * Device type classification:
         * - 0x0E: CompactLogix 5000 (L3x, L5x series)
         * - 0x23: ControlLogix 5000 (L5, L7 series)
         * - 0x6B: Generic Programmable Logic Controller (catchall)
         */

        pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "ENIP: Selected AB/Logix strategy");
        return &enip_mfg_ab;
    }

    /* Omron family */
    if(identity->vendor_id == VENDOR_ID_OMRON) {
        /*
         * Device type classification:
         * - 0x01: Generic PLC (typical for Omron ENJ)
         */

        pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "ENIP: Selected OMRON strategy");
        return &enip_mfg_omron;
    }

    /* Check for PCCC variants by device characteristics
     * PCCC devices typically expose through EtherNet/IP gateway
     * Product name may contain hints (e.g., "PLC5", "SLC", "DH+") */

    if(identity->product_name
       && (strstr(identity->product_name, "PLC5") || strstr(identity->product_name, "SLC")
           || strstr(identity->product_name, "DH"))) {

        pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "ENIP: Selected PCCC strategy (detected by name)");
        return &enip_mfg_pccc;
    }

    /* Default fallback: Try AB/Logix for unknown Allen-Bradley-compatible devices */
    pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "ENIP: Unknown vendor 0x%04x, falling back to AB strategy", identity->vendor_id);
    return &enip_mfg_ab;
}


/* Placeholder symbol to ensure file links */
int enip_mfg_selector_placeholder_symbol = 0;
