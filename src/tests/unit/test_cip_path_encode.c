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
 * Byte-exact expectations for cip_encode_path().
 *
 * This exists to pin the CURRENT encoder before it is rewritten as a recursive
 * descent parser over port/node pairs.  Every expectation here was read out of
 * the existing implementation, not derived from the CIP specification, so a
 * failure after the rewrite means the rewrite changed behaviour -- which may be
 * a fix or may be a regression, but is never silent.
 *
 * Path encoding is almost untested elsewhere.  The simulator suite runs "1,0"
 * and little else; DH+ and CIP bridging paths only ever appear against real
 * hardware.  That is what makes this table worth having.
 *
 * Note on the DH+ form: "A:27:1" is one comma-list item that emits no port/node
 * pair of its own.  It records a port and a destination node, and the driver
 * appends a six-byte logical segment (class 0xA6, instance = port, connection
 * point 1).  The middle field -- the source node -- is parsed and range checked
 * and then never read by anything; the wire always carries zero for it.
 */

#include "mini_mock.h"

#include <libplctag/lib/libplctag.h>
#include <libplctag/protocols/ab/cip.h>
#include <libplctag/protocols/ab/defs.h>
#include <libplctag/protocols/cip/defs.h>
#include <stdio.h>
#include <string.h>


#define MAX_EXPECTED (64)

typedef struct {
    const char *name;
    const char *path;
    plc_type_t plc_type;
    int needs_connection_in;

    int expected_rc;
    /* the fields below are only checked when expected_rc is PLCTAG_STATUS_OK */
    uint8_t expected[MAX_EXPECTED];
    int expected_size;
    int expected_needs_connection;
    int expected_is_dhp;
    uint16_t expected_dhp_dest;
} path_case_t;


/* "18,10.206.1.39" -> port byte, length, the ASCII address, pad to an even length. */
#define IP_SEG_1_39 0x12, 0x0B, '1', '0', '.', '2', '0', '6', '.', '1', '.', '3', '9', 0x00

/* the six bytes the driver appends for a DH+ path: class 0xA6, instance = port, conn point 1 */
#define DHP_TAIL(port) 0x20, 0xA6, 0x24, (port), 0x2C, 0x01

/* the four bytes appended when a PLC needs a connection: the message router object */
#define ROUTER_PATH 0x20, 0x02, 0x24, 0x01


static const path_case_t cases[] = {
    /* --- the ordinary backplane paths, which is nearly all the suite exercises --- */
    {"slot 0, unconnected", "1,0", AB_PLC_LGX, 0, PLCTAG_STATUS_OK, {0x01, 0x00}, 2, 0, 0, 0},
    {"slot 0, connected", "1,0", AB_PLC_LGX, 1, PLCTAG_STATUS_OK, {0x01, 0x00, ROUTER_PATH}, 6, 1, 0, 0},
    {"slot 4", "1,4", AB_PLC_LGX, 0, PLCTAG_STATUS_OK, {0x01, 0x04}, 2, 0, 0, 0},
    {"slot 5", "1,5", AB_PLC_LGX, 0, PLCTAG_STATUS_OK, {0x01, 0x05}, 2, 0, 0, 0},

    /* a lone segment is zero padded up to a 16-bit boundary */
    {"single segment pads", "1", AB_PLC_LGX, 0, PLCTAG_STATUS_OK, {0x01, 0x00}, 2, 0, 0, 0},

    /* spaces are skipped before and after each segment */
    {"spaces around separator", "1, 0", AB_PLC_LGX, 0, PLCTAG_STATUS_OK, {0x01, 0x00}, 2, 0, 0, 0},

    /* an empty path encodes to nothing at all rather than failing */
    {"empty path", "", AB_PLC_LGX, 0, PLCTAG_STATUS_OK, {0}, 0, 0, 0, 0},

    /* --- extended link addresses: the IP form --- */
    {"extended address port 18", "18,10.206.1.39", AB_PLC_LGX, 0, PLCTAG_STATUS_OK, {IP_SEG_1_39}, 14, 0, 0, 0},
    {"CIP bridge, hardware suite path", "1,4,18,10.206.1.39,1,0", AB_PLC_LGX, 0, PLCTAG_STATUS_OK,
     {0x01, 0x04, IP_SEG_1_39, 0x01, 0x00}, 18, 0, 0, 0},

    /*
     * The same bridge path with the port written as a channel letter.  These two
     * must encode identically; the hardware suite runs both so the wire agrees
     * with the table.
     */
    {"CIP bridge, channel letter port", "1,4,A,10.206.1.39,1,0", AB_PLC_LGX, 0, PLCTAG_STATUS_OK,
     {0x01, 0x04, IP_SEG_1_39, 0x01, 0x00}, 18, 0, 0, 0},

    /*
     * CHANGED by the port/node rewrite.  The old encoder recognised only 18 and
     * 19 as extended-address ports, so channels A2 and B2 had no spelling at
     * all.  A numeric port is now taken literally as the port byte, so 20 and
     * 21 work, and the channel letters resolve to the same bytes.
     */
    {"extended address port 20", "20,10.206.1.39", AB_PLC_LGX, 0, PLCTAG_STATUS_OK,
     {0x14, 0x0B, '1', '0', '.', '2', '0', '6', '.', '1', '.', '3', '9', 0x00}, 14, 0, 0, 0},
    {"channel A with IP is port 18", "A,10.206.1.39", AB_PLC_LGX, 0, PLCTAG_STATUS_OK, {IP_SEG_1_39}, 14, 0, 0, 0},
    {"channel A2 with IP is port 20", "A2,10.206.1.39", AB_PLC_LGX, 0, PLCTAG_STATUS_OK,
     {0x14, 0x0B, '1', '0', '.', '2', '0', '6', '.', '1', '.', '3', '9', 0x00}, 14, 0, 0, 0},
    {"channel B with IP is port 19", "B,10.206.1.39", AB_PLC_LGX, 0, PLCTAG_STATUS_OK,
     {0x13, 0x0B, '1', '0', '.', '2', '0', '6', '.', '1', '.', '3', '9', 0x00}, 14, 0, 0, 0},

    /* a channel letter with a plain node is the channel number, unadjusted. */
    {"channel A with numeric node", "A,42", AB_PLC_LGX, 0, PLCTAG_STATUS_OK, {0x01, 0x2A}, 2, 0, 0, 0},
    {"channel B2 with numeric node", "B2,42", AB_PLC_LGX, 0, PLCTAG_STATUS_OK, {0x04, 0x2A}, 2, 0, 0, 0},

    /* '/' separates a port from its node as well as ',' does. */
    {"slash separator", "1/4", AB_PLC_LGX, 0, PLCTAG_STATUS_OK, {0x01, 0x04}, 2, 0, 0, 0},

    /* --- the legacy DH+ item --- */
    {"DH+ channel A", "1,2,A:27:1", AB_PLC_PLC5, 0, PLCTAG_STATUS_OK, {0x01, 0x02, DHP_TAIL(0x01)}, 8, 1, 1, 1},
    {"DH+ channel A alone", "A:27:1", AB_PLC_SLC, 0, PLCTAG_STATUS_OK, {DHP_TAIL(0x01)}, 6, 1, 1, 1},
    {"DH+ channel B", "B:27:5", AB_PLC_PLC5, 0, PLCTAG_STATUS_OK, {DHP_TAIL(0x02)}, 6, 1, 1, 5},
    {"DH+ lower case a", "a:27:1", AB_PLC_PLC5, 0, PLCTAG_STATUS_OK, {DHP_TAIL(0x01)}, 6, 1, 1, 1},

    /*
     * The old DH+ matcher also accepted '2' and '3' as aliases for channels A
     * and B, but that code was unreachable: the numeric matcher ran first and
     * consumed the leading digit, leaving ":27:1" that matched nothing.  The
     * rewrite keeps the rejection -- only a channel letter introduces a DH+
     * item -- and the alias arms went with the matcher.
     */
    {"DH+ digit alias 2 is unreachable", "2:27:1", AB_PLC_MLGX, 0, PLCTAG_ERR_BAD_PARAM, {0}, 0, 0, 0, 0},
    {"DH+ digit alias 3 is unreachable", "3:27:1", AB_PLC_MLGX, 0, PLCTAG_ERR_BAD_PARAM, {0}, 0, 0, 0, 0},

    /* the source node is accepted and validated, then discarded */
    {"DH+ source node is ignored", "A:99:1", AB_PLC_PLC5, 0, PLCTAG_STATUS_OK, {DHP_TAIL(0x01)}, 6, 1, 1, 1},

    /* --- rejections --- */
    {"DH+ must be last", "A:27:1,1,0", AB_PLC_PLC5, 0, PLCTAG_ERR_BAD_PARAM, {0}, 0, 0, 0, 0},
    {"DH+ only on PCCC PLCs", "A:27:1", AB_PLC_LGX, 0, PLCTAG_ERR_BAD_PARAM, {0}, 0, 0, 0, 0},
    {"DH+ source node out of range", "A:900:1", AB_PLC_PLC5, 0, PLCTAG_ERR_BAD_PARAM, {0}, 0, 0, 0, 0},

    /*
     * CHANGED by the port/node rewrite.  The old encoder ran every number through
     * one matcher that capped at 0x11, which applied a port's range to node
     * addresses too -- so a DeviceNet or ControlNet node above 17 could not be
     * written.  A node is a link address and now takes the full byte.
     */
    {"node above 0x11", "1,99", AB_PLC_LGX, 0, PLCTAG_STATUS_OK, {0x01, 0x63}, 2, 0, 0, 0},
    {"node at 0x11", "1,17", AB_PLC_LGX, 0, PLCTAG_STATUS_OK, {0x01, 0x11}, 2, 0, 0, 0},
    {"node at the top of the byte", "1,255", AB_PLC_LGX, 0, PLCTAG_STATUS_OK, {0x01, 0xFF}, 2, 0, 0, 0},
    {"node past the top of the byte", "1,256", AB_PLC_LGX, 0, PLCTAG_ERR_OUT_OF_BOUNDS, {0}, 0, 0, 0, 0},

    {"garbage", "not-a-path", AB_PLC_LGX, 0, PLCTAG_ERR_BAD_PARAM, {0}, 0, 0, 0, 0},
};


static void dump(const char *label, const uint8_t *buf, int size) {
    fprintf(stderr, "  %-9s (%2d):", label, size);
    for(int i = 0; i < size; i++) { fprintf(stderr, " %02X", buf[i]); }
    fprintf(stderr, "\n");
}


static void run_case(const path_case_t *c) {
    uint8_t buf[MAX_CONN_PATH + MAX_IP_ADDR_SEG_LEN];
    int buf_size = (int)sizeof(buf);
    int needs_connection = c->needs_connection_in;
    int is_dhp = 0;
    uint16_t dhp_dest = 0;
    int rc = PLCTAG_STATUS_OK;

    memset(buf, 0xEE, sizeof(buf));

    rc = cip_encode_path(c->path, &needs_connection, c->plc_type, buf, &buf_size, &is_dhp, &dhp_dest);

    if(rc != c->expected_rc) {
        fprintf(stderr, "%s: path \"%s\" returned %s, expected %s\n", c->name, c->path, plc_tag_decode_error(rc),
                plc_tag_decode_error(c->expected_rc));
        abort();
    }

    if(rc != PLCTAG_STATUS_OK) { return; }

    if(buf_size != c->expected_size || memcmp(buf, c->expected, (size_t)c->expected_size) != 0) {
        fprintf(stderr, "%s: path \"%s\" encoded wrongly\n", c->name, c->path);
        dump("expected", c->expected, c->expected_size);
        dump("actual", buf, buf_size);
        abort();
    }

    if(needs_connection != c->expected_needs_connection) {
        fprintf(stderr, "%s: path \"%s\" needs_connection %d, expected %d\n", c->name, c->path, needs_connection,
                c->expected_needs_connection);
        abort();
    }

    if(is_dhp != c->expected_is_dhp) {
        fprintf(stderr, "%s: path \"%s\" is_dhp %d, expected %d\n", c->name, c->path, is_dhp, c->expected_is_dhp);
        abort();
    }

    if(dhp_dest != c->expected_dhp_dest) {
        fprintf(stderr, "%s: path \"%s\" dhp_dest %u, expected %u\n", c->name, c->path, (unsigned int)dhp_dest,
                (unsigned int)c->expected_dhp_dest);
        abort();
    }
}


static void test_path_table(void **state) {
    (void)state;

    for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) { run_case(&cases[i]); }
}


/*
 * The driver stops as soon as the encoded path reaches its limit, and reports
 * TOO_LARGE rather than quietly truncating.
 */
static void test_over_long_path(void **state) {
    uint8_t buf[MAX_CONN_PATH + MAX_IP_ADDR_SEG_LEN];
    int buf_size = (int)sizeof(buf);
    int needs_connection = 0;
    int is_dhp = 0;
    uint16_t dhp_dest = 0;
    char path[1024] = {0};
    size_t used = 0;

    (void)state;

    /* 400 single byte segments, comfortably past MAX_CONN_PATH. */
    for(int i = 0; i < 400 && used < sizeof(path) - 4; i++) { used += (size_t)snprintf(path + used, sizeof(path) - used, "1,"); }

    assert_int_equal(cip_encode_path(path, &needs_connection, AB_PLC_LGX, buf, &buf_size, &is_dhp, &dhp_dest),
                     PLCTAG_ERR_TOO_LARGE);
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_path_table),
        cmocka_unit_test(test_over_long_path),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
