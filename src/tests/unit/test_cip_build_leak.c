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
 * Known issue 12: a request builder that fails after it got its request must free it.  Each
 * builder error path runs once here; the request's destructor counts the frees.  The builders'
 * file is compiled in with the three session calls they make replaced, so no session is needed.
 * eip_cip.c and eip_cip_special.c cannot share a translation unit, so this file builds twice:
 * test_cip_build_leak for eip_cip.c, and test_cip_special_build_leak, with SPECIAL_BUILDERS
 * defined, for eip_cip_special.c.
 */

#include "mini_mock.h"

#include <libplctag/protocols/ab/session.h>
#include <stdlib.h>

#define REQUEST_CAPACITY (600)

static int32_t requests_freed = 0;
static int32_t available_payload = REQUEST_CAPACITY;
static int32_t add_request_rc = PLCTAG_STATUS_OK;

static void test_request_destroy(void *arg) {
    (void)arg;
    requests_freed++;
}

static int test_reuse_request(cip_conn_p session, ab_request_p spare, int tag_id, ab_request_p *req) {
    (void)session;
    (void)spare;

    *req = (ab_request_p)rc_alloc((int)(sizeof(struct ab_request_t) + REQUEST_CAPACITY), test_request_destroy);
    if(!*req) { return PLCTAG_ERR_NO_MEM; }

    (*req)->tag_id = tag_id;
    (*req)->data = (uint8_t *)(*req + 1);
    (*req)->request_capacity = REQUEST_CAPACITY;

    return PLCTAG_STATUS_OK;
}

static int test_add_request(cip_conn_p session, ab_request_p req) {
    (void)session;
    (void)req;
    return add_request_rc;
}

static int test_available_payload(cip_conn_p session) {
    (void)session;
    return available_payload;
}

#define session_reuse_request(session, spare, tag_id, req) test_reuse_request(session, spare, tag_id, req)
#define session_add_request(session, req) test_add_request(session, req)
#define session_get_available_cip_payload_space(session) test_available_payload(session)

#if defined(SPECIAL_BUILDERS)
#    include "../../libplctag/protocols/ab/eip_cip_special.c"
#else
#    include "../../libplctag/protocols/ab/eip_cip.c"
#endif


static tag_byte_order_t byte_order; /* no string padding */
static uint8_t tag_data[400];


/* a one-DINT Logix tag named "a" with its type known, and no session. */
static ab_tag_p make_tag(void) {
    ab_tag_p tag = (ab_tag_p)calloc(1, sizeof(struct ab_tag_t));

    assert_non_null(tag);
    mutex_init(&tag->api_mutex);

    tag->tag_id = 1;
    tag->plc_type = AB_PLC_LGX;
    tag->encoded_name[0] = 0x91; /* ANSI symbolic segment */
    tag->encoded_name[1] = 1;
    tag->encoded_name[2] = 'a';
    tag->encoded_name[3] = 0;
    tag->encoded_name_size = 4;
    tag->encoded_type_info[0] = 0xC4; /* DINT */
    tag->encoded_type_info[1] = 0;
    tag->encoded_type_info_size = 2;
    tag->elem_count = 1;
    tag->elem_size = 4;
    tag->size = 4;
    tag->data = tag_data;
    tag->byte_order = &byte_order;

    return tag;
}


/* runs one builder on a fresh tag and checks it failed with want_rc and freed its request. */
static void check_freed(const char *what, int (*build)(ab_tag_p tag), void (*setup)(ab_tag_p tag), int want_rc) {
    ab_tag_p tag = make_tag();
    int32_t freed_before = requests_freed;
    int rc = 0;

    printf("%s\n", what);

    if(setup) { setup(tag); }

    rc = build(tag);

    assert_int_equal(rc, want_rc);
    assert_null(tag->req);
    assert_int_equal(requests_freed, freed_before + 1);

    available_payload = REQUEST_CAPACITY;
    add_request_rc = PLCTAG_STATUS_OK;
    mutex_destroy(&tag->api_mutex);
    free(tag);
}


static void tiny_payload(ab_tag_p tag) { (void)tag; available_payload = 4; }
static void add_fails(ab_tag_p tag) { (void)tag; add_request_rc = PLCTAG_ERR_BAD_STATUS; }


#if defined(SPECIAL_BUILDERS)

static int build_listing(ab_tag_p tag) { return listing_tag_build_read_request(tag); }
static int build_udt(ab_tag_p tag) { return udt_tag_build_read_metadata_request(tag); }

static void test_listing_too_large(void **state) {
    (void)state;
    check_freed("tag listing: the request does not fit the payload", build_listing, tiny_payload, PLCTAG_ERR_TOO_LARGE);
}

static void test_listing_add_fails(void **state) {
    (void)state;
    check_freed("tag listing: the session refuses the request", build_listing, add_fails, PLCTAG_ERR_BAD_STATUS);
}

static void test_udt_too_large(void **state) {
    (void)state;
    check_freed("UDT metadata: the request does not fit the payload", build_udt, tiny_payload, PLCTAG_ERR_TOO_LARGE);
}

static void test_udt_add_fails(void **state) {
    (void)state;
    check_freed("UDT metadata: the session refuses the request", build_udt, add_fails, PLCTAG_ERR_BAD_STATUS);
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_listing_too_large), cmocka_unit_test(test_listing_add_fails),
        cmocka_unit_test(test_udt_too_large),     cmocka_unit_test(test_udt_add_fails),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}

#else

static int build_read(ab_tag_p tag) { return build_read_request(tag, 0); }
static int build_write(ab_tag_p tag) { return build_write_request(tag, 0); }
static void no_type(ab_tag_p tag) { tag->encoded_type_info_size = 0; }
static void bit_tiny_payload(ab_tag_p tag) { tag->is_bit = 1; tiny_payload(tag); }
static void bit_add_fails(ab_tag_p tag) { tag->is_bit = 1; add_fails(tag); }
static void big_bit_tag(ab_tag_p tag) { tag->is_bit = 1; tag->size = 400; tag->elem_size = 400; }


static void test_read_too_large(void **state) {
    (void)state;
    check_freed("read: the request does not fit the payload", build_read, tiny_payload, PLCTAG_ERR_TOO_LARGE);
}

static void test_read_add_fails(void **state) {
    (void)state;
    check_freed("read: the session refuses the request", build_read, add_fails, PLCTAG_ERR_BAD_STATUS);
}

static void test_write_too_large(void **state) {
    (void)state;
    check_freed("write: no room for any data", build_write, tiny_payload, PLCTAG_ERR_TOO_LARGE);
}

static void test_write_no_type(void **state) {
    (void)state;
    check_freed("write: the type is not known", build_write, no_type, PLCTAG_ERR_UNSUPPORTED);
}

static void test_write_add_fails(void **state) {
    (void)state;
    check_freed("write: the session refuses the request", build_write, add_fails, PLCTAG_ERR_BAD_STATUS);
}

static void test_bit_write_too_large(void **state) {
    (void)state;
    check_freed("bit write: no room for any data", build_write, bit_tiny_payload, PLCTAG_ERR_TOO_LARGE);
}

static void test_bit_write_too_small(void **state) {
    (void)state;
    check_freed("bit write: no room for the masks", build_write, big_bit_tag, PLCTAG_ERR_TOO_SMALL);
}

static void test_bit_write_add_fails(void **state) {
    (void)state;
    check_freed("bit write: the session refuses the request", build_write, bit_add_fails, PLCTAG_ERR_BAD_STATUS);
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_read_too_large),      cmocka_unit_test(test_read_add_fails),
        cmocka_unit_test(test_write_too_large),     cmocka_unit_test(test_write_no_type),
        cmocka_unit_test(test_write_add_fails),     cmocka_unit_test(test_bit_write_too_large),
        cmocka_unit_test(test_bit_write_too_small), cmocka_unit_test(test_bit_write_add_fails),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}

#endif
