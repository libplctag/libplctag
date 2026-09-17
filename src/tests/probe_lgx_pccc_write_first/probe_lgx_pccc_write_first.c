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
 * Does a Logix-mapped PCCC write carry type information when the tag has never
 * been read?  See 1.10 in docs/deferred_fixes.md.
 *
 * A PCCC typed write puts an encoded type descriptor ahead of the data.  For
 * this PLC family the descriptor is filled in only by a completed read
 * (eip_lgx_pccc.c:394), and tag_write_start() is supposed to turn a write into a
 * read when none has happened yet.  That guard is gated on tag->first_read, which
 * ab_common.c:460 sets to 0 for AB_PLC_LGX_PCCC and nothing sets back.  If the
 * guard really cannot fire, a write issued before any read goes out with a
 * zero-length type prefix.
 *
 * There is no way to read encoded_type_info_size back through the public API for
 * this PLC type -- "raw_tag_type_bytes.length" covers AB_PLC_LGX and
 * AB_PLC_MICRO800 only -- so this does not try to measure it.  It puts both
 * frames on the wire instead, in one process, against the same address, and
 * marks them so the two packet dumps can be compared:
 *
 *     probe_lgx_pccc_write_first "protocol=ab-eip&gateway=..." N7:0 42 4
 *
 * Then compare what went out in each half.  The two writes should be identical.
 * If the write-first frame is shorter, that is the missing type prefix, and the
 * difference is exactly the byte count 1.10 predicts.
 *
 * This is a measurement tool, not a pass/fail test.  It exits 0 whenever both
 * scenarios ran, whatever the PLC made of them -- a refusal is a result, not an
 * error, and the frame was still emitted and logged either way.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <libplctag/lib/libplctag.h>


#define TIMEOUT_MS (5000)


static const char *status_name(int rc) { return plc_tag_decode_error(rc); }


/*
 * Build one tag and exercise it.  When read_first is false the write is the very
 * first operation the tag ever performs, which is the case under test.
 */
static int run_scenario(const char *label, const char *attributes, const char *name, int16_t value, bool read_first) {
    char tag_string[512];
    int32_t tag = 0;
    int rc = PLCTAG_STATUS_OK;

    printf("\n===== SCENARIO %s =====\n", label);
    fflush(stdout);

    if(snprintf(tag_string, sizeof(tag_string), "%s&name=%s", attributes, name) >= (int)sizeof(tag_string)) {
        printf("%s: tag string is too long for the buffer.\n", label);
        return PLCTAG_ERR_TOO_LARGE;
    }

    tag = plc_tag_create(tag_string, TIMEOUT_MS);
    if(tag < 0) {
        printf("%s: create failed: %s\n", label, status_name((int)tag));
        return (int)tag;
    }

    printf("%s: created.\n", label);
    fflush(stdout);

    if(read_first) {
        printf("----- %s: read -----\n", label);
        fflush(stdout);

        rc = plc_tag_read(tag, TIMEOUT_MS);
        printf("%s: read returned %s\n", label, status_name(rc));
        fflush(stdout);

        if(rc != PLCTAG_STATUS_OK) {
            plc_tag_destroy(tag);
            return rc;
        }
    }

    rc = plc_tag_set_int16(tag, 0, value);
    if(rc != PLCTAG_STATUS_OK) {
        printf("%s: unable to set the value: %s\n", label, status_name(rc));
        plc_tag_destroy(tag);
        return rc;
    }

    printf("----- %s: write -----\n", label);
    fflush(stdout);

    rc = plc_tag_write(tag, TIMEOUT_MS);
    printf("%s: write returned %s\n", label, status_name(rc));
    fflush(stdout);

    plc_tag_destroy(tag);

    return rc;
}


int main(int argc, char **argv) {
    const char *attributes = NULL;
    const char *name = NULL;
    int16_t value = 0;
    int write_first_rc = PLCTAG_STATUS_OK;
    int read_first_rc = PLCTAG_STATUS_OK;

    if(argc < 4 || argc > 5) {
        printf("Usage: %s <tag attributes, no name> <name> <int16 value> [debug level]\n", argv[0]);
        printf("   e.g. %s 'protocol=ab-eip&gateway=10.1.2.3&plc=lgxpccc&path=1,0&elem_count=1' N7:0 42 4\n", argv[0]);
        printf("\nRun with debug level 4 and compare the packets sent by the two scenarios.\n");
        return 1;
    }

    attributes = argv[1];
    name = argv[2];
    value = (int16_t)atoi(argv[3]);

    if(argc == 5) { plc_tag_set_debug_level(atoi(argv[4])); }

    /* the case under test: a write with no read ahead of it, on a brand new tag. */
    write_first_rc = run_scenario("WRITE-FIRST", attributes, name, value, false);

    /* the control: the same write, on a tag that has completed a read. */
    read_first_rc = run_scenario("READ-FIRST", attributes, name, (int16_t)(value + 1), true);

    printf("\n===== RESULT =====\n");
    printf("write with no preceding read: %s\n", status_name(write_first_rc));
    printf("write after a read:           %s\n", status_name(read_first_rc));

    if(write_first_rc == PLCTAG_STATUS_OK && read_first_rc == PLCTAG_STATUS_OK) {
        printf("\nBoth writes were accepted.  Compare the two frames in the log: if they\n"
               "differ in length, the PLC accepted a typed write with no type prefix and\n"
               "1.10 is real but harmless against this device.  If they are identical,\n"
               "1.10 does not hold and the pre-write-read machinery is dead code.\n");
    } else if(read_first_rc == PLCTAG_STATUS_OK) {
        printf("\nThe write-first case failed where the read-first case succeeded.  That is\n"
               "1.10: the frame went out without its type prefix.\n");
    }

    /* a refusal is a measurement, not a failure of the probe. */
    return 0;
}
