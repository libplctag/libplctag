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
 * PCCC logical address parsing.  Named fields (T4:0.ACC, C5:0.DN, R6:0.LEN) failed with
 * PLCTAG_ERR_BAD_PARAM from 2.6.3 on because the '.' was consumed twice (#688).
 */

#include "mini_mock.h"

#include <libplctag/protocols/ab/tag.h>
#include <libplctag/protocols/ab/pccc.h>


static void check(const char *name, int32_t want_rc, int32_t sub_element, uint8_t is_bit, uint8_t bit) {
    pccc_addr_t addr = {0};

    printf("  %s\n", name);
    assert_int_equal(parse_pccc_logical_address(name, &addr), want_rc);

    if(want_rc != PLCTAG_STATUS_OK) { return; }

    assert_int_equal(addr.sub_element, sub_element);
    assert_int_equal(addr.is_bit, is_bit);
    if(is_bit) { assert_int_equal(addr.bit, bit); }
}


static void test_named_fields(void **state) {
    (void)state;

    check("T4:0.ACC", PLCTAG_STATUS_OK, 2, 0, 0);
    check("T4:0.acc", PLCTAG_STATUS_OK, 2, 0, 0);
    check("T4:0.PRE", PLCTAG_STATUS_OK, 1, 0, 0);
    check("T4:0.DN", PLCTAG_STATUS_OK, 0, 1, 13);
    check("C5:0.ACC", PLCTAG_STATUS_OK, 2, 0, 0);
    check("R6:0.LEN", PLCTAG_STATUS_OK, 1, 0, 0);
}


static void test_other_forms_unchanged(void **state) {
    (void)state;

    check("T4:0.2", PLCTAG_STATUS_OK, 2, 0, 0);
    check("T4:0", PLCTAG_STATUS_OK, -1, 0, 0);
    check("N7:0/3", PLCTAG_STATUS_OK, -1, 1, 3);
    check("T4:0.BOGUS", PLCTAG_ERR_BAD_PARAM, 0, 0, 0);
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_named_fields),
        cmocka_unit_test(test_other_forms_unchanged),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
