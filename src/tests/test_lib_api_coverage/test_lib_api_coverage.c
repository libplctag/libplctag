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
 * Exercises public API entry points that no other test in the suite reaches:
 * plc_tag_get/set_float32/float64/int64/uint64/int8, plc_tag_get/set_raw_bytes
 * (including its validation branches), plc_tag_lock/unlock,
 * plc_tag_unregister_callback, and the custom byte-order attribute parser
 * (check_byte_order_str, via int32_byte_order=).
 *
 * Each of these is real logic (bounds checks, byte-order-aware serialization,
 * a retry loop), not a bare wrapper -- see the coverage report analysis this
 * test was written to close.
 */

#include "compat_utils.h"
#include <libplctag/lib/libplctag.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 6, 0
#define DATA_TIMEOUT 5000

static int failures = 0;

#define CHECK(cond, msg)                                          \
    do {                                                          \
        if(!(cond)) {                                             \
            /* NOLINTNEXTLINE */                                  \
            fprintf(stderr, "FAIL: %s\n", msg);                   \
            failures++;                                           \
        } else {                                                  \
            /* NOLINTNEXTLINE */                                  \
            fprintf(stderr, "PASS: %s\n", msg);                   \
        }                                                         \
    } while(0)


typedef struct {
    const char *real_tag;
    const char *lreal_tag;
    const char *lint_tag;
    const char *sint_tag;
    const char *byteorder_tag;
} args_t;


static void parse_args(int argc, char **argv, args_t *args) {
    memset(args, 0, sizeof(*args));

    for(int i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--real-tag=", 11) == 0) {
            args->real_tag = &argv[i][11];
        } else if(strncmp(argv[i], "--lreal-tag=", 12) == 0) {
            args->lreal_tag = &argv[i][12];
        } else if(strncmp(argv[i], "--lint-tag=", 11) == 0) {
            args->lint_tag = &argv[i][11];
        } else if(strncmp(argv[i], "--sint-tag=", 11) == 0) {
            args->sint_tag = &argv[i][11];
        } else if(strncmp(argv[i], "--byteorder-tag=", 16) == 0) {
            args->byteorder_tag = &argv[i][16];
        }
    }

    if(!args->real_tag || !args->lreal_tag || !args->lint_tag || !args->sint_tag || !args->byteorder_tag) {
        // NOLINTNEXTLINE
        fprintf(stderr,
                "Usage: test_lib_api_coverage --real-tag=... --lreal-tag=... --lint-tag=... "
                "--sint-tag=... --byteorder-tag=...\n");
        exit(1);
    }
}


static int32_t create_tag(const char *attribs) {
    int32_t tag = plc_tag_create(attribs, DATA_TIMEOUT);
    if(tag < 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "FAIL: could not create tag '%s': %s\n", attribs, plc_tag_decode_error(tag));
        failures++;
    }
    return tag;
}


/* plc_tag_get/set_float32 -- byte-order-aware serialization, is_bit/no-data/
 * out-of-bounds branches. */
static void test_float32(const char *attribs) {
    int32_t tag = create_tag(attribs);
    if(tag < 0) { return; }

    CHECK(plc_tag_read(tag, DATA_TIMEOUT) == PLCTAG_STATUS_OK, "float32 initial read");

    plc_tag_set_float32(tag, 0, 3.5f);
    CHECK(plc_tag_status(tag) == PLCTAG_STATUS_OK, "float32 set_float32");

    CHECK(plc_tag_write(tag, DATA_TIMEOUT) == PLCTAG_STATUS_OK, "float32 write");
    CHECK(plc_tag_read(tag, DATA_TIMEOUT) == PLCTAG_STATUS_OK, "float32 read-back");

    float val = plc_tag_get_float32(tag, 0);
    CHECK(fabsf(val - 3.5f) < 0.0001f, "float32 round-trip value");

    plc_tag_destroy(tag);
}


/* plc_tag_get/set_float64 -- same pattern, 8-byte assembly. */
static void test_float64(const char *attribs) {
    int32_t tag = create_tag(attribs);
    if(tag < 0) { return; }

    CHECK(plc_tag_read(tag, DATA_TIMEOUT) == PLCTAG_STATUS_OK, "float64 initial read");

    plc_tag_set_float64(tag, 0, 123456.75);
    CHECK(plc_tag_status(tag) == PLCTAG_STATUS_OK, "float64 set_float64");

    CHECK(plc_tag_write(tag, DATA_TIMEOUT) == PLCTAG_STATUS_OK, "float64 write");
    CHECK(plc_tag_read(tag, DATA_TIMEOUT) == PLCTAG_STATUS_OK, "float64 read-back");

    double val = plc_tag_get_float64(tag, 0);
    CHECK(fabs(val - 123456.75) < 0.0001, "float64 round-trip value");

    plc_tag_destroy(tag);
}


/* plc_tag_get/set_int64 and _uint64 -- byte-order-array 8-byte assembly. */
static void test_int64(const char *attribs) {
    int32_t tag = create_tag(attribs);
    if(tag < 0) { return; }

    CHECK(plc_tag_read(tag, DATA_TIMEOUT) == PLCTAG_STATUS_OK, "int64 initial read");

    plc_tag_set_int64(tag, 0, -123456789012345LL);
    CHECK(plc_tag_status(tag) == PLCTAG_STATUS_OK, "int64 set_int64");
    CHECK(plc_tag_write(tag, DATA_TIMEOUT) == PLCTAG_STATUS_OK, "int64 write");
    CHECK(plc_tag_read(tag, DATA_TIMEOUT) == PLCTAG_STATUS_OK, "int64 read-back");
    CHECK(plc_tag_get_int64(tag, 0) == -123456789012345LL, "int64 round-trip value");

    plc_tag_set_uint64(tag, 0, 123456789012345ULL);
    CHECK(plc_tag_status(tag) == PLCTAG_STATUS_OK, "uint64 set_uint64");
    CHECK(plc_tag_write(tag, DATA_TIMEOUT) == PLCTAG_STATUS_OK, "uint64 write");
    CHECK(plc_tag_read(tag, DATA_TIMEOUT) == PLCTAG_STATUS_OK, "uint64 read-back");
    CHECK(plc_tag_get_uint64(tag, 0) == 123456789012345ULL, "uint64 round-trip value");

    plc_tag_destroy(tag);
}


/* plc_tag_get/set_int8 -- bounds check plus the is_bit delegation branch
 * (exercised separately by the existing PCCC bit tests; here we only cover
 * the plain byte-element path). */
static void test_int8(const char *attribs) {
    int32_t tag = create_tag(attribs);
    if(tag < 0) { return; }

    CHECK(plc_tag_read(tag, DATA_TIMEOUT) == PLCTAG_STATUS_OK, "int8 initial read");

    plc_tag_set_int8(tag, 0, -42);
    CHECK(plc_tag_status(tag) == PLCTAG_STATUS_OK, "int8 set_int8");
    CHECK(plc_tag_write(tag, DATA_TIMEOUT) == PLCTAG_STATUS_OK, "int8 write");
    CHECK(plc_tag_read(tag, DATA_TIMEOUT) == PLCTAG_STATUS_OK, "int8 read-back");
    CHECK(plc_tag_get_int8(tag, 0) == -42, "int8 round-trip value");

    plc_tag_destroy(tag);
}


/* plc_tag_get/set_raw_bytes -- the most defensively-coded getter/setter pair
 * in lib.c: null-buffer, non-positive-size, and out-of-bounds-offset are all
 * distinct returns, plus the normal success path. */
static void test_raw_bytes(const char *attribs) {
    int32_t tag = create_tag(attribs);
    if(tag < 0) { return; }

    CHECK(plc_tag_read(tag, DATA_TIMEOUT) == PLCTAG_STATUS_OK, "raw_bytes initial read");

    uint8_t write_buf[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    int rc = plc_tag_set_raw_bytes(tag, 0, write_buf, (int)sizeof(write_buf));
    CHECK(rc == PLCTAG_STATUS_OK, "raw_bytes set_raw_bytes success");

    rc = plc_tag_set_raw_bytes(tag, 0, NULL, (int)sizeof(write_buf));
    CHECK(rc == PLCTAG_ERR_NULL_PTR, "raw_bytes set_raw_bytes null buffer");

    rc = plc_tag_set_raw_bytes(tag, 0, write_buf, 0);
    CHECK(rc == PLCTAG_ERR_BAD_PARAM, "raw_bytes set_raw_bytes zero size");

    rc = plc_tag_set_raw_bytes(tag, 100000, write_buf, (int)sizeof(write_buf));
    CHECK(rc != PLCTAG_STATUS_OK, "raw_bytes set_raw_bytes out-of-bounds offset");

    CHECK(plc_tag_write(tag, DATA_TIMEOUT) == PLCTAG_STATUS_OK, "raw_bytes write");
    CHECK(plc_tag_read(tag, DATA_TIMEOUT) == PLCTAG_STATUS_OK, "raw_bytes read-back");

    uint8_t read_buf[4] = {0};
    rc = plc_tag_get_raw_bytes(tag, 0, read_buf, (int)sizeof(read_buf));
    CHECK(rc == PLCTAG_STATUS_OK, "raw_bytes get_raw_bytes success");
    CHECK(memcmp(read_buf, write_buf, sizeof(write_buf)) == 0, "raw_bytes round-trip value");

    rc = plc_tag_get_raw_bytes(tag, 0, NULL, (int)sizeof(read_buf));
    CHECK(rc == PLCTAG_ERR_NULL_PTR, "raw_bytes get_raw_bytes null buffer");

    rc = plc_tag_get_raw_bytes(tag, 0, read_buf, 0);
    CHECK(rc == PLCTAG_ERR_BAD_PARAM, "raw_bytes get_raw_bytes zero size");

    rc = plc_tag_get_raw_bytes(tag, 100000, read_buf, (int)sizeof(read_buf));
    CHECK(rc != PLCTAG_STATUS_OK, "raw_bytes get_raw_bytes out-of-bounds offset");

    /* plc_tag_lock/unlock -- real retry-loop synchronization logic, not a
     * bare wrapper. Any open tag will do. */
    CHECK(plc_tag_lock(tag) == PLCTAG_STATUS_OK, "plc_tag_lock");
    CHECK(plc_tag_unlock(tag) == PLCTAG_STATUS_OK, "plc_tag_unlock");

    plc_tag_destroy(tag);
}


static void dummy_callback(int32_t tag_id, int event, int status) {
    (void)tag_id;
    (void)event;
    (void)status;
}


/* plc_tag_unregister_callback's two branches: callback registered (removes
 * it, returns OK) and not registered (returns PLCTAG_ERR_NOT_FOUND). */
static void test_unregister_callback(const char *attribs) {
    int32_t tag = create_tag(attribs);
    if(tag < 0) { return; }

    CHECK(plc_tag_read(tag, DATA_TIMEOUT) == PLCTAG_STATUS_OK, "unregister_callback initial read");

    int rc = plc_tag_unregister_callback(tag);
    CHECK(rc == PLCTAG_ERR_NOT_FOUND, "unregister_callback with none registered");

    rc = plc_tag_register_callback(tag, dummy_callback);
    CHECK(rc == PLCTAG_STATUS_OK, "register_callback");

    rc = plc_tag_unregister_callback(tag);
    CHECK(rc == PLCTAG_STATUS_OK, "unregister_callback with one registered");

    plc_tag_destroy(tag);
}


/* check_byte_order_str, via int32_byte_order=<permutation>: exercises the
 * validation of a custom byte-order attribute string that no other test
 * sets. */
static void test_byte_order(const char *attribs) {
    /* Reversed from the default order -- a valid distinct permutation. */
    char full_attribs[1024] = {0};
    snprintf(full_attribs, sizeof(full_attribs), "%s&int32_byte_order=3210", attribs);

    int32_t tag = create_tag(full_attribs);
    if(tag < 0) { return; }

    CHECK(plc_tag_read(tag, DATA_TIMEOUT) == PLCTAG_STATUS_OK, "byte_order initial read");

    plc_tag_set_int32(tag, 0, 424242);
    CHECK(plc_tag_write(tag, DATA_TIMEOUT) == PLCTAG_STATUS_OK, "byte_order write");
    CHECK(plc_tag_read(tag, DATA_TIMEOUT) == PLCTAG_STATUS_OK, "byte_order read-back");
    CHECK(plc_tag_get_int32(tag, 0) == 424242, "byte_order round-trip value");

    plc_tag_destroy(tag);
}


int main(int argc, char **argv) {
    args_t args;
    int version_major = plc_tag_get_int_attribute(0, "version_major", 0);
    int version_minor = plc_tag_get_int_attribute(0, "version_minor", 0);
    int version_patch = plc_tag_get_int_attribute(0, "version_patch", 0);

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Required compatible library version %d.%d.%d not available, found %d.%d.%d!\n",
                REQUIRED_VERSION, version_major, version_minor, version_patch);
        return 1;
    }

    parse_args(argc, argv, &args);

    plc_tag_set_debug_level(PLCTAG_DEBUG_WARN);

    /* Also exercise the two trivial one-line wrappers while we're at it. */
    (void)plc_tag_get_debug_level();
    (void)plc_tag_get_debug_module_level(PLCTAG_MODULE_LIB);

    test_float32(args.real_tag);
    test_float64(args.lreal_tag);
    test_int64(args.lint_tag);
    test_int8(args.sint_tag);
    test_raw_bytes(args.byteorder_tag);
    test_unregister_callback(args.byteorder_tag);
    test_byte_order(args.byteorder_tag);

    if(failures > 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "FAILURE: %d check(s) failed.\n", failures);
        return 1;
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "SUCCESS: all checks passed.\n");
    return 0;
}
