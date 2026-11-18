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
 * P1: a Forward Close must carry the session handle and nothing left over from the last packet.
 * P2: the encoded path must fit conn_path_size (8 bits) with every suffix added, here or later.
 * Compiles session.c in to reach the static Forward Close encoder.
 */

#include "mini_mock.h"

#include "../../libplctag/protocols/ab/session.c"

#include <string.h>


static void test_forward_close_header(void **state) {
    static uint8_t path[2] = {0x01, 0x00};
    cip_conn_p session = NULL;
    exchange_t ex = {.state = EX_IDLE};
    eip_forward_close_req_t *fc = NULL;

    (void)state;

    session = (cip_conn_p)calloc(1, sizeof(*session));
    assert_non_null(session);

    /* a stale buffer: every header field the encoder leaves alone shows up as 0xAA. */
    memset(session->data, 0xAA, sizeof(session->data));
    session->session_handle = 0x11223344;
    session->targ_connection_id = 1;
    session->conn_path = path;
    session->conn_path_size = (uint8_t)sizeof(path);

    assert_int_equal(session_forward_close(session, &ex), PLCTAG_STATUS_PENDING);

    fc = (eip_forward_close_req_t *)session->data;
    assert_true(le2h32(fc->encap_session_handle) == 0x11223344);
    assert_true(le2h32(fc->encap_status) == 0);
    assert_true(le2h32(fc->encap_options) == 0);
    assert_true(le2h32(fc->interface_handle) == 0);
    assert_int_equal(fc->reserved, 0);

    free(session);
}


/* n port segments of "1": one byte each. */
static int encode_n(int n, int needs_connection, int *size) {
    static char path[1024];
    static uint8_t buf[MAX_CONN_PATH + MAX_IP_ADDR_SEG_LEN];
    int is_dhp = 0;
    uint16_t dhp_dest = 0;
    int32_t i;

    path[0] = 0;
    for(i = 0; i < n; i++) { strcat(path, i ? ",1" : "1"); }

    *size = (int)sizeof(buf);
    return cip_encode_path(path, &needs_connection, AB_PLC_LGX, buf, size, &is_dhp, &dhp_dest);
}


/* an IP address port segment: port 18, length, the address as text, a pad to an even length. */
static void test_ip_addr_segment(void **state) {
    static const uint8_t want[] = {0x01, 0x00, 0x12, 0x09, '1', '0', '.', '1', '.', '2', '.', '3', '4', 0x00, 0x01, 0x00};
    uint8_t buf[MAX_CONN_PATH + MAX_IP_ADDR_SEG_LEN];
    int needs_connection = 1;
    int is_dhp = 0;
    uint16_t dhp_dest = 0;
    int size = (int)sizeof(buf);

    (void)state;

    assert_int_equal(cip_encode_path("1,0,18,10.1.2.34,1,0", &needs_connection, AB_PLC_LGX, buf, &size, &is_dhp, &dhp_dest),
                     PLCTAG_STATUS_OK);
    /* a connected path then gets the Message Router, which test_path_fits_8_bits covers. */
    assert_true(size >= (int)sizeof(want));
    assert_int_equal(memcmp(buf, want, sizeof(want)), 0);

    /* an octet over 255, a missing octet, and a run of digits long enough to overflow an int. */
    size = (int)sizeof(buf);
    assert_int_equal(cip_encode_path("18,10.1.2.256", &needs_connection, AB_PLC_LGX, buf, &size, &is_dhp, &dhp_dest),
                     PLCTAG_ERR_BAD_PARAM);
    size = (int)sizeof(buf);
    assert_int_equal(cip_encode_path("18,10..2.3", &needs_connection, AB_PLC_LGX, buf, &size, &is_dhp, &dhp_dest),
                     PLCTAG_ERR_BAD_PARAM);
    size = (int)sizeof(buf);
    assert_int_equal(cip_encode_path("18,99999999999999.1.2.3", &needs_connection, AB_PLC_LGX, buf, &size, &is_dhp, &dhp_dest),
                     PLCTAG_ERR_BAD_PARAM);
}


static void test_path_fits_8_bits(void **state) {
    int size = 0;

    (void)state;

    /* the longest route leaves room for a pad byte and the 6-byte DH+ suffix. */
    assert_int_equal(encode_n(248, 1, &size), PLCTAG_STATUS_OK);
    assert_int_equal(size, 248 + 4);

    /* without a connection the Message Router is added after identify; it must still fit. */
    assert_int_equal(encode_n(248, 0, &size), PLCTAG_STATUS_OK);
    assert_true(size + (int)sizeof(cip_router_path) <= 255);

    /* one byte longer would wrap an 8-bit size once a suffix is added. */
    assert_int_equal(encode_n(249, 1, &size), PLCTAG_ERR_TOO_LARGE);
    assert_int_equal(encode_n(255, 1, &size), PLCTAG_ERR_TOO_LARGE);
}


/* a DH+ route forces connected messaging, so a second tag with the PLC's unconnected default shares it. */
static void test_dhp_shares_connection(void **state) {
    cip_conn_p session = NULL;

    (void)state;

    session = (cip_conn_p)calloc(1, sizeof(*session));
    assert_non_null(session);
    mutex_init(&session->session_mutex);

    session->plc_type = AB_PLC_PLC5;
    session->use_connected_msg = 1;
    session->is_dhp = 1;
    assert_true(!session_connected_conflicts(session, 0));

    /* without DH+ the two really differ. */
    session->is_dhp = 0;
    assert_true(session_connected_conflicts(session, 0));

    mutex_destroy(&session->session_mutex);
    free(session);
}


/*
 * Step 1.8: the session list lock lives for the process, so a session destructor that runs after
 * plc_tag_shutdown(), from a reference held across it, still finds the lock.
 */
static void test_session_lock_outlives_shutdown(void **state) {
    int32_t id = plc_tag_create("protocol=system&name=version", 0);

    (void)state;

    assert_true(id > 0);
    plc_tag_destroy(id);
    plc_tag_shutdown();

    assert_true(session_mutex.initialized);
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_forward_close_header),
        cmocka_unit_test(test_path_fits_8_bits),
        cmocka_unit_test(test_ip_addr_segment),
        cmocka_unit_test(test_dhp_shares_connection),
        cmocka_unit_test(test_session_lock_outlives_shutdown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
