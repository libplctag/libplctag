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
 * Unit test for review defect R4: the UDT member description size comes from the PLC in
 * 32-bit words, and 4 * words - 23 underflowed for words < 6, asking for about 65,000 bytes.
 *
 * udt_fields_size() is static, so the file is compiled in here.  Its other symbols then come
 * from this object, and the archive's copy of udt_tag.o is never pulled in.
 */

#include "mini_mock.h"

#include "../../libplctag/modules/ab/udt_tag.c"


static void test_too_few_words_is_refused(void **state) {
    (void)state;

    for(uint32_t words = 0; words < 6; words++) { assert_int_equal(udt_fields_size(words), 0); }
}


static void test_size_rounds_up_to_a_word(void **state) {
    (void)state;

    assert_int_equal(udt_fields_size(6), 4);    /* 1 byte */
    assert_int_equal(udt_fields_size(7), 8);    /* 5 bytes */
    assert_int_equal(udt_fields_size(100), 380); /* 377 bytes */
}


static void test_size_past_16_bits_is_refused(void **state) {
    (void)state;

    assert_int_equal(udt_fields_size(16388), 65532);
    assert_int_equal(udt_fields_size(16389), 0);
    assert_int_equal(udt_fields_size(UINT32_MAX), 0);
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_too_few_words_is_refused),
        cmocka_unit_test(test_size_rounds_up_to_a_word),
        cmocka_unit_test(test_size_past_16_bits_is_refused),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
