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
 * classify_fo_reply() decides every Forward Open retry, so each row of its table gets a reply:
 * general status, the count of status words, then the little-endian words.
 */

#include "mini_mock.h"

#include <protocols/ab/error_codes.h>


static fo_reply_t classify(const uint8_t *status, size_t size, uint16_t *offered) {
    return classify_fo_reply(status, size, offered);
}


static void test_every_row(void **state) {
    uint16_t offered = 0;

    static const uint8_t ok[] = {0x00, 0x00};
    static const uint8_t dup[] = {0x01, 0x01, 0x00, 0x01};
    static const uint8_t size_offer[] = {0x01, 0x02, 0x09, 0x01, 0xA2, 0x0F};
    static const uint8_t size_bare[] = {0x01, 0x01, 0x09, 0x01};
    static const uint8_t no_conn[] = {0x01, 0x01, 0x13, 0x01};
    static const uint8_t offline[] = {0x01, 0x01, 0x00, 0x08};
    static const uint8_t port[] = {0x01, 0x01, 0x11, 0x03};
    static const uint8_t link[] = {0x01, 0x01, 0x12, 0x03};
    static const uint8_t segment[] = {0x01, 0x01, 0x15, 0x03};
    static const uint8_t path_seg[] = {0x04, 0x00};
    static const uint8_t dest[] = {0x05, 0x00};
    static const uint8_t unsupported[] = {0x08, 0x00};
    static const uint8_t rpi[] = {0x01, 0x01, 0x12, 0x01};
    static const uint8_t other[] = {0x02, 0x00};

    (void)state;

    assert_int_equal(classify(ok, sizeof(ok), &offered), FO_ACCEPTED);
    assert_int_equal(classify(dup, sizeof(dup), &offered), FO_DUP_ID);
    assert_int_equal(classify(size_offer, sizeof(size_offer), &offered), FO_SIZE_REFUSED);
    assert_int_equal(offered, 4002);
    assert_int_equal(classify(size_bare, sizeof(size_bare), &offered), FO_SIZE_REFUSED);
    assert_int_equal(offered, 0);
    assert_int_equal(classify(no_conn, sizeof(no_conn), &offered), FO_NO_CONNECTIONS);
    assert_int_equal(classify(offline, sizeof(offline), &offered), FO_LINK_OFFLINE);
    assert_int_equal(classify(port, sizeof(port), &offered), FO_ROUTE_REFUSED);
    assert_int_equal(classify(link, sizeof(link), &offered), FO_ROUTE_REFUSED);
    assert_int_equal(classify(segment, sizeof(segment), &offered), FO_ROUTE_REFUSED);
    assert_int_equal(classify(path_seg, sizeof(path_seg), &offered), FO_ROUTE_REFUSED);
    assert_int_equal(classify(dest, sizeof(dest), &offered), FO_ROUTE_REFUSED);
    assert_int_equal(classify(unsupported, sizeof(unsupported), &offered), FO_UNSUPPORTED);
    assert_int_equal(classify(rpi, sizeof(rpi), &offered), FO_OTHER);
    assert_int_equal(classify(other, sizeof(other), &offered), FO_OTHER);
}


/* the PLC controls the word count, the buffer bounds what is read. */
static void test_truncated_status(void **state) {
    uint16_t offered = 1;
    static const uint8_t size_cut[] = {0x01, 0x02, 0x09, 0x01, 0xA2};

    (void)state;

    assert_int_equal(classify(size_cut, sizeof(size_cut), &offered), FO_SIZE_REFUSED);
    assert_int_equal(offered, 0);
    assert_int_equal(classify(size_cut, 2, &offered), FO_OTHER);
    assert_int_equal(classify(size_cut, 0, &offered), FO_OTHER);
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_every_row),
        cmocka_unit_test(test_truncated_status),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
