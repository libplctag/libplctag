/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever    *
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
 * Comprehensive tests for plc_tag_create_from_tag() covering all input
 * permutations.
 *
 * Tests 1-4 (invalid source IDs) require no PLC and always run.
 * Tests 5-12 require a live PLC or simulator; they run only when
 * --src-tag, --clone-attrib, and --device-tag are all provided.
 *
 *  No-PLC tests:
 *   1. src_tag_id = INT32_MAX (nonexistent positive)   -> ERR_NOT_FOUND
 *   2. src_tag_id = 0                                   -> ERR_BAD_PARAM
 *   3. src_tag_id = -1 (negative / error code as ID)   -> ERR_BAD_PARAM
 *   4. src_tag_id = INT32_MIN                           -> ERR_BAD_PARAM
 *
 *  With-PLC tests:
 *   5. valid src, attrib_str = NULL                     -> ERR_BAD_PARAM or ERR_NULL_PTR
 *   6. valid src, attrib_str = ""                       -> any error (not success)
 *   7. @connection src + data clone attribs                 -> success
 *   8. @connection src + @connection clone attribs              -> success
 *   9. regular src + @connection clone attribs              -> success
 *  10. regular src + valid data clone attribs           -> success; read works after src destroyed
 *  11. regular src, create two clones from same src     -> both succeed independently
 *  12. previously-destroyed src ID reused               -> ERR_NOT_FOUND
 *
 * Usage:
 *   test_create_from_tag [--src-tag=ATTRIBS] [--clone-attrib=ATTRIBS]
 *                        [--device-tag=ATTRIBS] [--timeout=ms] [--debug=N]
 *
 * Example (AB/EIP with simulator):
 *   test_create_from_tag \
 *     "--src-tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray[0]" \
 *     "--clone-attrib=name=TestBigArray[1]&elem_count=1" \
 *     "--device-tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=@connection" \
 *     --timeout=5000
 */

#include "compat_utils.h"
#include <libplctag/lib/libplctag.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 7, 0
#define DEFAULT_TIMEOUT_MS 5000
#define POLL_SLEEP_MS 10

static const char *src_tag_attribs = NULL;
static const char *clone_tag_attribs = NULL;
static const char *connection_tag_attribs = NULL;
static int timeout_ms = DEFAULT_TIMEOUT_MS;

/* Shared flags for test 16 (callback fires on clone). */
static compat_atomic_int32_t clone_created_cb_fired = {0};
static compat_atomic_int32_t clone_read_completed_cb_fired = {0};

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static int wait_for_tag_ready(int32_t tag, int timeout) {
    int64_t end_time = compat_time_ms() + (int64_t)timeout;
    int rc = PLCTAG_STATUS_PENDING;

    do {
        rc = plc_tag_status(tag);
        if(rc == PLCTAG_STATUS_OK) { return rc; }
        if(rc != PLCTAG_STATUS_PENDING) { return rc; }
        compat_sleep_ms(POLL_SLEEP_MS, NULL);
    } while(compat_time_ms() < end_time);

    return PLCTAG_ERR_TIMEOUT;
}

/** Return true if rc indicates invalid source tag ID input. */
static bool is_bad_src_id_error(int32_t rc) { return rc == PLCTAG_ERR_BAD_PARAM; }

/** Return true if rc indicates a NULL/bad attrib string. */
static bool is_bad_attrib_error(int32_t rc) { return rc == PLCTAG_ERR_BAD_PARAM || rc == PLCTAG_ERR_NULL_PTR; }

/** Create a data source tag and wait for it to be ready. Returns tag id or negative error. */
static int32_t create_ready_src_tag(void) {
    int32_t tag = plc_tag_create(src_tag_attribs, timeout_ms);
    if(tag < 0) {
        fprintf(stderr, "  ERROR: could not create source tag: %s\n", plc_tag_decode_error((int)tag));
        return tag;
    }

    int rc = wait_for_tag_ready(tag, timeout_ms);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "  ERROR: source tag not ready: %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return (int32_t)rc;
    }

    return tag;
}

/** Create an @connection tag and wait for it to be connected. Returns tag id or negative error. */
static int32_t create_ready_connection_tag(void) {
    int32_t tag = plc_tag_create(connection_tag_attribs, timeout_ms);
    if(tag < 0) {
        fprintf(stderr, "  ERROR: could not create @connection tag: %s\n", plc_tag_decode_error((int)tag));
        return tag;
    }

    int rc = wait_for_tag_ready(tag, timeout_ms);
    /* ERR_TIMEOUT just means the UP event did not fire within the window —
     * the tag was still created. Any other error is a hard failure. */
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_TIMEOUT) {
        fprintf(stderr, "  ERROR: @connection tag hard failure: %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return (int32_t)rc;
    }

    return tag;
}

/* -------------------------------------------------------------------------
 * No-PLC tests (tests 1-4)
 * ---------------------------------------------------------------------- */

/* Test 1: src_tag_id = INT32_MAX — a large, never-created positive ID. */
static int test_invalid_src_id_max(void) {
    int32_t rc = plc_tag_create_from_tag((int32_t)INT32_MAX, "name=Whatever", NULL, NULL, 0);

    fprintf(stderr, "  test_invalid_src_id_max: rc=%s (%d)\n", plc_tag_decode_error((int)rc), (int)rc);

    if(rc != PLCTAG_ERR_NOT_FOUND) {
        fprintf(stderr, "  FAIL: expected ERR_NOT_FOUND, got %s\n", plc_tag_decode_error((int)rc));
        return PLCTAG_ERR_BAD_STATUS;
    }

    return PLCTAG_STATUS_OK;
}

/* Test 2: src_tag_id = 0 — zero is never a valid tag handle. */
static int test_invalid_src_id_zero(void) {
    int32_t rc = plc_tag_create_from_tag(0, "name=Whatever", NULL, NULL, 0);

    fprintf(stderr, "  test_invalid_src_id_zero: rc=%s (%d)\n", plc_tag_decode_error((int)rc), (int)rc);

    if(!is_bad_src_id_error(rc)) {
        fprintf(stderr, "  FAIL: expected ERR_BAD_PARAM, got %s\n", plc_tag_decode_error((int)rc));
        return PLCTAG_ERR_BAD_STATUS;
    }

    return PLCTAG_STATUS_OK;
}

/* Test 3: src_tag_id = -1 — negative values are error codes, not handles. */
static int test_invalid_src_id_minus_one(void) {
    int32_t rc = plc_tag_create_from_tag(-1, "name=Whatever", NULL, NULL, 0);

    fprintf(stderr, "  test_invalid_src_id_minus_one: rc=%s (%d)\n", plc_tag_decode_error((int)rc), (int)rc);

    if(!is_bad_src_id_error(rc)) {
        fprintf(stderr, "  FAIL: expected ERR_BAD_PARAM, got %s\n", plc_tag_decode_error((int)rc));
        return PLCTAG_ERR_BAD_STATUS;
    }

    return PLCTAG_STATUS_OK;
}

/* Test 4: src_tag_id = INT32_MIN — extreme negative. */
static int test_invalid_src_id_min(void) {
    int32_t rc = plc_tag_create_from_tag((int32_t)INT32_MIN, "name=Whatever", NULL, NULL, 0);

    fprintf(stderr, "  test_invalid_src_id_min: rc=%s (%d)\n", plc_tag_decode_error((int)rc), (int)rc);

    if(!is_bad_src_id_error(rc)) {
        fprintf(stderr, "  FAIL: expected ERR_BAD_PARAM, got %s\n", plc_tag_decode_error((int)rc));
        return PLCTAG_ERR_BAD_STATUS;
    }

    return PLCTAG_STATUS_OK;
}

/* -------------------------------------------------------------------------
 * With-PLC tests (tests 5-12)
 * ---------------------------------------------------------------------- */

/* Test 5: valid source tag, attrib_str = NULL. */
static int test_null_attrib_str(void) {
    int32_t src = create_ready_src_tag();
    if(src < 0) { return (int)src; }

    int32_t rc = plc_tag_create_from_tag(src, NULL, NULL, NULL, 0);

    fprintf(stderr, "  test_null_attrib_str: rc=%s (%d)\n", plc_tag_decode_error((int)rc), (int)rc);

    plc_tag_destroy(src);

    if(!is_bad_attrib_error(rc)) {
        fprintf(stderr, "  FAIL: expected ERR_BAD_PARAM or ERR_NULL_PTR, got %s\n", plc_tag_decode_error((int)rc));
        if(rc > 0) { plc_tag_destroy(rc); }
        return PLCTAG_ERR_BAD_STATUS;
    }

    return PLCTAG_STATUS_OK;
}

/* Test 6: valid source tag, attrib_str = "" (empty string). */
static int test_empty_attrib_str(void) {
    int32_t src = create_ready_src_tag();
    if(src < 0) { return (int)src; }

    int32_t rc = plc_tag_create_from_tag(src, "", NULL, NULL, 0);

    fprintf(stderr, "  test_empty_attrib_str: rc=%s (%d)\n", plc_tag_decode_error((int)rc), (int)rc);

    plc_tag_destroy(src);

    if(rc > 0) {
        plc_tag_destroy(rc);
        fprintf(stderr, "  FAIL: expected an error for empty attrib string but got a valid tag handle.\n");
        return PLCTAG_ERR_BAD_STATUS;
    }

    return PLCTAG_STATUS_OK;
}

/* Test 7: @connection tag as source, data clone attribs -> success. */
static int test_device_src_data_dst(void) {
    if(connection_tag_attribs == NULL) {
        fprintf(stderr, "  SKIP: no --device-tag provided, skipping @connection source test.\n");
        return PLCTAG_STATUS_OK;
    }
    int32_t connection_tag = create_ready_connection_tag();
    if(connection_tag < 0) { return (int)connection_tag; }

    int32_t clone = plc_tag_create_from_tag(connection_tag, clone_tag_attribs, NULL, NULL, timeout_ms);

    fprintf(stderr, "  test_device_src_data_dst: rc=%s (%d)\n", plc_tag_decode_error((int)clone), (int)clone);

    if(clone < 0) {
        fprintf(stderr, "  FAIL: expected success cloning from @device source, got %s\n", plc_tag_decode_error((int)clone));
        plc_tag_destroy(device_tag);
        return (int)clone;
    }

    int rc = wait_for_tag_ready(clone, timeout_ms);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "  FAIL: clone from @device source not ready: %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(clone);
        plc_tag_destroy(device_tag);
        return rc;
    }

    if(clone < 0) {
        fprintf(stderr, "  FAIL: expected success cloning from @connection source, got %s\n", plc_tag_decode_error((int)clone));
        plc_tag_destroy(connection_tag);
        return (int)clone;
    }

    int rc = wait_for_tag_ready(clone, timeout_ms);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "  FAIL: clone from @connection source not ready: %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(clone);
        plc_tag_destroy(connection_tag);
        return rc;
    }

    plc_tag_destroy(connection_tag);

    rc = plc_tag_read(clone, timeout_ms);
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        fprintf(stderr, "  FAIL: clone read failed after @connection source destroyed: %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(clone);
        return rc;
    }

    if(rc == PLCTAG_STATUS_PENDING) {
        rc = wait_for_tag_ready(clone, timeout_ms);
        if(rc != PLCTAG_STATUS_OK) {
            fprintf(stderr, "  FAIL: clone from @connection source did not recover from pending read: %s\n",
                    plc_tag_decode_error(rc));
            plc_tag_destroy(clone);
            return rc;
        }
    }

    plc_tag_destroy(clone);

    return PLCTAG_STATUS_OK;
}

/* Test 8: @connection tag as source, @connection clone attribs -> success. */
static int test_device_src_device_dst(void) {
    if(connection_tag_attribs == NULL) {
        fprintf(stderr, "  SKIP: no --device-tag provided, skipping @connection source test.\n");
        return PLCTAG_STATUS_OK;
    }
    int32_t connection_tag = create_ready_connection_tag();
    if(connection_tag < 0) { return (int)connection_tag; }

    int32_t clone = plc_tag_create_from_tag(connection_tag, "name=@connection", NULL, NULL, timeout_ms);

    fprintf(stderr, "  test_device_src_device_dst: rc=%s (%d)\n", plc_tag_decode_error((int)clone), (int)clone);

    if(clone < 0) {
        fprintf(stderr, "  FAIL: expected success cloning @device from @device source, got %s\n",
                plc_tag_decode_error((int)clone));
        plc_tag_destroy(device_tag);
        return (int)clone;
    }

    if(clone < 0) {
        fprintf(stderr, "  FAIL: expected success cloning @connection from @connection source, got %s\n",
                plc_tag_decode_error((int)clone));
        plc_tag_destroy(connection_tag);
        return (int)clone;
    }

    plc_tag_destroy(connection_tag);

    /* Device tags may remain pending for a while; treat timeout as acceptable. */
    int rc = wait_for_tag_ready(clone, timeout_ms);
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_TIMEOUT) {
        fprintf(stderr, "  FAIL: cloned @connection tag hard failure: %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(clone);
        return rc;
    }

    plc_tag_destroy(clone);

    return PLCTAG_STATUS_OK;
}

/* Test 9: regular tag as source, @connection as the target type -> success. */
static int test_data_src_device_dst(void) {
    int32_t src = create_ready_src_tag();
    if(src < 0) { return (int)src; }

    int32_t clone = plc_tag_create_from_tag(src, "name=@connection", NULL, NULL, timeout_ms);

    fprintf(stderr, "  test_data_src_device_dst: rc=%s (%d)\n", plc_tag_decode_error((int)clone), (int)clone);

    if(clone < 0) {
        fprintf(stderr, "  FAIL: expected success cloning @connection from data source, got %s\n",
                plc_tag_decode_error((int)clone));
        plc_tag_destroy(src);
        return (int)clone;
    }

    plc_tag_destroy(src);

    /* Device tags may remain pending for a while; treat timeout as acceptable. */
    int rc = wait_for_tag_ready(clone, timeout_ms);
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_TIMEOUT) {
        fprintf(stderr, "  FAIL: cloned @connection tag hard failure after source destroy: %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(clone);
        return rc;
    }

    plc_tag_destroy(clone);

    return PLCTAG_STATUS_OK;
}

/* Test 10: regular src + valid data clone attribs -> success; clone survives src destruction. */
static int test_data_src_data_dst_success(void) {
    int32_t src = create_ready_src_tag();
    if(src < 0) { return (int)src; }

    int32_t clone = plc_tag_create_from_tag(src, clone_tag_attribs, NULL, NULL, timeout_ms);
    if(clone < 0) {
        fprintf(stderr, "  FAIL: could not create clone tag: %s\n", plc_tag_decode_error((int)clone));
        plc_tag_destroy(src);
        return (int)clone;
    }

    int rc = wait_for_tag_ready(clone, timeout_ms);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "  FAIL: clone tag not ready: %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(clone);
        plc_tag_destroy(src);
        return rc;
    }

    /* Destroy source first — clone must keep working. */
    plc_tag_destroy(src);

    rc = plc_tag_read(clone, timeout_ms);
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        fprintf(stderr, "  FAIL: clone read failed after source destroyed: %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(clone);
        return rc;
    }

    if(rc == PLCTAG_STATUS_PENDING) {
        rc = wait_for_tag_ready(clone, timeout_ms);
        if(rc != PLCTAG_STATUS_OK) {
            fprintf(stderr, "  FAIL: clone did not recover from pending read: %s\n", plc_tag_decode_error(rc));
            plc_tag_destroy(clone);
            return rc;
        }
    }

    fprintf(stderr, "  test_data_src_data_dst_success: clone id=%d, read OK after source destroyed.\n", (int)clone);

    plc_tag_destroy(clone);
    return PLCTAG_STATUS_OK;
}

/* Test 11: create two clones from the same source — both must succeed and read independently. */
static int test_two_clones_from_same_src(void) {
    int32_t src = create_ready_src_tag();
    if(src < 0) { return (int)src; }

    int32_t clone1 = plc_tag_create_from_tag(src, clone_tag_attribs, NULL, NULL, timeout_ms);
    if(clone1 < 0) {
        fprintf(stderr, "  FAIL: could not create first clone: %s\n", plc_tag_decode_error((int)clone1));
        plc_tag_destroy(src);
        return (int)clone1;
    }

    int32_t clone2 = plc_tag_create_from_tag(src, clone_tag_attribs, NULL, NULL, timeout_ms);
    if(clone2 < 0) {
        fprintf(stderr, "  FAIL: could not create second clone: %s\n", plc_tag_decode_error((int)clone2));
        plc_tag_destroy(clone1);
        plc_tag_destroy(src);
        return (int)clone2;
    }

    int rc1 = wait_for_tag_ready(clone1, timeout_ms);
    int rc2 = wait_for_tag_ready(clone2, timeout_ms);

    plc_tag_destroy(src);

    if(rc1 != PLCTAG_STATUS_OK) {
        fprintf(stderr, "  FAIL: first clone not ready: %s\n", plc_tag_decode_error(rc1));
        plc_tag_destroy(clone1);
        plc_tag_destroy(clone2);
        return rc1;
    }

    if(rc2 != PLCTAG_STATUS_OK) {
        fprintf(stderr, "  FAIL: second clone not ready: %s\n", plc_tag_decode_error(rc2));
        plc_tag_destroy(clone1);
        plc_tag_destroy(clone2);
        return rc2;
    }

    int rc = plc_tag_read(clone1, timeout_ms);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "  FAIL: read on first clone failed: %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(clone1);
        plc_tag_destroy(clone2);
        return rc;
    }

    rc = plc_tag_read(clone2, timeout_ms);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "  FAIL: read on second clone failed: %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(clone1);
        plc_tag_destroy(clone2);
        return rc;
    }

    fprintf(stderr, "  test_two_clones_from_same_src: clone1=%d clone2=%d, both reads OK.\n", (int)clone1, (int)clone2);

    plc_tag_destroy(clone1);
    plc_tag_destroy(clone2);
    return PLCTAG_STATUS_OK;
}

/* Test 13: system tag (TAG_PROTOCOL_SYSTEM) as source -> ERR_NOT_ALLOWED.
 * Does not require a PLC — system tags work without a network connection. */
static int test_system_tag_source(void) {
    int32_t system_tag = plc_tag_create("make=system&family=library&name=version", 0);

    fprintf(stderr, "  test_system_tag_source: system_tag=%d\n", (int)system_tag);

    if(system_tag < 0) {
        fprintf(stderr, "  FAIL: could not create system tag: %s\n", plc_tag_decode_error((int)system_tag));
        return (int)system_tag;
    }

    int32_t rc = plc_tag_create_from_tag(system_tag, "name=whatever", NULL, NULL, 0);

    fprintf(stderr, "  test_system_tag_source: rc=%s (%d)\n", plc_tag_decode_error((int)rc), (int)rc);

    plc_tag_destroy(system_tag);

    if(rc >= 0) {
        plc_tag_destroy(rc);
        fprintf(stderr, "  FAIL: expected ERR_NOT_ALLOWED for system tag source\n");
        return PLCTAG_ERR_BAD_STATUS;
    }

    if(rc != PLCTAG_ERR_NOT_ALLOWED) {
        fprintf(stderr, "  FAIL: expected ERR_NOT_ALLOWED, got %s\n", plc_tag_decode_error((int)rc));
        return PLCTAG_ERR_BAD_STATUS;
    }

    return PLCTAG_STATUS_OK;
}

/* Test 12: reuse the handle of a tag that was already destroyed -> ERR_NOT_FOUND. */
static int test_destroyed_src_id_reused(void) {
    int32_t src = create_ready_src_tag();
    if(src < 0) { return (int)src; }

    int32_t saved_id = src;
    plc_tag_destroy(src);

    int32_t rc = plc_tag_create_from_tag(saved_id, clone_tag_attribs, NULL, NULL, 0);

    fprintf(stderr, "  test_destroyed_src_id_reused: saved_id=%d rc=%s (%d)\n", (int)saved_id, plc_tag_decode_error((int)rc),
            (int)rc);

    if(rc > 0) {
        plc_tag_destroy(rc);
        fprintf(stderr, "  FAIL: expected ERR_NOT_FOUND for a destroyed tag ID but got a valid handle.\n");
        return PLCTAG_ERR_BAD_STATUS;
    }

    if(rc != PLCTAG_ERR_NOT_FOUND) {
        fprintf(stderr, "  FAIL: expected ERR_NOT_FOUND, got %s\n", plc_tag_decode_error((int)rc));
        return PLCTAG_ERR_BAD_STATUS;
    }

    return PLCTAG_STATUS_OK;
}

/* Test 14: negative timeout -> ERR_BAD_PARAM. */
static int test_negative_timeout(void) {
    int32_t src = create_ready_src_tag();
    if(src < 0) { return (int)src; }

    int32_t rc = plc_tag_create_from_tag(src, clone_tag_attribs, NULL, NULL, -1);

    fprintf(stderr, "  test_negative_timeout: rc=%s (%d)\n", plc_tag_decode_error((int)rc), (int)rc);

    plc_tag_destroy(src);

    if(rc >= 0) {
        plc_tag_destroy(rc);
        fprintf(stderr, "  FAIL: expected ERR_BAD_PARAM for negative timeout\n");
        return PLCTAG_ERR_BAD_STATUS;
    }

    if(rc != PLCTAG_ERR_BAD_PARAM) {
        fprintf(stderr, "  FAIL: expected ERR_BAD_PARAM, got %s\n", plc_tag_decode_error((int)rc));
        return PLCTAG_ERR_BAD_STATUS;
    }

    return PLCTAG_STATUS_OK;
}

/* Test 15: clone of a clone shares the same underlying session. */
static int test_clone_of_clone(void) {
    int32_t src = create_ready_src_tag();
    if(src < 0) { return (int)src; }

    int32_t clone1 = plc_tag_create_from_tag(src, clone_tag_attribs, NULL, NULL, timeout_ms);
    if(clone1 < 0) {
        fprintf(stderr, "  FAIL: could not create first clone: %s\n", plc_tag_decode_error((int)clone1));
        plc_tag_destroy(src);
        return (int)clone1;
    }

    int rc = wait_for_tag_ready(clone1, timeout_ms);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "  FAIL: first clone not ready: %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(clone1);
        plc_tag_destroy(src);
        return rc;
    }

    /* Create a clone of the clone. */
    int32_t clone2 = plc_tag_create_from_tag(clone1, clone_tag_attribs, NULL, NULL, timeout_ms);
    plc_tag_destroy(src);

    if(clone2 < 0) {
        fprintf(stderr, "  FAIL: could not create clone of clone: %s\n", plc_tag_decode_error((int)clone2));
        plc_tag_destroy(clone1);
        return (int)clone2;
    }

    rc = wait_for_tag_ready(clone2, timeout_ms);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "  FAIL: clone of clone not ready: %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(clone1);
        plc_tag_destroy(clone2);
        return rc;
    }

    rc = plc_tag_read(clone1, timeout_ms);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "  FAIL: read on clone1 failed: %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(clone1);
        plc_tag_destroy(clone2);
        return rc;
    }

    rc = plc_tag_read(clone2, timeout_ms);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "  FAIL: read on clone2 (clone of clone) failed: %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(clone1);
        plc_tag_destroy(clone2);
        return rc;
    }

    fprintf(stderr, "  test_clone_of_clone: clone1=%d clone2=%d, both reads OK.\n", (int)clone1, (int)clone2);

    plc_tag_destroy(clone1);
    plc_tag_destroy(clone2);
    return PLCTAG_STATUS_OK;
}

/* Callback used by test 16. */
static void clone_event_callback(int32_t tag_id, int event, int status, void *userdata) {
    (void)tag_id;
    (void)status;
    (void)userdata;

    if(event == PLCTAG_EVENT_CREATED) { compat_atomic_store_int32(&clone_created_cb_fired, 1); }
    if(event == PLCTAG_EVENT_READ_COMPLETED) { compat_atomic_store_int32(&clone_read_completed_cb_fired, 1); }
}

/* Test 16: callback registered on clone receives CREATED and READ_COMPLETED. */
static int test_callback_fires_on_clone(void) {
    compat_atomic_store_int32(&clone_created_cb_fired, 0);
    compat_atomic_store_int32(&clone_read_completed_cb_fired, 0);

    int32_t src = create_ready_src_tag();
    if(src < 0) { return (int)src; }

    int32_t clone = plc_tag_create_from_tag(src, clone_tag_attribs, clone_event_callback, NULL, timeout_ms);
    plc_tag_destroy(src);

    if(clone < 0) {
        fprintf(stderr, "  FAIL: could not create clone with callback: %s\n", plc_tag_decode_error((int)clone));
        return (int)clone;
    }

    int rc = wait_for_tag_ready(clone, timeout_ms);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "  FAIL: clone not ready: %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(clone);
        return rc;
    }

    if(!compat_atomic_load_int32(&clone_created_cb_fired)) {
        fprintf(stderr, "  FAIL: PLCTAG_EVENT_CREATED did not fire on clone.\n");
        plc_tag_destroy(clone);
        return PLCTAG_ERR_BAD_STATUS;
    }

    rc = plc_tag_read(clone, timeout_ms);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "  FAIL: read on clone failed: %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(clone);
        return rc;
    }

    if(!compat_atomic_load_int32(&clone_read_completed_cb_fired)) {
        fprintf(stderr, "  FAIL: PLCTAG_EVENT_READ_COMPLETED did not fire on clone.\n");
        plc_tag_destroy(clone);
        return PLCTAG_ERR_BAD_STATUS;
    }

    plc_tag_destroy(clone);
    return PLCTAG_STATUS_OK;
}

/* Test 17: async creation (timeout=0) followed by polling to PLCTAG_STATUS_OK. */
static int test_async_create_and_poll(void) {
    int32_t src = create_ready_src_tag();
    if(src < 0) { return (int)src; }

    int32_t clone = plc_tag_create_from_tag(src, clone_tag_attribs, NULL, NULL, 0);
    plc_tag_destroy(src);

    if(clone < 0) {
        fprintf(stderr, "  FAIL: create_from_tag with timeout=0 returned error: %s\n", plc_tag_decode_error((int)clone));
        return (int)clone;
    }

    /* Poll until ready or timeout. */
    int rc = wait_for_tag_ready(clone, timeout_ms);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "  FAIL: clone not ready after polling %d ms: %s\n", timeout_ms, plc_tag_decode_error(rc));
        plc_tag_destroy(clone);
        return rc;
    }

    rc = plc_tag_read(clone, timeout_ms);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "  FAIL: read on async-created clone failed: %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(clone);
        return rc;
    }

    fprintf(stderr, "  test_async_create_and_poll: clone=%d, read OK after async creation.\n", (int)clone);

    plc_tag_destroy(clone);
    return PLCTAG_STATUS_OK;
}

/* -------------------------------------------------------------------------
 * Argument parsing
 * ---------------------------------------------------------------------- */

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [--src-tag=ATTRIBS] [--clone-attrib=ATTRIBS]\n"
            "          [--connection-tag=ATTRIBS] [--timeout=ms] [--debug=N]\n"
            "\n"
            "Tests 1-4 and 13 (invalid/system source IDs) always run without a PLC.\n"
            "Tests 5-12 and 14-17 run only when --src-tag, --clone-attrib, and\n"
            "--connection-tag are all provided.\n"
            "\n"
            "Example (AB/EIP with simulator):\n"
            "  %s \\\n"
            "    \"--src-tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray[0]\"\\\n"
            "    \"--clone-attrib=name=TestBigArray[1]&elem_count=1\" \\\n"
            "    \"--connection-tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=@connection\" \\\n"
            "    --timeout=5000\n",
            prog, prog);
}

static void parse_args(int argc, char **argv) {
    int i = 0;

    for(i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--src-tag=", 10) == 0) {
            src_tag_attribs = &argv[i][10];
        } else if(strncmp(argv[i], "--clone-attrib=", 15) == 0) {
            clone_tag_attribs = &argv[i][15];
        } else if(strncmp(argv[i], "--connection-tag=", 17) == 0) {
            connection_tag_attribs = &argv[i][17];
        } else if(strncmp(argv[i], "--timeout=", 10) == 0) {
            timeout_ms = atoi(&argv[i][10]);
            if(timeout_ms <= 0) { timeout_ms = DEFAULT_TIMEOUT_MS; }
        } else if(strncmp(argv[i], "--debug=", 8) == 0) {
            plc_tag_set_debug_level(atoi(&argv[i][8]));
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            exit(1);
        }
    }
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

#define RUN_TEST(label, fn)                                                              \
    do {                                                                                 \
        int _rc = (fn);                                                                  \
        total++;                                                                         \
        if(_rc == PLCTAG_STATUS_OK) {                                                    \
            passed++;                                                                    \
            fprintf(stderr, "  PASS: %s\n", (label));                                    \
        } else {                                                                         \
            failed++;                                                                    \
            fprintf(stderr, "  FAIL: %s (rc=%s)\n", (label), plc_tag_decode_error(_rc)); \
        }                                                                                \
    } while(0)

int main(int argc, char **argv) {
    int total = 0, passed = 0, failed = 0;
    bool have_plc_args = false;

    int version_major = plc_tag_get_int_attribute(0, "version_major", 0);
    int version_minor = plc_tag_get_int_attribute(0, "version_minor", 0);
    int version_patch = plc_tag_get_int_attribute(0, "version_patch", 0);

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required library version %d.%d.%d not available, found %d.%d.%d!\n", REQUIRED_VERSION, version_major,
                version_minor, version_patch);
        return 1;
    }

    fprintf(stderr, "Library version %d.%d.%d.\n", version_major, version_minor, version_patch);

    parse_args(argc, argv);

    have_plc_args = (src_tag_attribs != NULL) && (clone_tag_attribs != NULL) && (connection_tag_attribs != NULL);

    /* --- No-PLC tests (always run) --- */
    fprintf(stderr, "\n-- No-PLC tests (invalid/system source IDs) --\n");
    RUN_TEST("1.  src_tag_id = INT32_MAX", test_invalid_src_id_max());
    RUN_TEST("2.  src_tag_id = 0", test_invalid_src_id_zero());
    RUN_TEST("3.  src_tag_id = -1", test_invalid_src_id_minus_one());
    RUN_TEST("4.  src_tag_id = INT32_MIN", test_invalid_src_id_min());
    RUN_TEST("13. system tag source -> ERR_NOT_ALLOWED", test_system_tag_source());

    /* --- With-PLC tests --- */
    if(!have_plc_args) {
        fprintf(stderr, "\nSkipping tests 5-12 and 14-17: --src-tag, --clone-attrib, and\n"
                        "--connection-tag are all required.\n"
                        "Re-run with PLC arguments to execute the full test suite.\n");
    } else {
        fprintf(stderr, "\n-- With-PLC tests --\n");
        RUN_TEST("5.  valid src, attrib_str = NULL", test_null_attrib_str());
        RUN_TEST("6.  valid src, attrib_str = \"\"", test_empty_attrib_str());
        RUN_TEST("7.  @connection src + data clone attribs", test_device_src_data_dst());
        RUN_TEST("8.  @connection src + @connection clone attribs", test_device_src_device_dst());
        RUN_TEST("9.  regular src + @connection clone attribs", test_data_src_device_dst());
        RUN_TEST("10. regular src + data clone, src destroyed", test_data_src_data_dst_success());
        RUN_TEST("11. two clones from same src", test_two_clones_from_same_src());
        RUN_TEST("12. destroyed src ID reused", test_destroyed_src_id_reused());
        RUN_TEST("14. negative timeout -> ERR_BAD_PARAM", test_negative_timeout());
        RUN_TEST("15. clone of clone -> success", test_clone_of_clone());
        RUN_TEST("16. callback fires on clone (CREATED + READ_COMPLETED)", test_callback_fires_on_clone());
        RUN_TEST("17. async create (timeout=0) + poll -> success", test_async_create_and_poll());
    }

    fprintf(stderr, "\nRESULT: %d/%d passed", passed, total);
    if(failed > 0) {
        fprintf(stderr, ", %d FAILED\n", failed);
        fprintf(stderr, "RESULT: FAIL\n");
        return 1;
    }
    fprintf(stderr, "\nRESULT: PASS\n");
    return 0;
}
