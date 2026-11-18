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
 * Modbus create and destroy safety (review M1, M2, plan step 1.8).  modbus.c is compiled in so the test can
 * fail vector_create() on demand and call the static destructors.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <utils/vector.h>

static bool fail_vector_create = false;

static vector_p test_vector_create(int capacity, int max_inc) {
    return fail_vector_create ? NULL : vector_create(capacity, max_inc);
}

#define vector_create(capacity, max_inc) test_vector_create(capacity, max_inc)

#include "../../libplctag/protocols/mb/modbus.c"

#define check(cond, what)                                                              \
    do {                                                                               \
        if(!(cond)) {                                                                  \
            fprintf(stderr, "FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, what, #cond); \
            failures++;                                                                \
        }                                                                              \
    } while(0)

static int32_t failures = 0;


/* M1: a failed allocation fails the create; a later step must not overwrite the error. */
static void test_create_fails_on_no_mem(void) {
    attr attribs = attr_create_from_str("protocol=modbus-tcp&gateway=127.0.0.1:1&path=1&name=hr0");
    modbus_plc_p plc = NULL;
    int32_t rc = 0;

    printf("M1: a failed tag vector create fails the PLC create.\n");

    fail_vector_create = true;
    rc = find_or_create_plc(attribs, &plc, NULL);
    fail_vector_create = false;

    check(rc == PLCTAG_ERR_NO_MEM, "M1: create returns PLCTAG_ERR_NO_MEM");
    check(plc == NULL, "M1: no PLC returned");

    if(plc) { rc_dec(plc); }
    attr_destroy(attribs);
}


/* M2: a NULL pointer is reported, not dereferenced, with logging on. */
static void test_destructors_null(void) {
    printf("M2: the Modbus destructors accept NULL with logging on.\n");

    modbus_tag_destructor(NULL);
    modbus_plc_destructor(NULL);
}


int main(void) {
    int32_t id = 0;

    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    /* any create starts the library, and with it this module. */
    id = plc_tag_create("protocol=system&name=version", 0);
    check(id > 0, "library start");

    test_create_fails_on_no_mem();
    test_destructors_null();

    plc_tag_destroy(id);
    plc_tag_shutdown();

    /* step 1.8: a PLC destructor that runs after shutdown, from a held reference, still finds it. */
    printf("1.8: the Modbus list lock outlives plc_tag_shutdown().\n");
    check(mb_mutex.initialized, "1.8: mb_mutex still initialized after shutdown");

    printf("%s: %" PRId32 " failure(s).\n", failures ? "FAILED" : "PASSED", failures);

    return failures ? 1 : 0;
}
